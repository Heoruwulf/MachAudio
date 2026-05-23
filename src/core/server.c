#include "machaudio/server.h"
#include "machaudio/log.h"
#include "machaudio/protocol.h"

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    MachSession *const session = (MachSession *)handle->data;
    (void)suggested_size;

    // Use a dedicated read buffer so we don't corrupt input when writing to the arena.
    buf->base = (char *)session->read_buf;
    buf->len  = sizeof(session->read_buf);
}

static void on_close(uv_handle_t *handle) {
    MachSession *const session = (MachSession *)handle->data;
    LOGINF("Client disconnected");
    transcode_session_stop(&session->transcode_session);
    audio_engine_destroy(&session->audio_engine);
    free(session);
}

static void on_write(uv_write_t *req, int status) {
    if (status < 0) {
        LOGERR("Write error: %s", uv_strerror(status));
    }
    free(req->data);
    free(req);
}

static void send_error(uv_stream_t *client, uint32_t sequence_id, AudioErrorCode code) {
    size_t const   resp_len = sizeof(AudioMsgHeader) + sizeof(struct audio_error_payload);
    uint8_t *const resp_buf = malloc(resp_len);
    if (resp_buf == NULL)
        return;

    AudioMsgHeader *const resp_header = (AudioMsgHeader *)resp_buf;
    resp_header->magic                = htonl(AUDIO_MAGIC);
    resp_header->version              = htons(AUDIO_VERSION);
    resp_header->command              = htons(CMD_ERROR);
    resp_header->sequence_id          = htonl(sequence_id);
    resp_header->payload_len          = htonl((uint32_t)sizeof(struct audio_error_payload));

    struct audio_error_payload *const resp_payload =
        (struct audio_error_payload *)(resp_buf + sizeof(AudioMsgHeader));
    resp_payload->error_code = htonl((uint32_t)code);

    uv_write_t *const write_req = malloc(sizeof(uv_write_t));
    if (write_req == NULL) {
        free(resp_buf);
        return;
    }
    write_req->data        = resp_buf;
    uv_buf_t const uv_resp = uv_buf_init((char *)resp_buf, (unsigned int)resp_len);
    uv_write(write_req, client, &uv_resp, 1, on_write);
}

static void on_read(uv_stream_t *client, ssize_t nread, uv_buf_t const *buf) {
    MachSession *const session = (MachSession *)client->data;

    if (nread > 0) {
        if (nread < (ssize_t)sizeof(AudioMsgHeader)) {
            return;
        }

        AudioMsgHeader *header  = (AudioMsgHeader *)buf->base;
        AudioMsgHeader  decoded = *header;
        protocol_decode_header(&decoded);

        if (!protocol_validate_header(&decoded)) {
            LOGERR("Invalid protocol header received");
            send_error(client, decoded.sequence_id, ERR_INVALID_MAGIC);
            return;
        }

        if (nread < (ssize_t)(sizeof(AudioMsgHeader) + decoded.payload_len)) {
            return;
        }

        void *payload = (uint8_t *)buf->base + sizeof(AudioMsgHeader);

        switch (decoded.command) {
        case CMD_START: {
            LOGINF("Received CMD_START");
            if (decoded.payload_len < sizeof(struct audio_start_payload)) {
                send_error(client, decoded.sequence_id, ERR_INVALID_PAYLOAD);
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
                send_error(client, decoded.sequence_id, ERR_INVALID_PAYLOAD);
                break;
            }

            session->arena.curr     = session->arena_curr_start;
            uint64_t const start_ns = uv_hrtime();

            int r = audio_process_transcode(
                &session->transcode_session,
                (struct audio_input_payload const *)payload,
                decoded.payload_len,
                &session->arena);

            uint64_t const end_ns      = uv_hrtime();
            uint64_t const duration_ns = end_ns - start_ns;

            if (r == 0) {
                size_t const out_data_len = arena_used(&session->arena) - session->arena_curr_start;
                size_t const payload_len  = sizeof(struct audio_output_payload) + out_data_len;
                size_t const resp_len     = sizeof(AudioMsgHeader) + payload_len;

                uint8_t *const resp_buf = malloc(resp_len);
                if (resp_buf == NULL)
                    break;

                AudioMsgHeader *const resp_header = (AudioMsgHeader *)resp_buf;
                resp_header->magic                = htonl(AUDIO_MAGIC);
                resp_header->version              = htons(AUDIO_VERSION);
                resp_header->command              = htons(CMD_OUTPUT);
                resp_header->sequence_id          = htonl(decoded.sequence_id);
                resp_header->payload_len          = htonl((uint32_t)payload_len);

                struct audio_output_payload *const resp_payload =
                    (struct audio_output_payload *)(resp_buf + sizeof(AudioMsgHeader));
                resp_payload->duration_ns = htobe64(duration_ns);

                uint32_t const prob_net =
                    protocol_float_to_net(session->transcode_session.last_vad_prob);
                memcpy(&resp_payload->vad_prob, &prob_net, sizeof(float));

                memset(resp_payload->reserved, 0, sizeof(resp_payload->reserved));
                memcpy(
                    resp_payload->data,
                    session->arena.buf + session->arena_curr_start,
                    out_data_len);

                uv_write_t *const write_req = malloc(sizeof(uv_write_t));
                if (write_req == NULL) {
                    free(resp_buf);
                    break;
                }
                write_req->data        = resp_buf;
                uv_buf_t const uv_resp = uv_buf_init((char *)resp_buf, (unsigned int)resp_len);
                uv_write(write_req, client, &uv_resp, 1, on_write);

                session->arena.curr = session->arena_curr_start;
            } else {
                LOGERR("Transcode processing failed");
                send_error(client, decoded.sequence_id, ERR_PROCESSING_FAILED);
            }
            break;
        }

        case CMD_STOP:
            LOGINF("Received CMD_STOP");
            transcode_session_stop(&session->transcode_session);
            audio_engine_destroy(&session->audio_engine);
            break;

        case CMD_PING: {
            size_t const   resp_len = sizeof(AudioMsgHeader);
            uint8_t *const resp_buf = malloc(resp_len);
            if (resp_buf == NULL)
                break;

            AudioMsgHeader *const resp_header = (AudioMsgHeader *)resp_buf;
            resp_header->magic                = htonl(AUDIO_MAGIC);
            resp_header->version              = htons(AUDIO_VERSION);
            resp_header->command              = htons(CMD_PONG);
            resp_header->sequence_id          = htonl(decoded.sequence_id);
            resp_header->payload_len          = 0;

            uv_write_t *const write_req = malloc(sizeof(uv_write_t));
            if (write_req == NULL) {
                free(resp_buf);
                break;
            }
            write_req->data        = resp_buf;
            uv_buf_t const uv_resp = uv_buf_init((char *)resp_buf, (unsigned int)resp_len);
            uv_write(write_req, client, &uv_resp, 1, on_write);
            break;
        }

        case CMD_DISCOVER: {
            LOGINF("Received CMD_DISCOVER");
            size_t const resp_len =
                sizeof(AudioMsgHeader) + sizeof(struct audio_discover_reply_payload);
            uint8_t *const resp_buf = malloc(resp_len);
            if (resp_buf == NULL)
                break;

            AudioMsgHeader *const resp_header = (AudioMsgHeader *)resp_buf;
            resp_header->magic                = htonl(AUDIO_MAGIC);
            resp_header->version              = htons(AUDIO_VERSION);
            resp_header->command              = htons(CMD_DISCOVER_REPLY);
            resp_header->sequence_id          = htonl(decoded.sequence_id);
            resp_header->payload_len = htonl((uint32_t)sizeof(struct audio_discover_reply_payload));

            struct audio_discover_reply_payload *const resp_payload =
                (struct audio_discover_reply_payload *)(resp_buf + sizeof(AudioMsgHeader));
            resp_payload->num_workers = htonl(session->server->num_workers);
            resp_payload->reserved    = 0;

            uv_write_t *const write_req = malloc(sizeof(uv_write_t));
            if (write_req == NULL) {
                free(resp_buf);
                break;
            }
            write_req->data        = resp_buf;
            uv_buf_t const uv_resp = uv_buf_init((char *)resp_buf, (unsigned int)resp_len);
            uv_write(write_req, client, &uv_resp, 1, on_write);
            break;
        }

        default:

            LOGERR("Unknown command: %d", decoded.command);
            send_error(client, decoded.sequence_id, ERR_INVALID_COMMAND);
            break;
        }
    } else if (nread < 0) {
        if (unlikely(nread != UV_EOF)) {
            LOGERR("Read error: %s", uv_err_name(nread));
        }
        uv_close((uv_handle_t *)client, on_close);
    }
}

static void on_new_connection(uv_stream_t *server_stream, int status) {
    if (status < 0) {
        LOGERR("New connection error: %s", uv_strerror(status));
        return;
    }

    MachServer *const  server  = (MachServer *)server_stream->data;
    MachSession *const session = calloc(1, sizeof(MachSession));
    if (session == NULL) {
        return;
    }

    session->is_tcp = server->is_tcp;
    session->server = server;
    if (session->is_tcp) {
        uv_tcp_init(server->loop, &session->handle.tcp);
    } else {
        uv_pipe_init(server->loop, &session->handle.pipe, 0);
    }

    uv_stream_t *const client_stream = (uv_stream_t *)&session->handle;
    client_stream->data              = session;
    arena_init(&session->arena, session->arena_buf, sizeof(session->arena_buf), "session");

    if (uv_accept(server_stream, client_stream) == 0) {
        LOGINF("New client connected");
        uv_read_start(client_stream, on_alloc, on_read);
    } else {
        uv_close((uv_handle_t *)client_stream, on_close);
    }
}

int mach_server_init(
    MachServer *const server,
    uv_loop_t *const  loop,
    char const *const socket_path,
    char const *const host,
    int const         port,
    uint32_t const    num_workers) {
    if (server == NULL || loop == NULL) {
        return -1;
    }
    server->loop        = loop;
    server->socket_path = socket_path;
    server->host        = host;
    server->port        = port;
    server->is_tcp      = (host != NULL);
    server->num_workers = num_workers;

    int r;
    if (server->is_tcp) {
        r = uv_tcp_init(loop, &server->handle.tcp);
        if (r)
            return r;

        struct sockaddr_in addr;
        r = uv_ip4_addr(host, port, &addr);
        if (r)
            return r;

        r = uv_tcp_bind(&server->handle.tcp, (struct sockaddr const *)&addr, 0);
        if (r)
            return r;
    } else {
        r = uv_pipe_init(loop, &server->handle.pipe, 0);
        if (r)
            return r;

        if (socket_path) {
            unlink(socket_path); // Ensure path is clear
            r = uv_pipe_bind(&server->handle.pipe, socket_path);
            if (r)
                return r;

            r = uv_pipe_chmod(&server->handle.pipe, UV_WRITABLE | UV_READABLE);
            if (r)
                return r;
        }
    }

    server->handle.pipe.data = server;
    return 0;
}

int mach_server_start(MachServer *const server) {
    int r = uv_listen((uv_stream_t *)&server->handle, SOMAXCONN, on_new_connection);
    if (r) {
        LOGERR("Listen error: %s", uv_strerror(r));
        return r;
    }
    if (server->is_tcp) {
        LOGINF("MachAudio server listening on %s:%d", server->host, server->port);
    } else {
        LOGINF("MachAudio server listening on %s", server->socket_path);
    }
    return 0;
}

static void on_server_close(uv_handle_t *handle) { (void)handle; }

static void on_walk_close(uv_handle_t *handle, void *arg) {
    (void)arg;
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

void mach_server_stop(MachServer *const server) {
    if (server == NULL) {
        return;
    }

    if (!uv_is_closing((uv_handle_t *)&server->handle)) {
        uv_close((uv_handle_t *)&server->handle, on_server_close);
    }

    // Stop all other handles (clients, signals, etc.)
    uv_walk(server->loop, on_walk_close, NULL);
}
