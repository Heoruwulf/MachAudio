#include "machaudio/server.h"
#include "machaudio/log.h"
#include "machaudio/protocol.h"

#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
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

static void send_error_uring(MachSession *session, uint32_t sequence_id, AudioErrorCode code) {
    size_t const resp_len = sizeof(AudioMsgHeader) + sizeof(struct audio_error_payload);
    if (resp_len > sizeof(session->write_buf))
        return;

    AudioMsgHeader *const resp_header = (AudioMsgHeader *)session->write_buf;
    resp_header->magic                = htonl(AUDIO_MAGIC);
    resp_header->version              = htons(AUDIO_VERSION);
    resp_header->command              = htons(CMD_ERROR);
    resp_header->sequence_id          = htonl(sequence_id);
    resp_header->payload_len          = htonl((uint32_t)sizeof(struct audio_error_payload));

    struct audio_error_payload *const resp_payload =
        (struct audio_error_payload *)(session->write_buf + sizeof(AudioMsgHeader));
    resp_payload->error_code = htonl((uint32_t)code);

    struct io_uring_sqe *sqe = io_uring_get_sqe(&session->server->ring);
    if (sqe) {
        io_uring_prep_send(sqe, session->client_fd, session->write_buf, resp_len, 0);
        io_uring_sqe_set_data(sqe, &session->write_req);
        session->write_pending = true;
        io_uring_submit(&session->server->ring);
    }
}

static void process_client_read(MachSession *session, int nread) {
    uint8_t *buf_base = session->read_buf;

    if (nread < (int)sizeof(AudioMsgHeader)) {
        goto reschedule_read;
    }

    AudioMsgHeader *header  = (AudioMsgHeader *)buf_base;
    AudioMsgHeader  decoded = *header;
    protocol_decode_header(&decoded);

    if (!protocol_validate_header(&decoded)) {
        LOGERR("Invalid protocol header received");
        send_error_uring(session, decoded.sequence_id, ERR_INVALID_MAGIC);
        return;
    }

    if (nread < (int)(sizeof(AudioMsgHeader) + decoded.payload_len)) {
        goto reschedule_read;
    }

    void *payload = buf_base + sizeof(AudioMsgHeader);

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

        // Reschedule next read immediately since no write response is triggered
        struct io_uring_sqe *r_sqe = io_uring_get_sqe(&session->server->ring);
        if (r_sqe) {
            io_uring_prep_recv(
                r_sqe,
                session->client_fd,
                session->read_buf,
                sizeof(session->read_buf),
                0);
            io_uring_sqe_set_data(r_sqe, &session->read_req);
            io_uring_submit(&session->server->ring);
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

            if (resp_len > sizeof(session->write_buf)) {
                LOGERR("Response too large for session write buffer");
                send_error_uring(session, decoded.sequence_id, ERR_PROCESSING_FAILED);
                break;
            }

            AudioMsgHeader *const resp_header = (AudioMsgHeader *)session->write_buf;
            resp_header->magic                = htonl(AUDIO_MAGIC);
            resp_header->version              = htons(AUDIO_VERSION);
            resp_header->command              = htons(CMD_OUTPUT);
            resp_header->sequence_id          = htonl(decoded.sequence_id);
            resp_header->payload_len          = htonl((uint32_t)payload_len);

            struct audio_output_payload *const resp_payload =
                (struct audio_output_payload *)(session->write_buf + sizeof(AudioMsgHeader));
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
                io_uring_prep_send(sqe, session->client_fd, session->write_buf, resp_len, 0);
                io_uring_sqe_set_data(sqe, &session->write_req);
                session->write_pending = true;
                io_uring_submit(&session->server->ring);
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

        // Reschedule next read immediately
        struct io_uring_sqe *r_sqe = io_uring_get_sqe(&session->server->ring);
        if (r_sqe) {
            io_uring_prep_recv(
                r_sqe,
                session->client_fd,
                session->read_buf,
                sizeof(session->read_buf),
                0);
            io_uring_sqe_set_data(r_sqe, &session->read_req);
            io_uring_submit(&session->server->ring);
        }
        break;

    case CMD_PING: {
        size_t const          resp_len    = sizeof(AudioMsgHeader);
        AudioMsgHeader *const resp_header = (AudioMsgHeader *)session->write_buf;
        resp_header->magic                = htonl(AUDIO_MAGIC);
        resp_header->version              = htons(AUDIO_VERSION);
        resp_header->command              = htons(CMD_PONG);
        resp_header->sequence_id          = htonl(decoded.sequence_id);
        resp_header->payload_len          = 0;

        struct io_uring_sqe *sqe = io_uring_get_sqe(&session->server->ring);
        if (sqe) {
            io_uring_prep_send(sqe, session->client_fd, session->write_buf, resp_len, 0);
            io_uring_sqe_set_data(sqe, &session->write_req);
            session->write_pending = true;
            io_uring_submit(&session->server->ring);
        }
        break;
    }

    case CMD_DISCOVER: {
        LOGINF("Received CMD_DISCOVER");
        size_t const resp_len =
            sizeof(AudioMsgHeader) + sizeof(struct audio_discover_reply_payload);

        AudioMsgHeader *const resp_header = (AudioMsgHeader *)session->write_buf;
        resp_header->magic                = htonl(AUDIO_MAGIC);
        resp_header->version              = htons(AUDIO_VERSION);
        resp_header->command              = htons(CMD_DISCOVER_REPLY);
        resp_header->sequence_id          = htonl(decoded.sequence_id);
        resp_header->payload_len = htonl((uint32_t)sizeof(struct audio_discover_reply_payload));

        struct audio_discover_reply_payload *const resp_payload =
            (struct audio_discover_reply_payload *)(session->write_buf + sizeof(AudioMsgHeader));
        resp_payload->num_workers = htonl(session->server->num_workers);
        resp_payload->reserved    = 0;

        struct io_uring_sqe *sqe = io_uring_get_sqe(&session->server->ring);
        if (sqe) {
            io_uring_prep_send(sqe, session->client_fd, session->write_buf, resp_len, 0);
            io_uring_sqe_set_data(sqe, &session->write_req);
            session->write_pending = true;
            io_uring_submit(&session->server->ring);
        }
        break;
    }

    default:
        LOGERR("Unknown command: %d", decoded.command);
        send_error_uring(session, decoded.sequence_id, ERR_INVALID_COMMAND);
        break;
    }
    return;

reschedule_read:;
    struct io_uring_sqe *r_sqe = io_uring_get_sqe(&session->server->ring);
    if (r_sqe) {
        io_uring_prep_recv(
            r_sqe,
            session->client_fd,
            session->read_buf,
            sizeof(session->read_buf),
            0);
        io_uring_sqe_set_data(r_sqe, &session->read_req);
        io_uring_submit(&session->server->ring);
    }
}

int mach_server_init(
    MachServer *const server,
    char const *const socket_path,
    char const *const host,
    int const         port,
    uint32_t const    num_workers) {
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
    int r = io_uring_queue_init_params(QD, &server->ring, &params);
    if (r < 0) {
        LOGERR("Failed to initialize io_uring queue: %s", strerror(-r));
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
                    int                client_fd = res;
                    MachSession *const session   = calloc(1, sizeof(MachSession));
                    if (session == NULL) {
                        LOGERR("Failed to allocate MachSession");
                        close(client_fd);
                    } else {
                        session->client_fd     = client_fd;
                        session->is_tcp        = server->is_tcp;
                        session->server        = server;
                        session->write_pending = false;
                        arena_init(
                            &session->arena,
                            session->arena_buf,
                            sizeof(session->arena_buf),
                            "session");

                        session->read_req.op  = IO_OP_READ;
                        session->read_req.fd  = client_fd;
                        session->read_req.ctx = session;

                        session->write_req.op  = IO_OP_WRITE;
                        session->write_req.fd  = client_fd;
                        session->write_req.ctx = session;

                        LOGINF("New client connected");

                        // Submit first read
                        struct io_uring_sqe *c_sqe = io_uring_get_sqe(&server->ring);
                        if (c_sqe) {
                            io_uring_prep_recv(
                                c_sqe,
                                client_fd,
                                session->read_buf,
                                sizeof(session->read_buf),
                                0);
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
                if (res <= 0) {
                    if (res < 0 && res != -ECONNRESET && res != -EPIPE) {
                        LOGERR("Read error: %s", strerror(-res));
                    }
                    LOGINF("Client disconnected");
                    close(session->client_fd);
                    transcode_session_stop(&session->transcode_session);
                    audio_engine_destroy(&session->audio_engine);
                    free(session);
                } else {
                    process_client_read(session, res);
                }
                break;
            }

            case IO_OP_WRITE: {
                MachSession *session   = (MachSession *)req->ctx;
                session->write_pending = false;
                if (res < 0) {
                    LOGERR("Write error: %s", strerror(-res));
                    LOGINF("Closing connection due to write error");
                    close(session->client_fd);
                    transcode_session_stop(&session->transcode_session);
                    audio_engine_destroy(&session->audio_engine);
                    free(session);
                } else {
                    // Re-submit read now that the write has completed (Sequential request-response)
                    struct io_uring_sqe *r_sqe = io_uring_get_sqe(&server->ring);
                    if (r_sqe) {
                        io_uring_prep_recv(
                            r_sqe,
                            session->client_fd,
                            session->read_buf,
                            sizeof(session->read_buf),
                            0);
                        io_uring_sqe_set_data(r_sqe, &session->read_req);
                    }
                }
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
