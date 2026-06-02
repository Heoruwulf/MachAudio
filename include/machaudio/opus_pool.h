#ifndef MACHAUDIO_OPUS_POOL_H
#define MACHAUDIO_OPUS_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct MachSession MachSession;
typedef struct MachServer  MachServer;

// Represents an Opus transcode job
#define OPUS_MAX_INLINE_PAYLOAD 4096

typedef struct {
    MachSession *session;
    uint32_t     sequence_id;
    uint32_t     command;
    void        *payload;
    size_t       payload_len;
    uint8_t      inline_payload[OPUS_MAX_INLINE_PAYLOAD];
} OpusJob;

// Initialize the global Opus thread pool
// event_fd is used to signal the io_uring loop that jobs are completed
int opus_pool_init(int num_threads, int event_fd);

// Shutdown the global Opus thread pool
void opus_pool_shutdown(void);

// Enqueue a job for a specific session
int opus_pool_enqueue_job(
    MachSession *session,
    uint32_t     sequence_id,
    uint32_t     command,
    const void  *payload,
    size_t       payload_len);

// Register a session with the pool (so it can be fair-scheduled)
void opus_pool_register_session(MachSession *session);

// Unregister a session (when it closes)
void opus_pool_unregister_session(MachSession *session);

// Type for a completed job
typedef struct {
    MachSession *session;
    uint32_t     sequence_id;
    int          error_code; // 0 on success
    void        *out_data;
    size_t       out_len;
    uint64_t     duration_ns;
    uint8_t      inline_out_data[OPUS_MAX_INLINE_PAYLOAD];
} OpusCompletedJob;

// Dequeue a completed job from the MPSC completion queue. Returns true if a job was dequeued.
bool opus_pool_dequeue_completed(OpusCompletedJob *job);

// Free the output data buffer of a completed job
void opus_pool_free_completed_job(OpusCompletedJob *job);

#endif // MACHAUDIO_OPUS_POOL_H
