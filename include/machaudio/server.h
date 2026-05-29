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
} MachServer;

typedef struct {
    int              client_fd;
    IoRequest        read_req;
    IoRequest        write_req;
    Arena            arena;
    MachAudioEngine  audio_engine;
    TranscodeSession transcode_session;
    uint8_t          arena_buf[MACH_SESSION_ARENA_SIZE];
    uint8_t          read_buf[MACH_SESSION_ARENA_SIZE];
    uint8_t          write_buf[MACH_SESSION_ARENA_SIZE];
    size_t           arena_curr_start;
    bool             is_tcp;
    MachServer      *server;
    bool             write_pending;
} MachSession;

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
    uint32_t const    num_workers);

/**
 * Starts the io_uring event loop and listens for connections.
 */
int mach_server_start(MachServer *const server);

/**
 * Gracefully stops the server event loop.
 */
void mach_server_stop(MachServer *const server);

#endif // MACHAUDIO_SERVER_H
