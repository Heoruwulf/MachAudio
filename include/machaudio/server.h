#ifndef MACHAUDIO_SERVER_H
#define MACHAUDIO_SERVER_H

#include <uv.h>
#include "machaudio/arena.h"
#include "machaudio/audio.h"
#include "machaudio/transcode.h"

#define MACH_SESSION_ARENA_SIZE 262144 // 256KB per session

#define DEFAULT_UDS_DIR  "/tmp"
#define DEFAULT_UDS_NAME "machaudio"

typedef struct {
    uv_loop_t *loop;
    union {
        uv_pipe_t pipe;
        uv_tcp_t  tcp;
    } handle;
    char const *socket_path;
    char const *host;
    int         port;
    bool        is_tcp;
    uint32_t    num_workers;
} MachServer;

typedef struct {
    union {
        uv_pipe_t pipe;
        uv_tcp_t  tcp;
    } handle;
    Arena            arena;
    MachAudioEngine  audio_engine;
    TranscodeSession transcode_session;
    uint8_t          arena_buf[MACH_SESSION_ARENA_SIZE];
    uint8_t          read_buf[MACH_SESSION_ARENA_SIZE];
    bool             is_tcp;
    MachServer      *server;
} MachSession;

/**
 * Initializes and starts the MachAudio server.
 * If host is NULL, it uses UDS with socket_path.
 * If host is non-NULL, it uses TCP with host and port.
 */
int mach_server_init(
    MachServer *const server,
    uv_loop_t *const  loop,
    char const *const socket_path,
    char const *const host,
    int const         port,
    uint32_t const    num_workers);

/**
 * Starts listening for connections.
 */
int mach_server_start(MachServer *const server);

/**
 * Gracefully stops the server and closes all handles.
 */
void mach_server_stop(MachServer *const server);

#endif // MACHAUDIO_SERVER_H
