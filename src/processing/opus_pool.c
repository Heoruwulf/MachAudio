#include "machaudio/opus_pool.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "machaudio/log.h"
#include "machaudio/server.h"

// The maximum number of concurrent audio sessions that the thread pool can process.
#define MAX_ACTIVE_SESSIONS      4096

// The maximum number of pending tasks per session queue. Since jobs are grouped by session,
// this limits the backlog size for an individual session.
#define SESSION_QUEUE_CAPACITY   64

// The maximum number of processed jobs waiting to be sent back via io_uring globally.
// 4096 is chosen to comfortably accommodate peaks across all active sessions.
#define COMPLETED_QUEUE_CAPACITY 4096

typedef struct {
    OpusJob         jobs[SESSION_QUEUE_CAPACITY];
    int             head;
    int             tail;
    int             count;
    bool            is_processing;
    pthread_mutex_t lock;
} SessionQueue;

typedef struct {
    OpusCompletedJob jobs[COMPLETED_QUEUE_CAPACITY];
    int              head;
    int              tail;
    int              count;
    pthread_mutex_t  lock;
} CompletedQueue;

static struct {
    MachSession    *sessions[MAX_ACTIVE_SESSIONS];
    int             session_count;
    int             current_session_idx;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            shutdown;

    pthread_t *threads;
    int        num_threads;
    int        event_fd;

    CompletedQueue completed_queue;
} pool;

static void *worker_thread_main(void *arg) {
    (void)arg;
    LOGINF("Opus worker thread started");

    uint8_t *t_arena_buf = malloc(MACH_SESSION_ARENA_SIZE);
    if (!t_arena_buf) {
        LOGERR("Failed to allocate worker arena buffer");
        return NULL;
    }

    Arena t_arena;
    arena_init(&t_arena, t_arena_buf, MACH_SESSION_ARENA_SIZE, "opus_arena");

    while (true) {
        OpusJob job;
        bool    found_job = false;

        pthread_mutex_lock(&pool.lock);

        while (!pool.shutdown && !found_job) {
            if (pool.session_count == 0) {
                pthread_cond_wait(&pool.cond, &pool.lock);
                continue;
            }

            // Fair scheduler: Round robin over active sessions
            int start_idx = pool.current_session_idx;
            int i         = start_idx;
            do {
                MachSession  *session = pool.sessions[i];
                SessionQueue *sq      = (SessionQueue *)session->opus_queue;

                pthread_mutex_lock(&sq->lock);
                if (sq->count > 0 && !sq->is_processing) {
                    OpusJob *src = &sq->jobs[sq->head];
                    job          = *src;
                    if (src->payload == src->inline_payload) {
                        job.payload = job.inline_payload;
                    }
                    sq->head = (sq->head + 1) % SESSION_QUEUE_CAPACITY;
                    sq->count--;
                    sq->is_processing = true;
                    found_job         = true;
                }
                pthread_mutex_unlock(&sq->lock);

                i = (i + 1) % pool.session_count;
                if (found_job) {
                    pool.current_session_idx = i;
                    break;
                }
            } while (i != start_idx);

            if (!found_job) {
                pthread_cond_wait(&pool.cond, &pool.lock);
            }
        }

        if (pool.shutdown && !found_job) {
            pthread_mutex_unlock(&pool.lock);
            break;
        }

        pthread_mutex_unlock(&pool.lock);

        if (found_job) {
            // Process the job
            uint64_t start_ns = mach_hrtime();

            arena_reset(&t_arena);

            struct audio_input_payload *payload = job.payload;

            int r = audio_process_transcode(
                &job.session->transcode_session,
                payload,
                job.payload_len,
                &t_arena);

            uint64_t end_ns = mach_hrtime();

            size_t           out_len  = arena_used(&t_arena);
            void            *out_data = NULL;
            OpusCompletedJob cjob     = {
                    .session     = job.session,
                    .sequence_id = job.sequence_id,
                    .error_code  = r,
                    .out_len     = out_len,
                    .duration_ns = end_ns - start_ns};

            if (r == 0 && out_len > 0) {
                if (out_len <= OPUS_MAX_INLINE_PAYLOAD) {
                    memcpy(cjob.inline_out_data, t_arena.buf, out_len);
                    cjob.out_data = cjob.inline_out_data;
                } else {
                    LOGDBG(
                        "Opus completed payload exceeds inline buffer, malloc fallback used (size: "
                        "%zu)",
                        out_len);
                    out_data = malloc(out_len);
                    if (out_data) {
                        memcpy(out_data, t_arena.buf, out_len);
                        cjob.out_data = out_data;
                    } else {
                        cjob.error_code = -1;
                        cjob.out_data   = NULL;
                    }
                }
            } else {
                cjob.out_data = NULL;
            }

            if (job.payload && job.payload != job.inline_payload) {
                free(job.payload);
            }

            pthread_mutex_lock(&pool.completed_queue.lock);
            if (pool.completed_queue.count < COMPLETED_QUEUE_CAPACITY) {
                OpusCompletedJob *pcjob = &pool.completed_queue.jobs[pool.completed_queue.tail];
                *pcjob                  = cjob;
                if (cjob.out_data == cjob.inline_out_data) {
                    pcjob->out_data = pcjob->inline_out_data;
                }

                pool.completed_queue.tail =
                    (pool.completed_queue.tail + 1) % COMPLETED_QUEUE_CAPACITY;
                pool.completed_queue.count++;

                // Signal io_uring
                uint64_t val = 1;
                ssize_t  s   = write(pool.event_fd, &val, sizeof(val));
                (void)s;
            } else {
                LOGERR("Opus completed queue is full! Dropping response.");
                if (cjob.out_data && cjob.out_data != cjob.inline_out_data) {
                    free(cjob.out_data);
                }
            }
            pthread_mutex_unlock(&pool.completed_queue.lock);

            // Release session processing lock
            SessionQueue *sq = (SessionQueue *)job.session->opus_queue;
            pthread_mutex_lock(&sq->lock);
            sq->is_processing   = false;
            bool const has_more = (sq->count > 0);
            pthread_mutex_unlock(&sq->lock);

            if (has_more) {
                pthread_mutex_lock(&pool.lock);
                pthread_cond_signal(&pool.cond);
                pthread_mutex_unlock(&pool.lock);
            }
        }
    }

    free(t_arena_buf);
    LOGINF("Opus worker thread exiting");
    return NULL;
}

int opus_pool_init(int num_threads, int event_fd) {
    memset(&pool, 0, sizeof(pool));
    pthread_mutex_init(&pool.lock, NULL);
    pthread_cond_init(&pool.cond, NULL);
    pool.num_threads = num_threads;
    pool.event_fd    = event_fd;

    pthread_mutex_init(&pool.completed_queue.lock, NULL);

    pool.threads = calloc(num_threads, sizeof(pthread_t));
    if (!pool.threads)
        return -1;

    for (int i = 0; i < num_threads; i++) {
        pthread_create(&pool.threads[i], NULL, worker_thread_main, NULL);
    }

    LOGINF("Opus thread pool initialized with %d threads", num_threads);
    return 0;
}

void opus_pool_shutdown(void) {
    pthread_mutex_lock(&pool.lock);
    pool.shutdown = true;
    pthread_cond_broadcast(&pool.cond);
    pthread_mutex_unlock(&pool.lock);

    if (pool.threads) {
        for (int i = 0; i < pool.num_threads; i++) {
            pthread_join(pool.threads[i], NULL);
        }
        free(pool.threads);
        pool.threads = NULL;
    }
}

void opus_pool_register_session(MachSession *session) {
    if (!session->opus_queue) {
        SessionQueue *sq = calloc(1, sizeof(SessionQueue));
        pthread_mutex_init(&sq->lock, NULL);
        session->opus_queue = sq;
    }

    pthread_mutex_lock(&pool.lock);
    if (pool.session_count < MAX_ACTIVE_SESSIONS) {
        pool.sessions[pool.session_count++] = session;
    } else {
        LOGERR("Max active sessions reached in Opus pool!");
    }
    pthread_mutex_unlock(&pool.lock);
}

void opus_pool_unregister_session(MachSession *session) {
    pthread_mutex_lock(&pool.lock);
    for (int i = 0; i < pool.session_count; i++) {
        if (pool.sessions[i] == session) {
            pool.sessions[i] = pool.sessions[pool.session_count - 1];
            pool.session_count--;
            if (pool.current_session_idx >= pool.session_count && pool.session_count > 0) {
                pool.current_session_idx = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&pool.lock);

    if (session->opus_queue) {
        SessionQueue *sq = (SessionQueue *)session->opus_queue;

        // Wait until any active worker thread finishes processing this session
        while (1) {
            pthread_mutex_lock(&sq->lock);
            if (!sq->is_processing) {
                break;
            }
            pthread_mutex_unlock(&sq->lock);
            usleep(100);
        }

        // Free remaining payloads
        for (int i = 0; i < sq->count; i++) {
            int idx = (sq->head + i) % SESSION_QUEUE_CAPACITY;
            if (sq->jobs[idx].payload && sq->jobs[idx].payload != sq->jobs[idx].inline_payload) {
                free(sq->jobs[idx].payload);
            }
        }
        pthread_mutex_unlock(&sq->lock);
        pthread_mutex_destroy(&sq->lock);
        free(sq);
        session->opus_queue = NULL;
    }
}

int opus_pool_enqueue_job(
    MachSession *session,
    uint32_t     sequence_id,
    uint32_t     command,
    const void  *payload,
    size_t       payload_len) {
    SessionQueue *sq = (SessionQueue *)session->opus_queue;
    if (!sq)
        return -1;

    void *payload_copy = NULL;
    if (payload && payload_len > 0) {
        if (payload_len > OPUS_MAX_INLINE_PAYLOAD) {
            LOGDBG(
                "Opus payload exceeds inline buffer, malloc fallback used (size: %zu)",
                payload_len);
            payload_copy = malloc(payload_len);
            if (!payload_copy)
                return -1;
            memcpy(payload_copy, payload, payload_len);
        }
    }

    pthread_mutex_lock(&sq->lock);
    if (sq->count >= SESSION_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&sq->lock);
        free(payload_copy);
        LOGERR("Session Opus queue is full");
        return -1;
    }

    OpusJob *job     = &sq->jobs[sq->tail];
    job->session     = session;
    job->sequence_id = sequence_id;
    job->command     = command;
    job->payload_len = payload_len;

    if (payload && payload_len > 0) {
        if (payload_len <= OPUS_MAX_INLINE_PAYLOAD) {
            memcpy(job->inline_payload, payload, payload_len);
            job->payload = job->inline_payload;
        } else {
            job->payload = payload_copy;
        }
    } else {
        job->payload = NULL;
    }

    sq->tail = (sq->tail + 1) % SESSION_QUEUE_CAPACITY;
    sq->count++;
    pthread_mutex_unlock(&sq->lock);

    pthread_mutex_lock(&pool.lock);
    pthread_cond_signal(&pool.cond);
    pthread_mutex_unlock(&pool.lock);

    return 0;
}

bool opus_pool_dequeue_completed(OpusCompletedJob *job) {
    bool found = false;
    pthread_mutex_lock(&pool.completed_queue.lock);
    if (pool.completed_queue.count > 0) {
        OpusCompletedJob *src = &pool.completed_queue.jobs[pool.completed_queue.head];
        *job                  = *src;
        if (src->out_data == src->inline_out_data) {
            job->out_data = job->inline_out_data;
        }
        pool.completed_queue.head = (pool.completed_queue.head + 1) % COMPLETED_QUEUE_CAPACITY;
        pool.completed_queue.count--;
        found = true;
    }
    pthread_mutex_unlock(&pool.completed_queue.lock);
    return found;
}

void opus_pool_free_completed_job(OpusCompletedJob *job) {
    if (job->out_data && job->out_data != job->inline_out_data) {
        free(job->out_data);
    }
    job->out_data = NULL;
}
