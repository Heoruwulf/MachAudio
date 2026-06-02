#include "machaudio/opus_pool.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include "machaudio/log.h"
#include "machaudio/server.h"

// The maximum number of concurrent audio sessions that the thread pool can process.
#define MAX_ACTIVE_SESSIONS 4096

// The maximum number of pending tasks per session queue. Since jobs are grouped by session,
// this limits the backlog size for an individual session.
#define SESSION_QUEUE_CAPACITY 64

// The maximum number of processed jobs waiting to be sent back via io_uring globally.
// 4096 is chosen to comfortably accommodate peaks across all active sessions.
#define COMPLETED_QUEUE_CAPACITY 4096

typedef struct {
    OpusJob         jobs[SESSION_QUEUE_CAPACITY];
    int             head;
    int             tail;
    int             count;
    bool            is_processing;
    bool            in_ready_queue;
    pthread_mutex_t lock;
} SessionQueue;

typedef struct {
    OpusCompletedJob *jobs[COMPLETED_QUEUE_CAPACITY];
    int               head;
    int               tail;
    int               count;
    pthread_mutex_t   lock;
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

    MachSession *ready_queue[MAX_ACTIVE_SESSIONS];
    int          ready_head;
    int          ready_tail;
    int          ready_count;
} pool;

// Helper Functions

static void lower_worker_thread_priority(void) {
    struct sched_param param;
    int                policy = sched_getscheduler(0);
    if (policy == SCHED_FIFO || policy == SCHED_RR) {
        if (sched_getparam(0, &param) == 0) {
            if (param.sched_priority > 1) {
                param.sched_priority -= 1;
                sched_setscheduler(0, policy, &param);
            }
        }
    }
}

static void free_job_payload(OpusJob *job) {
    if (job->payload && job->payload != job->inline_payload) {
        free(job->payload);
    }
}

static void free_completed_job_payload(OpusCompletedJob *job) {
    if (job->out_data && job->out_data != job->inline_out_data) {
        free(job->out_data);
    }
}

static int try_dequeue_ready_job(OpusJob *job) {
    pthread_mutex_lock(&pool.lock);

    while (!pool.shutdown && pool.ready_count == 0) {
        pthread_cond_wait(&pool.cond, &pool.lock);
    }

    if (pool.shutdown && pool.ready_count == 0) {
        pthread_mutex_unlock(&pool.lock);
        return -1;
    }

    MachSession *session = pool.ready_queue[pool.ready_head];
    pool.ready_head      = (pool.ready_head + 1) % MAX_ACTIVE_SESSIONS;
    pool.ready_count--;
    pthread_mutex_unlock(&pool.lock);

    SessionQueue *sq = (SessionQueue *)session->opus_queue;
    if (!sq) {
        return 0;
    }

    bool found_job = false;
    pthread_mutex_lock(&sq->lock);
    sq->in_ready_queue = false;
    if (sq->count > 0 && !sq->is_processing) {
        OpusJob *src     = &sq->jobs[sq->head];
        job->session     = src->session;
        job->sequence_id = src->sequence_id;
        job->command     = src->command;
        job->payload_len = src->payload_len;
        if (src->payload == src->inline_payload) {
            memcpy(job->inline_payload, src->inline_payload, src->payload_len);
            job->payload = job->inline_payload;
        } else {
            job->payload = src->payload;
        }
        sq->head = (sq->head + 1) % SESSION_QUEUE_CAPACITY;
        sq->count--;
        sq->is_processing = true;
        found_job         = true;
    }
    pthread_mutex_unlock(&sq->lock);

    return found_job ? 1 : 0;
}

static void process_single_job(OpusJob *job, Arena *t_arena, OpusCompletedJob *cjob) {
    uint64_t start_ns = mach_hrtime();

    arena_reset(t_arena);

    struct audio_input_payload *payload = job->payload;

    int r = audio_process_transcode(
        &job->session->transcode_session,
        payload,
        job->payload_len,
        t_arena);

    uint64_t end_ns = mach_hrtime();

    size_t out_len    = arena_used(t_arena);
    cjob->session     = job->session;
    cjob->sequence_id = job->sequence_id;
    cjob->error_code  = r;
    cjob->out_len     = out_len;
    cjob->duration_ns = end_ns - start_ns;
    cjob->out_data    = NULL;

    if (r == 0 && out_len > 0) {
        if (out_len <= OPUS_MAX_INLINE_PAYLOAD) {
            memcpy(cjob->inline_out_data, t_arena->buf, out_len);
            cjob->out_data = cjob->inline_out_data;
        } else {
            LOGDBG(
                "Opus completed payload exceeds inline buffer, malloc fallback used (size: %zu)",
                out_len);
            void *out_data = malloc(out_len);
            if (out_data) {
                memcpy(out_data, t_arena->buf, out_len);
                cjob->out_data = out_data;
            } else {
                cjob->error_code = -1;
                cjob->out_data   = NULL;
            }
        }
    }
}

static void enqueue_completed_job(OpusCompletedJob *cjob) {
    OpusCompletedJob *pcjob = malloc(sizeof(OpusCompletedJob));
    if (!pcjob) {
        LOGERR("Failed to allocate OpusCompletedJob");
        free_completed_job_payload(cjob);
        return;
    }

    pcjob->session     = cjob->session;
    pcjob->sequence_id = cjob->sequence_id;
    pcjob->error_code  = cjob->error_code;
    pcjob->out_len     = cjob->out_len;
    pcjob->duration_ns = cjob->duration_ns;

    if (cjob->out_data == cjob->inline_out_data) {
        memcpy(pcjob->inline_out_data, cjob->inline_out_data, cjob->out_len);
        pcjob->out_data = pcjob->inline_out_data;
    } else {
        pcjob->out_data = cjob->out_data;
    }

    pthread_mutex_lock(&pool.completed_queue.lock);
    if (pool.completed_queue.count < COMPLETED_QUEUE_CAPACITY) {
        pool.completed_queue.jobs[pool.completed_queue.tail] = pcjob;
        pool.completed_queue.tail = (pool.completed_queue.tail + 1) % COMPLETED_QUEUE_CAPACITY;
        pool.completed_queue.count++;

        bool need_signal = (pool.completed_queue.count == 1);
        pthread_mutex_unlock(&pool.completed_queue.lock);

        if (need_signal) {
            uint64_t val = 1;
            ssize_t  s   = write(pool.event_fd, &val, sizeof(val));
            (void)s;
        }
    } else {
        pthread_mutex_unlock(&pool.completed_queue.lock);
        LOGERR("Opus completed queue is full! Dropping response.");
        free_completed_job_payload(pcjob);
        free(pcjob);
    }
}

static void finish_session_processing(MachSession *session) {
    SessionQueue *sq = (SessionQueue *)session->opus_queue;
    pthread_mutex_lock(&sq->lock);
    sq->is_processing  = false;
    bool needs_requeue = false;
    if (sq->count > 0 && !sq->in_ready_queue) {
        sq->in_ready_queue = true;
        needs_requeue      = true;
    }
    pthread_mutex_unlock(&sq->lock);

    if (needs_requeue) {
        pthread_mutex_lock(&pool.lock);
        pool.ready_queue[pool.ready_tail] = session;
        pool.ready_tail                   = (pool.ready_tail + 1) % MAX_ACTIVE_SESSIONS;
        pool.ready_count++;
        pthread_mutex_unlock(&pool.lock);
        pthread_cond_signal(&pool.cond);
    }
}

static void remove_session_from_active_list(MachSession *session) {
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
}

static void remove_session_from_ready_queue(MachSession *session) {
    int rq_count     = pool.ready_count;
    int read_idx     = pool.ready_head;
    int write_idx    = pool.ready_head;
    int new_rq_count = 0;

    for (int i = 0; i < rq_count; i++) {
        MachSession *s = pool.ready_queue[read_idx];
        if (s != session) {
            if (write_idx != read_idx) {
                pool.ready_queue[write_idx] = s;
            }
            write_idx = (write_idx + 1) % MAX_ACTIVE_SESSIONS;
            new_rq_count++;
        } else {
            if (s->opus_queue) {
                SessionQueue *sq = (SessionQueue *)s->opus_queue;
                pthread_mutex_lock(&sq->lock);
                sq->in_ready_queue = false;
                pthread_mutex_unlock(&sq->lock);
            }
        }
        read_idx = (read_idx + 1) % MAX_ACTIVE_SESSIONS;
    }
    pool.ready_tail  = write_idx;
    pool.ready_count = new_rq_count;
}

static void wait_and_purge_session_queues(MachSession *session) {
    if (!session->opus_queue) {
        return;
    }

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

    // Purge completed jobs for this session to prevent dangling pointers
    pthread_mutex_lock(&pool.completed_queue.lock);
    int q_count   = pool.completed_queue.count;
    int read_idx  = pool.completed_queue.head;
    int write_idx = pool.completed_queue.head;
    int new_count = 0;

    for (int i = 0; i < q_count; i++) {
        OpusCompletedJob *cjob = pool.completed_queue.jobs[read_idx];
        if (cjob->session == session) {
            free_completed_job_payload(cjob);
            free(cjob);
        } else {
            if (write_idx != read_idx) {
                pool.completed_queue.jobs[write_idx] = cjob;
            }
            write_idx = (write_idx + 1) % COMPLETED_QUEUE_CAPACITY;
            new_count++;
        }
        read_idx = (read_idx + 1) % COMPLETED_QUEUE_CAPACITY;
    }
    pool.completed_queue.tail  = write_idx;
    pool.completed_queue.count = new_count;
    pthread_mutex_unlock(&pool.completed_queue.lock);

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

// Public API

static void *worker_thread_main(void *arg) {
    (void)arg;

    lower_worker_thread_priority();

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
        int     status = try_dequeue_ready_job(&job);
        if (status == -1) {
            break;
        }
        if (status == 0) {
            continue;
        }

        OpusCompletedJob cjob;
        process_single_job(&job, &t_arena, &cjob);

        free_job_payload(&job);

        enqueue_completed_job(&cjob);

        finish_session_processing(job.session);
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
    remove_session_from_active_list(session);
    remove_session_from_ready_queue(session);
    pthread_mutex_unlock(&pool.lock);

    wait_and_purge_session_queues(session);
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

    bool needs_queue = false;
    if (!sq->is_processing && !sq->in_ready_queue) {
        sq->in_ready_queue = true;
        needs_queue        = true;
    }
    pthread_mutex_unlock(&sq->lock);

    if (needs_queue) {
        pthread_mutex_lock(&pool.lock);
        pool.ready_queue[pool.ready_tail] = session;
        pool.ready_tail                   = (pool.ready_tail + 1) % MAX_ACTIVE_SESSIONS;
        pool.ready_count++;
        pthread_mutex_unlock(&pool.lock);
        pthread_cond_signal(&pool.cond);
    }

    return 0;
}

OpusCompletedJob *opus_pool_dequeue_completed(void) {
    pthread_mutex_lock(&pool.completed_queue.lock);
    if (pool.completed_queue.count == 0) {
        pthread_mutex_unlock(&pool.completed_queue.lock);
        return NULL;
    }

    OpusCompletedJob *job     = pool.completed_queue.jobs[pool.completed_queue.head];
    pool.completed_queue.head = (pool.completed_queue.head + 1) % COMPLETED_QUEUE_CAPACITY;
    pool.completed_queue.count--;
    pthread_mutex_unlock(&pool.completed_queue.lock);

    return job;
}

int opus_pool_dequeue_completed_batch(OpusCompletedJob **jobs, int max_jobs) {
    pthread_mutex_lock(&pool.completed_queue.lock);
    int count = 0;
    while (pool.completed_queue.count > 0 && count < max_jobs) {
        jobs[count++]             = pool.completed_queue.jobs[pool.completed_queue.head];
        pool.completed_queue.head = (pool.completed_queue.head + 1) % COMPLETED_QUEUE_CAPACITY;
        pool.completed_queue.count--;
    }
    pthread_mutex_unlock(&pool.completed_queue.lock);
    return count;
}

void opus_pool_free_completed_job(OpusCompletedJob *job) {
    if (job) {
        free_completed_job_payload(job);
        free(job);
    }
}
