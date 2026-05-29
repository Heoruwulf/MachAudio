#ifndef MACHAUDIO_SERVER_H
#define MACHAUDIO_SERVER_H

#include <liburing.h>
#include <stdbool.h>
#include "machaudio/arena.h"
#include "machaudio/audio.h"
#include "machaudio/transcode.h"

#define MACH_SESSION_ARENA_SIZE 262144 // 256KB per session

#define DEFAULT_UDS_DIR  "/tmp"
#define DEFAULT_UDS_NAME "machaudio"

#define BGID_READ_RING 1
#define NUM_BUFFERS    128
#define BUF_SIZE       262144

struct buf_ring_context {
    struct io_uring_buf_ring *br;
    void                     *buf_mem;
    int                       bgid;
};

typedef enum { IO_OP_ACCEPT, IO_OP_READ, IO_OP_WRITE, IO_OP_SIGNAL } IoOp;

typedef struct {
    IoOp  op;
    int   fd;
    void *ctx;
} IoRequest;

typedef struct {
    struct io_uring ring;
    int             listen_fd;
    int             signal_fd;
    IoRequest       accept_req;
    IoRequest       signal_req;
    char const     *socket_path;
    char const     *host;
    int             port;
    bool            is_tcp;
    uint32_t        num_workers;
    bool            running;
    struct buf_ring_context *buf_ring;
} MachServer;

typedef struct MachSession MachSession;

#define MAX_CONCURRENT_WRITES 32
#define MAX_WRITE_PAYLOAD     8192

typedef struct {
    IoRequest req;
    MachSession *session;
    size_t len;
    bool in_use;
    bool is_dynamic;
    uint8_t data[MAX_WRITE_PAYLOAD];
} WriteResponse;

struct MachSession {
    int              client_fd;
    IoRequest        read_req;
    Arena            arena;
    MachAudioEngine  audio_engine;
    TranscodeSession transcode_session;
    uint8_t          arena_buf[MACH_SESSION_ARENA_SIZE];
    WriteResponse    write_pool[MAX_CONCURRENT_WRITES];
    size_t           arena_curr_start;
    bool             is_tcp;
    MachServer      *server;
    int              pending_writes;
    bool             closing;
};

/**
 * Initializes the MachAudio server structure and sets up sockets.
 * If host is NULL, it uses UDS with socket_path.
 * If host is non-NULL, it uses TCP with host and port.
 */
int mach_server_init(
    MachServer *const server,
    char const *const socket_path,
    char const *const host,
    int const         port,
    uint32_t const    num_workers,
    int const         sq_thread_cpu);

/**
 * Starts the io_uring event loop and listens for connections.
 */
int mach_server_start(MachServer *const server);

/**
 * Gracefully stops the server event loop.
 */
void mach_server_stop(MachServer *const server);

#endif // MACHAUDIO_SERVER_H
