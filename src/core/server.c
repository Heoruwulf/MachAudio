#include "machaudio/server.h"
#include "machaudio/log.h"
#include "machaudio/protocol.h"

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define QD 256

struct buf_ring_context *init_registered_buffer_ring(struct io_uring *ring, int bgid) {
    struct buf_ring_context *ctx = calloc(1, sizeof(struct buf_ring_context));
    if (!ctx)
        return NULL;

    size_t ring_size = NUM_BUFFERS * sizeof(struct io_uring_buf);
    size_t data_size = NUM_BUFFERS * BUF_SIZE;

    posix_memalign(&ctx->buf_mem, 4096, data_size);
    posix_memalign((void **)&ctx->br, 4096, ring_size);

    if (!ctx->buf_mem || !ctx->br) {
        free(ctx->buf_mem);
        free(ctx->br);
        free(ctx);
        return NULL;
    }

    struct io_uring_buf_reg reg = {
        .ring_addr    = (uint64_t)ctx->br,
        .ring_entries = NUM_BUFFERS,
        .bgid         = bgid,
    };

    int ret = io_uring_register_buf_ring(ring, &reg, 0);
    if (ret < 0) {
        free(ctx->buf_mem);
        free(ctx->br);
        free(ctx);
        return NULL;
    }

    io_uring_buf_ring_init(ctx->br);
    for (int i = 0; i < NUM_BUFFERS; i++) {
        void *buf_addr = (uint8_t *)ctx->buf_mem + (i * BUF_SIZE);
        io_uring_buf_ring_add(
            ctx->br,
            buf_addr,
            BUF_SIZE,
            i,
            io_uring_buf_ring_mask(NUM_BUFFERS),
            i);
    }

    io_uring_buf_ring_advance(ctx->br, NUM_BUFFERS);
    ctx->bgid = bgid;

    return ctx;
}

static void cleanup_session_if_needed(MachSession *session) {
    if (session->closing && session->pending_writes == 0) {
        close(session->client_fd);
        transcode_session_stop(&session->transcode_session);
        audio_engine_destroy(&session->audio_engine);
        free(session);
    }
}

static void free_write_response(WriteResponse *wr) {
    if (wr) {
        if (wr->is_dynamic)
            free(wr);
        else
            wr->in_use = false;
    }
}

static WriteResponse *get_write_response(MachSession *session, size_t resp_len) {
    if (resp_len > MAX_WRITE_PAYLOAD) {
        LOGERR("Response length %zu exceeds maximum %d", resp_len, MAX_WRITE_PAYLOAD);
        return NULL;
    }
    for (int i = 0; i < MAX_CONCURRENT_WRITES; i++) {
        if (!session->write_pool[i].in_use) {
            session->write_pool[i].in_use     = true;
            session->write_pool[i].is_dynamic = false;
            return &session->write_pool[i];
        }
    }
    LOGDBG("Write pool exhausted for session, falling back to malloc");
    WriteResponse *wr = malloc(sizeof(WriteResponse));
    if (wr) {
        wr->in_use     = true;
        wr->is_dynamic = true;
        return wr;
    }
    return NULL;
}

static void send_error_uring(MachSession *session, uint32_t sequence_id, AudioErrorCode code) {
    if (session->closing)
        return;

    size_t const resp_len = sizeof(AudioMsgHeader) + sizeof(struct audio_error_payload);

    WriteResponse *wr = get_write_response(session, resp_len);
    if (!wr)
        return;

    wr->req.op  = IO_OP_WRITE;
    wr->req.fd  = session->client_fd;
    wr->req.ctx = wr;
    wr->session = session;
    wr->len     = resp_len;

    AudioMsgHeader *const resp_header = (AudioMsgHeader *)wr->data;
    resp_header->magic                = htonl(AUDIO_MAGIC);
    resp_header->version              = htons(AUDIO_VERSION);
    resp_header->command              = htons(CMD_ERROR);
    resp_header->sequence_id          = htonl(sequence_id);
    resp_header->payload_len          = htonl((uint32_t)sizeof(struct audio_error_payload));

    struct audio_error_payload *const resp_payload =
        (struct audio_error_payload *)(wr->data + sizeof(AudioMsgHeader));
    resp_payload->error_code = htonl((uint32_t)code);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&session->server->ring);
    if (sqe) {
        io_uring_prep_send(sqe, session->client_fd, wr->data, resp_len, 0);
        io_uring_sqe_set_data(sqe, &wr->req);
        session->pending_writes++;
        io_uring_submit(&session->server->ring);
    } else {
        free_write_response(wr);
    }
}

static void process_client_read(MachSession *session, int nread, void *buf_base, int bid) {
    int offset = 0;

    while (nread - offset >= (int)sizeof(AudioMsgHeader)) {
        AudioMsgHeader *header  = (AudioMsgHeader *)((uint8_t *)buf_base + offset);
        AudioMsgHeader  decoded = *header;
        protocol_decode_header(&decoded);

        if (!protocol_validate_header(&decoded)) {
            LOGERR("Invalid protocol header received");
            send_error_uring(session, decoded.sequence_id, ERR_INVALID_MAGIC);
            break;
        }

        if (nread - offset < (int)(sizeof(AudioMsgHeader) + decoded.payload_len)) {
            // Partial message, cannot process (buffer boundary split)
            break;
        }

        void *payload = (uint8_t *)buf_base + offset + sizeof(AudioMsgHeader);

        switch (decoded.command) {
        case CMD_START: {
            LOGINF("Received CMD_START");
            if (decoded.payload_len < sizeof(struct audio_start_payload)) {
                send_error_uring(session, decoded.sequence_id, ERR_INVALID_PAYLOAD);
                break;
            }
            struct audio_start_payload const *const config =
                (struct audio_start_payload const *)payload;

            transcode_session_init(&session->transcode_session, config);

            audio_engine_destroy(&session->audio_engine);
            audio_engine_init(
                &session->audio_engine,
                (int)ntohl(config->in_sample_rate),
                (int)config->in_channels);

            if (session->transcode_session.vad_enabled) {
                arena_reset(&session->arena);
                session->transcode_session.vad_state = vad_gru_init(&session->arena);
                session->arena_curr_start            = session->arena.curr;
            } else {
                arena_reset(&session->arena);
                session->arena_curr_start = 0;
            }
            break;
        }

        case CMD_INPUT: {
            if (decoded.payload_len < sizeof(struct audio_input_payload)) {
                send_error_uring(session, decoded.sequence_id, ERR_INVALID_PAYLOAD);
                break;
            }

            session->arena.curr = session->arena_curr_start;

            uint64_t const start_ns = mach_hrtime();

            int r = audio_process_transcode(
                &session->transcode_session,
                (struct audio_input_payload const *)payload,
                decoded.payload_len,
                &session->arena);

            uint64_t const end_ns      = mach_hrtime();
            uint64_t const duration_ns = end_ns - start_ns;

            if (r == 0) {
                size_t const out_data_len = arena_used(&session->arena) - session->arena_curr_start;
                size_t const payload_len  = sizeof(struct audio_output_payload) + out_data_len;
                size_t const resp_len     = sizeof(AudioMsgHeader) + payload_len;

                if (session->closing) {
                    break;
                }

                WriteResponse *wr = get_write_response(session, resp_len);
                if (wr) {
                    wr->req.op  = IO_OP_WRITE;
                    wr->req.fd  = session->client_fd;
                    wr->req.ctx = wr;
                    wr->session = session;
                    wr->len     = resp_len;

                    AudioMsgHeader *const resp_header = (AudioMsgHeader *)wr->data;
                    resp_header->magic                = htonl(AUDIO_MAGIC);
                    resp_header->version              = htons(AUDIO_VERSION);
                    resp_header->command              = htons(CMD_OUTPUT);
                    resp_header->sequence_id          = htonl(decoded.sequence_id);
                    resp_header->payload_len          = htonl((uint32_t)payload_len);

                    struct audio_output_payload *const resp_payload =
                        (struct audio_output_payload *)(wr->data + sizeof(AudioMsgHeader));
                    resp_payload->duration_ns = htobe64(duration_ns);

                    uint32_t const prob_net =
                        protocol_float_to_net(session->transcode_session.last_vad_prob);
                    memcpy(&resp_payload->vad_prob, &prob_net, sizeof(float));

                    memset(resp_payload->reserved, 0, sizeof(resp_payload->reserved));
                    memcpy(
                        resp_payload->data,
                        session->arena.buf + session->arena_curr_start,
                        out_data_len);

                    struct io_uring_sqe *sqe = io_uring_get_sqe(&session->server->ring);
                    if (sqe) {
                        io_uring_prep_send(sqe, session->client_fd, wr->data, resp_len, 0);
                        io_uring_sqe_set_data(sqe, &wr->req);
                        session->pending_writes++;
                        io_uring_submit(&session->server->ring);
                    } else {
                        free_write_response(wr);
                    }
                }

                session->arena.curr = session->arena_curr_start;
            } else {
                LOGERR("Transcode processing failed");
                send_error_uring(session, decoded.sequence_id, ERR_PROCESSING_FAILED);
            }
            break;
        }

        case CMD_STOP:
            LOGINF("Received CMD_STOP");
            transcode_session_stop(&session->transcode_session);
            audio_engine_destroy(&session->audio_engine);
            break;

        case CMD_PING: {
            if (session->closing)
                break;
            size_t const resp_len = sizeof(AudioMsgHeader);

            WriteResponse *wr = get_write_response(session, resp_len);
            if (wr) {
                wr->req.op  = IO_OP_WRITE;
                wr->req.fd  = session->client_fd;
                wr->req.ctx = wr;
                wr->session = session;
                wr->len     = resp_len;

                AudioMsgHeader *const resp_header = (AudioMsgHeader *)wr->data;
                resp_header->magic                = htonl(AUDIO_MAGIC);
                resp_header->version              = htons(AUDIO_VERSION);
                resp_header->command              = htons(CMD_PONG);
                resp_header->sequence_id          = htonl(decoded.sequence_id);
                resp_header->payload_len          = 0;

                struct io_uring_sqe *sqe = io_uring_get_sqe(&session->server->ring);
                if (sqe) {
                    io_uring_prep_send(sqe, session->client_fd, wr->data, resp_len, 0);
                    io_uring_sqe_set_data(sqe, &wr->req);
                    session->pending_writes++;
                    io_uring_submit(&session->server->ring);
                } else {
                    free_write_response(wr);
                }
            }
            break;
        }

        case CMD_DISCOVER: {
            LOGINF("Received CMD_DISCOVER");
            if (session->closing)
                break;
            size_t const resp_len =
                sizeof(AudioMsgHeader) + sizeof(struct audio_discover_reply_payload);

            WriteResponse *wr = get_write_response(session, resp_len);
            if (wr) {
                wr->req.op  = IO_OP_WRITE;
                wr->req.fd  = session->client_fd;
                wr->req.ctx = wr;
                wr->session = session;
                wr->len     = resp_len;

                AudioMsgHeader *const resp_header = (AudioMsgHeader *)wr->data;
                resp_header->magic                = htonl(AUDIO_MAGIC);
                resp_header->version              = htons(AUDIO_VERSION);
                resp_header->command              = htons(CMD_DISCOVER_REPLY);
                resp_header->sequence_id          = htonl(decoded.sequence_id);
                resp_header->payload_len =
                    htonl((uint32_t)sizeof(struct audio_discover_reply_payload));

                struct audio_discover_reply_payload *const resp_payload =
                    (struct audio_discover_reply_payload *)(wr->data + sizeof(AudioMsgHeader));
                resp_payload->num_workers = htonl(session->server->num_workers);
                resp_payload->reserved    = 0;

                struct io_uring_sqe *sqe = io_uring_get_sqe(&session->server->ring);
                if (sqe) {
                    io_uring_prep_send(sqe, session->client_fd, wr->data, resp_len, 0);
                    io_uring_sqe_set_data(sqe, &wr->req);
                    session->pending_writes++;
                    io_uring_submit(&session->server->ring);
                } else {
                    free_write_response(wr);
                }
            }
            break;
        }

        default:
            LOGERR("Unknown command: %d", decoded.command);
            send_error_uring(session, decoded.sequence_id, ERR_INVALID_COMMAND);
            break;
        }

        offset += sizeof(AudioMsgHeader) + decoded.payload_len;
    }

    io_uring_buf_ring_add(
        session->server->buf_ring->br,
        buf_base,
        BUF_SIZE,
        bid,
        io_uring_buf_ring_mask(NUM_BUFFERS),
        0);
    io_uring_buf_ring_advance(session->server->buf_ring->br, 1);
}

int mach_server_init(
    MachServer *const server,
    char const *const socket_path,
    char const *const host,
    int const         port,
    uint32_t const    num_workers,
    int const         sq_thread_cpu) {
    if (server == NULL) {
        return -1;
    }
    memset(server, 0, sizeof(MachServer));
    server->socket_path = socket_path;
    server->host        = host;
    server->port        = port;
    server->is_tcp      = (host != NULL);
    server->num_workers = num_workers;
    server->running     = false;

    // Create the listening socket
    int fd;
    if (server->is_tcp) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            LOGERR("Failed to create TCP socket: %s", strerror(errno));
            return -1;
        }

        int opt = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            LOGERR("Failed to set SO_REUSEADDR: %s", strerror(errno));
            close(fd);
            return -1;
        }

        if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
            LOGERR("Failed to set TCP_NODELAY: %s", strerror(errno));
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
            LOGERR("Invalid IP address: %s", host);
            close(fd);
            return -1;
        }

        if (bind(fd, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
            LOGERR("Failed to bind TCP socket: %s", strerror(errno));
            close(fd);
            return -1;
        }
    } else {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            LOGERR("Failed to create UDS socket: %s", strerror(errno));
            return -1;
        }

        if (socket_path) {
            unlink(socket_path); // Ensure path is clear
            struct sockaddr_un addr;
            memset(&addr, 0, sizeof(addr));
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

            if (bind(fd, (struct sockaddr const *)&addr, sizeof(addr)) < 0) {
                LOGERR("Failed to bind UDS socket to %s: %s", socket_path, strerror(errno));
                close(fd);
                return -1;
            }

            if (chmod(socket_path, 0666) < 0) {
                LOGERR("Failed to chmod UDS socket: %s", strerror(errno));
                close(fd);
                return -1;
            }
        }
    }

    // Set socket non-blocking
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    server->listen_fd = fd;

    // Initialize io_uring
    struct io_uring_params params;
    memset(&params, 0, sizeof(params));
    params.flags         = IORING_SETUP_SQPOLL | IORING_SETUP_SQ_AFF;
    params.sq_thread_cpu = sq_thread_cpu;
    int r                = io_uring_queue_init_params(QD, &server->ring, &params);
    if (r == -EINVAL) {
        LOGINF("SQPOLL invalid params, falling back to standard io_uring");
        memset(&params, 0, sizeof(params));
        r = io_uring_queue_init_params(QD, &server->ring, &params);
    }
    if (r < 0) {
        LOGERR("Failed to initialize io_uring queue: %s", strerror(-r));
        close(fd);
        return -1;
    }

    server->buf_ring = init_registered_buffer_ring(&server->ring, BGID_READ_RING);
    if (!server->buf_ring) {
        LOGERR("Failed to initialize buffer ring");
        io_uring_queue_exit(&server->ring);
        close(fd);
        return -1;
    }

    // Initialize signalfd for unified event loop signals
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
        LOGERR("Failed to block signals: %s", strerror(errno));
        io_uring_queue_exit(&server->ring);
        close(fd);
        return -1;
    }

    int sig_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sig_fd < 0) {
        LOGERR("Failed to create signalfd: %s", strerror(errno));
        io_uring_queue_exit(&server->ring);
        close(fd);
        return -1;
    }
    server->signal_fd = sig_fd;

    // Set up request tokens
    server->accept_req.op  = IO_OP_ACCEPT;
    server->accept_req.fd  = server->listen_fd;
    server->accept_req.ctx = server;

    server->signal_req.op  = IO_OP_SIGNAL;
    server->signal_req.fd  = server->signal_fd;
    server->signal_req.ctx = server;

    return 0;
}

int mach_server_start(MachServer *const server) {
    if (server == NULL) {
        return -1;
    }

    if (listen(server->listen_fd, SOMAXCONN) < 0) {
        LOGERR("Listen error: %s", strerror(errno));
        return -1;
    }

    if (server->is_tcp) {
        LOGINF("MachAudio server listening on %s:%d (io_uring)", server->host, server->port);
    } else {
        LOGINF("MachAudio server listening on %s (io_uring)", server->socket_path);
    }

    // Submit initial accept
    struct io_uring_sqe *sqe = io_uring_get_sqe(&server->ring);
    if (!sqe) {
        LOGERR("Failed to get SQE for accept");
        return -1;
    }
    io_uring_prep_accept(sqe, server->listen_fd, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, &server->accept_req);

    // Submit initial signal read
    sqe = io_uring_get_sqe(&server->ring);
    if (!sqe) {
        LOGERR("Failed to get SQE for signal");
        return -1;
    }
    static struct signalfd_siginfo sig_info;
    io_uring_prep_read(sqe, server->signal_fd, &sig_info, sizeof(sig_info), 0);
    io_uring_sqe_set_data(sqe, &server->signal_req);

    int r = io_uring_submit(&server->ring);
    if (r < 0) {
        LOGERR("io_uring_submit failed: %s", strerror(-r));
        return -1;
    }

    server->running = true;

    while (server->running) {
        struct io_uring_cqe *cqe;
        int                  ret = io_uring_wait_cqe(&server->ring, &cqe);
        if (ret < 0) {
            if (ret == -EINTR)
                continue;
            LOGERR("io_uring_wait_cqe failed: %s", strerror(-ret));
            break;
        }

        struct io_uring_cqe *cqes[64]      = {0};
        unsigned int         cqe_count     = io_uring_peek_batch_cqe(&server->ring, cqes, 64);
        unsigned int         advance_count = 0;

        for (unsigned int i = 0; i < cqe_count; i++) {
            struct io_uring_cqe *c = cqes[i];
            advance_count++;

            IoRequest *req = (IoRequest *)io_uring_cqe_get_data(c);
            if (!req)
                continue;

            int res = c->res;

            switch (req->op) {
            case IO_OP_SIGNAL: {
                if (res >= 0) {
                    LOGINF("Received signal, shutting down...");
                    mach_server_stop(server);
                } else {
                    LOGERR("Signalfd read error: %d", res);
                    mach_server_stop(server);
                }
                break;
            }

            case IO_OP_ACCEPT: {
                if (res < 0) {
                    if (res != -EAGAIN && res != -EWOULDBLOCK && res != -ECANCELED) {
                        LOGERR("Accept error: %s", strerror(-res));
                    }
                } else {
                    int          client_fd = res;
                    MachSession *session   = NULL;
                    if (posix_memalign((void **)&session, 4096, sizeof(MachSession)) != 0) {
                        session = NULL;
                    }
                    if (session == NULL) {
                        LOGERR("Failed to allocate MachSession");
                        close(client_fd);
                    } else {
                        memset(session, 0, sizeof(MachSession));
                        session->client_fd      = client_fd;
                        session->is_tcp         = server->is_tcp;
                        session->server         = server;
                        session->pending_writes = 0;
                        session->closing        = false;
                        arena_init(
                            &session->arena,
                            session->arena_buf,
                            sizeof(session->arena_buf),
                            "session");

                        session->read_req.op  = IO_OP_READ;
                        session->read_req.fd  = client_fd;
                        session->read_req.ctx = session;

                        LOGINF("New client connected");

                        // Submit first read
                        struct io_uring_sqe *c_sqe = io_uring_get_sqe(&server->ring);
                        if (c_sqe) {
                            io_uring_prep_recv_multishot(c_sqe, client_fd, NULL, 0, 0);
                            c_sqe->buf_group = BGID_READ_RING;
                            c_sqe->flags |= IOSQE_BUFFER_SELECT;
                            io_uring_sqe_set_data(c_sqe, &session->read_req);
                        }
                    }
                }

                // Re-submit accept if we are running
                if (server->running) {
                    struct io_uring_sqe *a_sqe = io_uring_get_sqe(&server->ring);
                    if (a_sqe) {
                        io_uring_prep_accept(a_sqe, server->listen_fd, NULL, NULL, 0);
                        io_uring_sqe_set_data(a_sqe, &server->accept_req);
                    }
                }
                break;
            }

            case IO_OP_READ: {
                MachSession *session = (MachSession *)req->ctx;

                bool  has_buffer     = (c->flags & IORING_CQE_F_BUFFER) != 0;
                int   bid            = -1;
                void *buffer_address = NULL;
                if (has_buffer) {
                    bid            = c->flags >> IORING_CQE_BUFFER_SHIFT;
                    buffer_address = (uint8_t *)server->buf_ring->buf_mem + (bid * BUF_SIZE);
                }

                if (res <= 0) {
                    if (has_buffer) {
                        io_uring_buf_ring_add(
                            server->buf_ring->br,
                            buffer_address,
                            BUF_SIZE,
                            bid,
                            io_uring_buf_ring_mask(NUM_BUFFERS),
                            0);
                        io_uring_buf_ring_advance(server->buf_ring->br, 1);
                    }

                    if (res < 0 && res != -ECONNRESET && res != -EPIPE && res != -ENOBUFS) {
                        LOGERR("Read error: %s", strerror(-res));
                    }
                    if (res == -ENOBUFS) {
                        // Silent resubmission since this is normal under heavy stream load
                        struct io_uring_sqe *r_sqe = io_uring_get_sqe(&server->ring);
                        if (r_sqe) {
                            io_uring_prep_recv_multishot(r_sqe, session->client_fd, NULL, 0, 0);
                            r_sqe->buf_group = BGID_READ_RING;
                            r_sqe->flags |= IOSQE_BUFFER_SELECT;
                            io_uring_sqe_set_data(r_sqe, &session->read_req);
                        }
                    } else {
                        LOGINF("Client disconnected");
                        session->closing = true;
                        cleanup_session_if_needed(session);
                    }
                } else {
                    if (has_buffer) {
                        process_client_read(session, res, buffer_address, bid);
                    } else {
                        LOGERR("Read succeeded but no buffer was selected!");
                    }

                    if (!(c->flags & IORING_CQE_F_MORE)) {
                        struct io_uring_sqe *r_sqe = io_uring_get_sqe(&server->ring);
                        if (r_sqe) {
                            io_uring_prep_recv_multishot(r_sqe, session->client_fd, NULL, 0, 0);
                            r_sqe->buf_group = BGID_READ_RING;
                            r_sqe->flags |= IOSQE_BUFFER_SELECT;
                            io_uring_sqe_set_data(r_sqe, &session->read_req);
                        }
                    }
                }
                break;
            }

            case IO_OP_WRITE: {
                WriteResponse *wr      = (WriteResponse *)req;
                MachSession   *session = wr->session;
                session->pending_writes--;

                if (res < 0) {
                    if (!session->closing) {
                        if (res != -EPIPE && res != -ECONNRESET) {
                            LOGERR("Write error: %s", strerror(-res));
                        }
                        LOGINF("Closing connection due to write error");
                        session->closing = true;
                    }
                }

                free_write_response(wr);
                cleanup_session_if_needed(session);
                break;
            }
            }
        }

        if (advance_count > 0) {
            io_uring_cq_advance(&server->ring, advance_count);
            io_uring_submit(&server->ring);
        }
    }

    // Cleanup resources upon exit
    close(server->signal_fd);
    close(server->listen_fd);
    io_uring_queue_exit(&server->ring);

    if (server->buf_ring) {
        free(server->buf_ring->buf_mem);
        free(server->buf_ring->br);
        free(server->buf_ring);
    }

    if (!server->is_tcp && server->socket_path) {
        unlink(server->socket_path);
    }

    return 0;
}

void mach_server_stop(MachServer *const server) {
    if (server == NULL) {
        return;
    }
    server->running = false;
}
