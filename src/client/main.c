#include <arpa/inet.h>
#include <endian.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

#include "machaudio/log.h"
#include "machaudio/protocol.h"
#include "machaudio/server.h"

#define DEFAULT_PTIME_MS   20
#define DEFAULT_CONCURRENT 1
#define DEFAULT_RATE       8000
#define DEFAULT_CHANNELS   1
#define DEFAULT_FORMAT     0 // PCMU

typedef struct {
    char const *uds_dir;
    char const *uds_name;
    char const *host;
    int         port;
    char const *input_file;
    char const *input_file_2;
    bool        write_output;
    uint32_t    rate;
    uint8_t     channels;
    uint8_t     format;
    uint8_t     in_endian;
    uint32_t    out_rate;
    uint8_t     out_channels;
    uint8_t     out_format;
    uint8_t     out_endian;
    uint32_t    concurrent;
    uint32_t    ptime;
    bool        loop;
    uint32_t    duration_sec;
    bool        vad_enabled;
} ClientConfig;

typedef struct {
    union {
        uv_pipe_t   pipe;
        uv_tcp_t    tcp;
        uv_stream_t stream;
    } handle;
    uv_timer_t timer;
    uint32_t   id;
    size_t     offset;
    size_t     offset_2;
    uint32_t   sequence_id;
    FILE      *output_file;
    FILE      *output_vad_file;

    // Statistics
    uint64_t  total_sent;
    uint64_t  total_received;
    uint64_t  min_duration_ns;
    uint64_t  max_duration_ns;
    uint64_t  sum_duration_ns;
    uint64_t  count_received;
    uint64_t *latencies;
    size_t    latencies_cap;
    size_t    latencies_count;
} ClientConnection;

typedef struct {
    ClientConfig      config;
    uint8_t          *input_data;
    size_t            input_size;
    uint8_t          *input_data_2;
    size_t            input_size_2;
    uv_loop_t        *loop;
    ClientConnection *connections;
    uv_timer_t        global_timer;
    bool              running;
} ClientApp;

static ClientApp g_app = {0};

static char const *get_codec_extension(uint8_t format) {
    switch (format) {
    case 0:
        return "mulaw";
    case 8:
        return "alaw";
    case 96:
        return "l16";
    case 111:
        return "opus";
    default:
        return "raw";
    }
}

static void print_usage(char const *prog) {
    printf("Usage: %s [options]\n", prog);
    printf("Options:\n");
    printf("  -D, --uds-dir <path>      UDS directory (default: %s)\n", DEFAULT_UDS_DIR);
    printf("  -S, --uds-name <name>     UDS base name (default: %s)\n", DEFAULT_UDS_NAME);
    printf("  -H, --host <ip>           TCP host (e.g., 127.0.0.1)\n");
    printf("  -P, --port <port>         TCP port (e.g., 8000)\n");
    printf("  -i, --input <file>        Input raw PCM file (required)\n");
    printf("  -j, --input2 <file>       Second input file for mixing\n");
    printf("  -w, --write-output        Write raw output to file\n");
    printf("  -f, --format <type>       Input format (0=ulaw, 8=alaw, 96=l16, 111=opus)\n");
    printf("  -r, --rate <hz>           Input sample rate (default: %d)\n", DEFAULT_RATE);
    printf("  -c, --channels <num>      Channel count (default: %d)\n", DEFAULT_CHANNELS);
    printf("  -e, --in-endian <num>     Input endianness (0=none, 1=LE, 2=BE, default: 1)\n");
    printf("  -F, --out-format <type>   Output format (default: 96)\n");
    printf("  -R, --out-rate <hz>       Output sample rate (default: 16000)\n");
    printf("  -C, --out-channels <num>  Output channel count (default: 1)\n");
    printf("  -E, --out-endian <num>    Output endianness (0=none, 1=LE, 2=BE, default: 1)\n");
    printf(
        "  -n, --concurrent <count>  Number of concurrent connections (default: %d)\n",
        DEFAULT_CONCURRENT);
    printf("  -p, --ptime <ms>          Packet time in ms (default: %d)\n", DEFAULT_PTIME_MS);
    printf("  -l, --loop                Loop the input file\n");
    printf("  -d, --duration <sec>      Total test duration when looping\n");
    printf("  -V, --vad                 Enable VAD processing\n");
    printf("  -h, --help                Show this help\n");
}

static void parse_args(int argc, char **argv, ClientConfig *const config) {
    static struct option const long_options[] = {
        {"uds-dir", required_argument, 0, 'D'},
        {"uds-name", required_argument, 0, 'S'},
        {"host", required_argument, 0, 'H'},
        {"port", required_argument, 0, 'P'},
        {"input", required_argument, 0, 'i'},
        {"input2", required_argument, 0, 'j'},
        {"write-output", no_argument, 0, 'w'},
        {"format", required_argument, 0, 'f'},
        {"rate", required_argument, 0, 'r'},
        {"channels", required_argument, 0, 'c'},
        {"in-endian", required_argument, 0, 'e'},
        {"out-format", required_argument, 0, 'F'},
        {"out-rate", required_argument, 0, 'R'},
        {"out-channels", required_argument, 0, 'C'},
        {"out-endian", required_argument, 0, 'E'},
        {"concurrent", required_argument, 0, 'n'},
        {"ptime", required_argument, 0, 'p'},
        {"loop", no_argument, 0, 'l'},
        {"duration", required_argument, 0, 'd'},
        {"vad", no_argument, 0, 'V'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    config->uds_dir      = NULL;
    config->uds_name     = NULL;
    config->host         = NULL;
    config->port         = 0;
    config->rate         = DEFAULT_RATE;
    config->channels     = DEFAULT_CHANNELS;
    config->format       = DEFAULT_FORMAT;
    config->in_endian    = 1;
    config->out_rate     = 16000;
    config->out_channels = 1;
    config->out_format   = 96;
    config->out_endian   = 1;
    config->concurrent   = DEFAULT_CONCURRENT;
    config->ptime        = DEFAULT_PTIME_MS;
    config->vad_enabled  = false;

    int opt;
    int option_index = 0;
    while ((opt = getopt_long(
                argc,
                argv,
                "D:S:H:P:i:j:wf:r:c:e:F:R:C:E:n:p:ld:hV",
                long_options,
                &option_index)) != -1)
    {
        switch (opt) {
        case 'D':
            config->uds_dir = optarg;
            break;
        case 'S':
            config->uds_name = optarg;
            break;
        case 'H':
            config->host = optarg;
            break;
        case 'P':
            config->port = atoi(optarg);
            break;
        case 'i':
            config->input_file = optarg;
            break;
        case 'j':
            config->input_file_2 = optarg;
            break;
        case 'w':
            config->write_output = true;
            break;
        case 'f':
            config->format = (uint8_t)atoi(optarg);
            break;
        case 'r':
            config->rate = (uint32_t)atoi(optarg);
            break;
        case 'c':
            config->channels = (uint8_t)atoi(optarg);
            break;
        case 'e':
            config->in_endian = (uint8_t)atoi(optarg);
            break;
        case 'F':
            config->out_format = (uint8_t)atoi(optarg);
            break;
        case 'R':
            config->out_rate = (uint32_t)atoi(optarg);
            break;
        case 'C':
            config->out_channels = (uint8_t)atoi(optarg);
            break;
        case 'E':
            config->out_endian = (uint8_t)atoi(optarg);
            break;
        case 'n':
            config->concurrent = (uint32_t)atoi(optarg);
            break;
        case 'p':
            config->ptime = (uint32_t)atoi(optarg);
            break;
        case 'l':
            config->loop = true;
            break;
        case 'd':
            config->duration_sec = (uint32_t)atoi(optarg);
            break;
        case 'V':
            config->vad_enabled = true;
            break;
        case 'h':
        default:
            print_usage(argv[0]);
            exit(opt == 'h' ? 0 : 1);
        }
    }

    if (!config->uds_dir && !config->host) {
        config->uds_dir = DEFAULT_UDS_DIR;
    }
    if (!config->uds_name) {
        config->uds_name = DEFAULT_UDS_NAME;
    }

    if (config->uds_dir && (config->host || config->port)) {
        LOGERR("Cannot specify both UDS directory and TCP host/port");
        print_usage(argv[0]);
        exit(1);
    }

    if (config->host && config->port == 0) {
        LOGERR("TCP port is required when host is specified");
        print_usage(argv[0]);
        exit(1);
    }

    if (config->host && (config->port < 1024 || config->port > 65535)) {
        LOGERR("Invalid TCP port: %d", config->port);
        print_usage(argv[0]);
        exit(1);
    }

    if (!config->input_file) {
        LOGERR("Input file is required (-i, --input)");
        print_usage(argv[0]);
        exit(1);
    }
}

static void on_walk_close(uv_handle_t *handle, void *arg) {
    (void)arg;
    if (!uv_is_closing(handle)) {
        uv_close(handle, NULL);
    }
}

static void stop_app(void) {
    if (g_app.running) {
        g_app.running = false;
        uv_walk(g_app.loop, on_walk_close, NULL);
    }
}

static void on_global_timer(uv_timer_t *handle) {
    (void)handle;
    LOGINF("Test duration reached, stopping...");
    stop_app();
}

static void signal_handler(int sig) {
    LOGINF("Signal %s received, stopping...", log_get_signal_name(sig));
    stop_app();
}

static void load_input_file(ClientApp *const app) {
    int fd = open(app->config.input_file, O_RDONLY);
    if (fd < 0) {
        perror("open input_file");
        exit(1);
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        perror("fstat input_file");
        close(fd);
        exit(1);
    }

    app->input_size = (size_t)st.st_size;
    app->input_data = mmap(NULL, app->input_size, PROT_READ, MAP_SHARED, fd, 0);
    if (app->input_data == MAP_FAILED) {
        perror("mmap input_file");
        close(fd);
        exit(1);
    }
    close(fd);
    LOGINF("Loaded input file 1: %s (%zu bytes)", app->config.input_file, app->input_size);

    if (app->config.input_file_2) {
        int fd2 = open(app->config.input_file_2, O_RDONLY);
        if (fd2 < 0) {
            perror("open input_file_2");
            exit(1);
        }

        if (fstat(fd2, &st) < 0) {
            perror("fstat input_file_2");
            close(fd2);
            exit(1);
        }

        app->input_size_2 = (size_t)st.st_size;
        app->input_data_2 = mmap(NULL, app->input_size_2, PROT_READ, MAP_SHARED, fd2, 0);
        if (app->input_data_2 == MAP_FAILED) {
            perror("mmap input_file_2");
            close(fd2);
            exit(1);
        }
        close(fd2);
        LOGINF("Loaded input file 2: %s (%zu bytes)", app->config.input_file_2, app->input_size_2);
    }
}

static void on_alloc(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
    (void)handle;
    buf->base = malloc(suggested_size);
    buf->len  = suggested_size;
}

static void on_read(uv_stream_t *stream, ssize_t nread, uv_buf_t const *buf) {
    ClientConnection *const conn = (ClientConnection *)stream->data;

    if (nread > 0) {
        // Process protocol output
        uint8_t const *ptr       = (uint8_t const *)buf->base;
        ssize_t        remaining = nread;

        while (remaining >= (ssize_t)sizeof(AudioMsgHeader)) {
            AudioMsgHeader header;
            memcpy(&header, ptr, sizeof(AudioMsgHeader));
            protocol_decode_header(&header);

            if (header.magic != AUDIO_MAGIC) {
                LOGERR("Invalid magic: 0x%08X", header.magic);
                break;
            }

            if (remaining < (ssize_t)(sizeof(AudioMsgHeader) + header.payload_len)) {
                // Partial message, should ideally buffer, but for test client we assume small
                // chunks
                break;
            }

            if (header.command == CMD_OUTPUT) {
                struct audio_output_payload *payload =
                    (struct audio_output_payload *)(ptr + sizeof(AudioMsgHeader));
                uint64_t const duration_ns = be64toh(payload->duration_ns);

                uint32_t prob_net_bits;
                memcpy(&prob_net_bits, &payload->vad_prob, sizeof(uint32_t));
                float const vad_prob = protocol_net_to_float(prob_net_bits);

                if (g_app.config.vad_enabled) {
                    LOGINF(
                        "Connection %u received chunk with VAD probability: %.6f",
                        conn->id,
                        vad_prob);
                }

                conn->count_received++;
                conn->total_received += header.payload_len - sizeof(struct audio_output_payload);
                conn->sum_duration_ns += duration_ns;

                if (duration_ns < conn->min_duration_ns) {
                    conn->min_duration_ns = duration_ns;
                }
                if (duration_ns > conn->max_duration_ns) {
                    conn->max_duration_ns = duration_ns;
                }

                // Record latency for percentile calculations
                if (conn->latencies_count >= conn->latencies_cap) {
                    size_t const new_cap =
                        (conn->latencies_cap == 0) ? 1024 : conn->latencies_cap * 2;
                    uint64_t *const new_latencies =
                        realloc(conn->latencies, new_cap * sizeof(uint64_t));
                    if (new_latencies) {
                        conn->latencies     = new_latencies;
                        conn->latencies_cap = new_cap;
                    }
                }

                if (conn->latencies_count < conn->latencies_cap) {
                    conn->latencies[conn->latencies_count++] = duration_ns;
                }

                if (conn->output_file) {
                    fwrite(
                        payload->data,
                        1,
                        header.payload_len - sizeof(struct audio_output_payload),
                        conn->output_file);
                }

                if (conn->output_vad_file) {
                    fwrite(&vad_prob, sizeof(float), 1, conn->output_vad_file);
                }
            }

            size_t msg_total = sizeof(AudioMsgHeader) + header.payload_len;
            ptr += msg_total;
            remaining -= (ssize_t)msg_total;
        }
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            LOGERR("Read error: %s", uv_strerror((int)nread));
        }
        uv_close((uv_handle_t *)stream, NULL);
    }

    free(buf->base);
}

// Statistics
typedef struct {
    uv_write_t req;
    void      *buf;
} WriteRequest;

static void on_write_completed(uv_write_t *req, int status) {
    WriteRequest *const wr = (WriteRequest *)req;
    if (status < 0 && status != UV_ECANCELED) {
        LOGERR("Write error: %s", uv_strerror(status));
    }
    free(wr->buf);
    free(wr);
}

static void client_init(uv_loop_t *loop, uv_stream_t *stream) {
    if (g_app.config.host) {
        uv_tcp_init(loop, (uv_tcp_t *)stream);
    } else {
        uv_pipe_init(loop, (uv_pipe_t *)stream, 0);
    }
}

static void
client_connect(uv_stream_t *stream, uv_connect_t *req, uv_connect_cb cb, uint32_t worker_index) {
    if (g_app.config.host) {
        struct sockaddr_in addr;
        uv_ip4_addr(g_app.config.host, g_app.config.port + (int)worker_index, &addr);
        uv_tcp_connect(req, (uv_tcp_t *)stream, (struct sockaddr const *)&addr, cb);
    } else {
        char socket_path[256];
        snprintf(
            socket_path,
            sizeof(socket_path),
            "%s/%s.%u.sock",
            g_app.config.uds_dir,
            g_app.config.uds_name,
            worker_index);
        uv_pipe_connect(req, (uv_pipe_t *)stream, socket_path, cb);
    }
}

static void send_input_chunk(ClientConnection *const conn) {
    size_t const samples_per_ms   = g_app.config.rate / 1000;
    size_t const bytes_per_sample = (g_app.config.format == 96) ? 2 : 1;
    size_t const chunk_size =
        samples_per_ms * g_app.config.ptime * bytes_per_sample * g_app.config.channels;

    bool const has_input2 = (g_app.input_data_2 != NULL);

    if (conn->offset + chunk_size > g_app.input_size) {
        if (g_app.config.loop) {
            conn->offset = 0;
        } else {
            uv_timer_stop(&conn->timer);
            return;
        }
    }

    if (has_input2 && conn->offset_2 + chunk_size > g_app.input_size_2) {
        if (g_app.config.loop) {
            conn->offset_2 = 0;
        } else {
            uv_timer_stop(&conn->timer);
            return;
        }
    }

    size_t const   padded_chunk_size = (chunk_size + 3) & ~3U;
    uint32_t const num_buffers       = has_input2 ? 2 : 1;
    size_t const   payload_len =
        sizeof(struct audio_input_payload) +
        num_buffers * (sizeof(struct audio_buffer_header) + padded_chunk_size);
    size_t const total_len = sizeof(AudioMsgHeader) + payload_len;

    uint8_t *const        buf    = calloc(1, total_len);
    AudioMsgHeader *const header = (AudioMsgHeader *)buf;
    header->magic                = htonl(AUDIO_MAGIC);
    header->version              = htons(AUDIO_VERSION);
    header->command              = htons(CMD_INPUT);
    header->sequence_id          = htonl(conn->sequence_id++);
    header->payload_len          = htonl((uint32_t)payload_len);

    struct audio_input_payload *const ip =
        (struct audio_input_payload *)(buf + sizeof(AudioMsgHeader));
    ip->num_buffers = htonl(num_buffers);

    uint8_t *ptr = buf + sizeof(AudioMsgHeader) + sizeof(struct audio_input_payload);

    // Buffer 1
    struct audio_buffer_header *bh1 = (struct audio_buffer_header *)ptr;
    bh1->length                     = htonl((uint32_t)chunk_size);
    uint32_t vol_bits_net           = protocol_float_to_net(1.0f);
    memcpy(&bh1->volume, &vol_bits_net, 4);
    ptr += sizeof(struct audio_buffer_header);
    memcpy(ptr, g_app.input_data + conn->offset, chunk_size);
    ptr += padded_chunk_size;

    conn->offset += chunk_size;
    conn->total_sent += chunk_size;

    // Buffer 2
    if (has_input2) {
        struct audio_buffer_header *bh2 = (struct audio_buffer_header *)ptr;
        bh2->length                     = htonl((uint32_t)chunk_size);
        memcpy(&bh2->volume, &vol_bits_net, 4);
        ptr += sizeof(struct audio_buffer_header);
        memcpy(ptr, g_app.input_data_2 + conn->offset_2, chunk_size);
        ptr += padded_chunk_size;

        conn->offset_2 += chunk_size;
        conn->total_sent += chunk_size;
    }

    uv_buf_t            uv_buf = uv_buf_init((char *)buf, (unsigned int)total_len);
    WriteRequest *const wr     = malloc(sizeof(WriteRequest));
    wr->buf                    = buf;
    uv_write(&wr->req, (uv_stream_t *)&conn->handle.stream, &uv_buf, 1, on_write_completed);
}

static void on_timer(uv_timer_t *handle) {
    ClientConnection *const conn = (ClientConnection *)handle->data;
    send_input_chunk(conn);
}

static void on_connect(uv_connect_t *req, int status) {
    ClientConnection *const conn = (ClientConnection *)req->data;
    free(req);

    if (status < 0) {
        LOGERR("Connect error: %s", uv_strerror(status));
        return;
    }

    LOGINF("Connection %u established", conn->id);

    // Send CMD_START
    size_t const   payload_len = sizeof(struct audio_start_payload);
    size_t const   total_len   = sizeof(AudioMsgHeader) + payload_len;
    uint8_t *const buf         = calloc(1, total_len);

    AudioMsgHeader *const header = (AudioMsgHeader *)buf;
    header->magic                = htonl(AUDIO_MAGIC);
    header->version              = htons(AUDIO_VERSION);
    header->command              = htons(CMD_START);
    header->sequence_id          = htonl(conn->sequence_id++);
    header->payload_len          = htonl((uint32_t)payload_len);

    struct audio_start_payload *const payload =
        (struct audio_start_payload *)(buf + sizeof(AudioMsgHeader));
    payload->in_payload_type = g_app.config.format;
    payload->in_channels     = g_app.config.channels;
    payload->flags           = htons(g_app.config.vad_enabled ? AUDIO_START_FLAGS_VAD_ENABLED : 0);
    payload->in_endian       = g_app.config.in_endian;
    payload->in_sample_rate  = htonl(g_app.config.rate);

    payload->out_payload_type = g_app.config.out_format;
    payload->out_channels     = g_app.config.out_channels;
    payload->out_endian       = g_app.config.out_endian;
    payload->out_sample_rate  = htonl(g_app.config.out_rate);

    uv_buf_t            uv_buf = uv_buf_init((char *)buf, (unsigned int)total_len);
    WriteRequest *const wr     = malloc(sizeof(WriteRequest));
    wr->buf                    = buf;
    uv_write(&wr->req, (uv_stream_t *)&conn->handle.stream, &uv_buf, 1, on_write_completed);

    // Start reading
    uv_read_start((uv_stream_t *)&conn->handle.stream, on_alloc, on_read);

    // Start timer for real-time pacing
    uv_timer_start(&conn->timer, on_timer, g_app.config.ptime, g_app.config.ptime);
}

// --- Discovery Phase ---
typedef struct {
    union {
        uv_pipe_t   pipe;
        uv_tcp_t    tcp;
        uv_stream_t stream;
    } handle;
    uint32_t num_workers;
    bool     completed;
} DiscoveryClient;

static void on_discover_read(uv_stream_t *stream, ssize_t nread, uv_buf_t const *buf) {
    DiscoveryClient *dc = (DiscoveryClient *)stream->data;
    if (nread > 0) {
        if (nread >= (ssize_t)sizeof(AudioMsgHeader)) {
            AudioMsgHeader header;
            memcpy(&header, buf->base, sizeof(AudioMsgHeader));
            protocol_decode_header(&header);

            if (header.command == CMD_DISCOVER_REPLY &&
                header.payload_len >= sizeof(struct audio_discover_reply_payload))
            {
                struct audio_discover_reply_payload payload;
                memcpy(&payload, buf->base + sizeof(AudioMsgHeader), sizeof(payload));
                dc->num_workers = ntohl(payload.num_workers);
                dc->completed   = true;
                LOGINF("Discovery successful: %u worker(s) running.", dc->num_workers);
            }
        }
        uv_close((uv_handle_t *)stream, NULL);
    } else if (nread < 0) {
        if (nread != UV_EOF) {
            LOGERR("Discovery read error: %s", uv_strerror((int)nread));
        }
        uv_close((uv_handle_t *)stream, NULL);
    }
    if (buf->base) {
        free(buf->base);
    }
}

static void on_discover_connect(uv_connect_t *req, int status) {
    DiscoveryClient *dc = (DiscoveryClient *)req->data;
    free(req);

    if (status < 0) {
        LOGERR("Discovery connection failed: %s", uv_strerror(status));
        uv_close((uv_handle_t *)&dc->handle.stream, NULL);
        return;
    }

    size_t const    total_len = sizeof(AudioMsgHeader);
    uint8_t *const  buf       = calloc(1, total_len);
    AudioMsgHeader *header    = (AudioMsgHeader *)buf;
    header->magic             = htonl(AUDIO_MAGIC);
    header->version           = htons(AUDIO_VERSION);
    header->command           = htons(CMD_DISCOVER);
    header->sequence_id       = htonl(0);
    header->payload_len       = 0;

    uv_buf_t      uv_buf = uv_buf_init((char *)buf, (unsigned int)total_len);
    WriteRequest *wr     = malloc(sizeof(WriteRequest));
    wr->buf              = buf;
    uv_write(&wr->req, (uv_stream_t *)&dc->handle.stream, &uv_buf, 1, on_write_completed);

    uv_read_start((uv_stream_t *)&dc->handle.stream, on_alloc, on_discover_read);
}
// --- End Discovery Phase ---

static int compare_uint64(void const *const a, void const *const b) {
    uint64_t const va = *(uint64_t const *)a;
    uint64_t const vb = *(uint64_t const *)b;
    return (va > vb) - (va < vb);
}

int main(int argc, char **argv) {
    parse_args(argc, argv, &g_app.config);
    load_input_file(&g_app);

    g_app.loop = uv_default_loop();

    LOGINF("Starting discovery phase...");
    DiscoveryClient dc = {0};
    client_init(g_app.loop, &dc.handle.stream);
    dc.handle.stream.data = &dc;

    uv_connect_t *disc_req = malloc(sizeof(uv_connect_t));
    disc_req->data         = &dc;
    client_connect(&dc.handle.stream, disc_req, on_discover_connect, 0);

    uv_run(g_app.loop, UV_RUN_DEFAULT); // Blocks until discovery finishes

    if (!dc.completed) {
        LOGERR("Discovery failed. Exiting.");
        munmap(g_app.input_data, g_app.input_size);
        if (g_app.input_data_2) {
            munmap(g_app.input_data_2, g_app.input_size_2);
        }
        return 1;
    }

    g_app.connections = calloc(g_app.config.concurrent, sizeof(ClientConnection));
    if (!g_app.connections) {
        LOGERR("Failed to allocate memory for connections");
        munmap(g_app.input_data, g_app.input_size);
        if (g_app.input_data_2) {
            munmap(g_app.input_data_2, g_app.input_size_2);
        }
        return 1;
    }

    char timestamp_str[64] = {0};
    if (g_app.config.write_output) {
        time_t const           t  = time(NULL);
        struct tm const *const tm = localtime(&t);
        strftime(timestamp_str, sizeof(timestamp_str), "%Y%m%d_%H%M%S", tm);
    }

    for (uint32_t i = 0; i < g_app.config.concurrent; i++) {
        ClientConnection *const conn = &g_app.connections[i];
        conn->id                     = i;
        conn->min_duration_ns        = UINT64_MAX;
        conn->handle.stream.data     = conn;
        conn->timer.data             = conn;

        client_init(g_app.loop, &conn->handle.stream);
        uv_timer_init(g_app.loop, &conn->timer);

        if (g_app.config.write_output) {
            char filename[256];
            snprintf(
                filename,
                sizeof(filename),
                "output_%s_%04u.%s",
                timestamp_str,
                i,
                get_codec_extension(g_app.config.out_format));

            conn->output_file = fopen(filename, "wb");
            if (!conn->output_file) {
                LOGERR("Failed to open output audio file: %s", filename);
            }

            snprintf(filename, sizeof(filename), "output_%s_%04u.vad", timestamp_str, i);

            conn->output_vad_file = fopen(filename, "wb");
            if (!conn->output_vad_file) {
                LOGERR("Failed to open output vad file: %s", filename);
            }
        }

        uv_connect_t *const connect_req = malloc(sizeof(uv_connect_t));
        connect_req->data               = conn;
        client_connect(&conn->handle.stream, connect_req, on_connect, i % dc.num_workers);
    }

    if (g_app.config.duration_sec > 0) {
        uv_timer_init(g_app.loop, &g_app.global_timer);
        uv_timer_start(&g_app.global_timer, on_global_timer, g_app.config.duration_sec * 1000, 0);
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    g_app.running = true;
    uv_run(g_app.loop, UV_RUN_DEFAULT);

    // Report stats
    LOGINF("Test summary:");
    uint64_t total_sent         = 0;
    uint64_t total_received     = 0;
    uint64_t sum_duration_ns    = 0;
    uint64_t count_received     = 0;
    uint64_t global_min         = UINT64_MAX;
    uint64_t global_max         = 0;
    size_t   total_latency_pool = 0;

    for (uint32_t i = 0; i < g_app.config.concurrent; i++) {
        total_latency_pool += g_app.connections[i].latencies_count;
    }

    uint64_t *const all_latencies =
        (total_latency_pool > 0) ? malloc(total_latency_pool * sizeof(uint64_t)) : NULL;
    size_t all_latencies_idx = 0;

    for (uint32_t i = 0; i < g_app.config.concurrent; i++) {
        ClientConnection *const conn = &g_app.connections[i];

        if (conn->latencies_count > 0) {
            qsort(conn->latencies, conn->latencies_count, sizeof(uint64_t), compare_uint64);

            size_t const   p95_idx = (conn->latencies_count * 95) / 100;
            uint64_t const p95_val = conn->latencies[p95_idx];
            uint64_t const avg_val = conn->sum_duration_ns / conn->latencies_count;

            LOGINF(
                "  Conn %u: Avg: %.3f ms, P95: %.3f ms, Min: %.3f ms, Max: %.3f ms (samples: %zu)",
                conn->id,
                (double)avg_val / 1000000.0,
                (double)p95_val / 1000000.0,
                (double)conn->min_duration_ns / 1000000.0,
                (double)conn->max_duration_ns / 1000000.0,
                conn->latencies_count);

            if (all_latencies) {
                memcpy(
                    all_latencies + all_latencies_idx,
                    conn->latencies,
                    conn->latencies_count * sizeof(uint64_t));
                all_latencies_idx += conn->latencies_count;
            }
        }

        total_sent += conn->total_sent;
        total_received += conn->total_received;
        sum_duration_ns += conn->sum_duration_ns;
        count_received += conn->count_received;

        if (conn->min_duration_ns < global_min) {
            global_min = conn->min_duration_ns;
        }
        if (conn->max_duration_ns > global_max) {
            global_max = conn->max_duration_ns;
        }

        if (conn->output_file) {
            fclose(conn->output_file);
        }

        if (conn->output_vad_file) {
            fclose(conn->output_vad_file);
        }

        free(conn->latencies);
    }

    LOGINF("Global Summary:");
    LOGINF("  Total Sent:     %lu bytes", total_sent);
    LOGINF("  Total Received: %lu bytes", total_received);
    if (count_received > 0) {
        uint64_t const avg_ns = sum_duration_ns / count_received;
        uint64_t       p95_ns = 0;

        if (all_latencies) {
            qsort(all_latencies, total_latency_pool, sizeof(uint64_t), compare_uint64);
            size_t const p95_idx = (total_latency_pool * 95) / 100;
            p95_ns               = all_latencies[p95_idx];
            free(all_latencies);
        }

        LOGINF(
            "  Latency (ms):   Min: %.3f, Max: %.3f, Avg: %.3f, P95: %.3f",
            (double)global_min / 1000000.0,
            (double)global_max / 1000000.0,
            (double)avg_ns / 1000000.0,
            (double)p95_ns / 1000000.0);
    }

    munmap(g_app.input_data, g_app.input_size);
    if (g_app.input_data_2) {
        munmap(g_app.input_data_2, g_app.input_size_2);
    }
    free(g_app.connections);

    return 0;
}
