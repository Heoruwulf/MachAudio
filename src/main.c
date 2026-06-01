#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "machaudio/log.h"
#include "machaudio/macros.h"
#include "machaudio/opus_pool.h"
#include "machaudio/os_tune.h"
#include "machaudio/server.h"
#include "processing/vad_training_loader.h"

#define DEFAULT_SOCKET_DIR  "/tmp"
#define DEFAULT_SOCKET_NAME "machaudio"

// Supervisory logic to identify and tune the kernel SQPOLL thread
static void tune_sqpoll_thread(pid_t worker_pid, int rt_priority, int core) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/task", worker_pid);

    for (int attempt = 0; attempt < 10; attempt++) {
        DIR *dir = opendir(path);
        if (!dir)
            return;

        bool           found = false;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.')
                continue;

            char comm_path[1024];
            snprintf(comm_path, sizeof(comm_path), "%s/%s/comm", path, ent->d_name);
            FILE *f = fopen(comm_path, "r");
            if (f) {
                char comm[256] = {0};
                if (fgets(comm, sizeof(comm), f)) {
                    if (strncmp(comm, "iou-sqp", 7) == 0) {
                        pid_t tid = atoi(ent->d_name);
                        LOGINF("Found SQPOLL thread %d for worker %d, tuning...", tid, worker_pid);

                        if (rt_priority > 1) {
                            // Give SQPOLL slightly lower priority than the worker so the worker can
                            // preempt it
                            struct sched_param sp = {.sched_priority = rt_priority - 1};
                            if (sched_setscheduler(tid, SCHED_FIFO, &sp) < 0) {
                                LOGERR(
                                    "Failed to set SCHED_FIFO for SQPOLL thread %d: %s",
                                    tid,
                                    strerror(errno));
                            }
                        }

                        if (core != -1) {
                            cpu_set_t cpuset;
                            CPU_ZERO(&cpuset);
                            CPU_SET(core, &cpuset);
                            if (sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset) < 0) {
                                LOGERR(
                                    "Failed to set CPU affinity for SQPOLL thread %d: %s",
                                    tid,
                                    strerror(errno));
                            }
                        }
                        found = true;
                    }
                }
                fclose(f);
            }
        }
        closedir(dir);
        if (found)
            break;
        usleep(50000); // 50ms wait
    }
}

static void print_usage(char const *const prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c, --core-mask <mask|csv> Core mask (e.g., 0x15 or 0,2,4) (default: 0)\n");
    fprintf(stderr, "  -p, --rt-priority <N> Set SCHED_FIFO priority N (1-99)\n");
    fprintf(stderr, "  -H, --host <IP>       TCP host to listen on (activates TCP mode)\n");
    fprintf(stderr, "  -P, --port <N>        TCP base port (default: 8000)\n");
    fprintf(stderr, "  -D, --uds-dir <dir>   Directory for UDS sockets (default: /tmp)\n");
    fprintf(stderr, "  -L, --fd-limit <N>    Set max open files limit (default: 4096)\n");
    fprintf(stderr, "  -v, --vad-data <path> Custom VAD training data file (overrides env var)\n");
    fprintf(stderr, "  -o, --enable-opus     Enable Opus Fair-Scheduling mode\n");
    fprintf(
        stderr,
        "  -t, --opus-threads <N> Number of threads for the Opus pool (default: CPU cores)\n");
    fprintf(stderr, "  -h, --help            Show this help message\n");
}

static void parse_core_mask(const char *mask_str, int **cores, int *num_cores) {
    if (!mask_str) {
        *cores      = malloc(sizeof(int));
        (*cores)[0] = 0;
        *num_cores  = 1;
        return;
    }

    if (strncmp(mask_str, "0x", 2) == 0) {
        unsigned long long mask  = strtoull(mask_str, NULL, 16);
        int                count = 0;
        for (int i = 0; i < 64; i++) {
            if ((mask & (1ULL << i)) != 0) {
                count++;
            }
        }
        *cores = malloc(sizeof(int) * (count > 0 ? count : 1));
        if (count == 0) {
            (*cores)[0] = 0;
            *num_cores  = 1;
            return;
        }
        int idx = 0;
        for (int i = 0; i < 64; i++) {
            if ((mask & (1ULL << i)) != 0) {
                (*cores)[idx++] = i;
            }
        }
        *num_cores = count;
    } else if (strcmp(mask_str, "none") == 0) {
        *cores      = malloc(sizeof(int));
        (*cores)[0] = -1;
        *num_cores  = 1;
    } else {
        char *str   = strdup(mask_str);
        int   count = 0;
        for (char *p = str; *p; p++) {
            if (*p == ',')
                count++;
        }
        count++; // At least 1 item

        *cores      = malloc(sizeof(int) * count);
        int   idx   = 0;
        char *token = strtok(str, ",");
        while (token) {
            if (strcmp(token, "none") == 0) {
                (*cores)[idx++] = -1;
            } else {
                (*cores)[idx++] = atoi(token);
            }
            token = strtok(NULL, ",");
        }
        *num_cores = idx;
        free(str);
    }
}

int run_worker(
    int         worker_index,
    int         assigned_core,
    int         rt_priority,
    char const *host,
    int         base_port,
    char const *uds_dir,
    uint32_t    num_workers,
    bool        enable_opus,
    uint32_t    opus_threads) {
    LOGINF("Worker %d (PID %d) starting...", worker_index, getpid());

    // Apply OS tuning
    if (assigned_core != -1) {
        os_pin_thread(assigned_core);
    }

    if (rt_priority > 0) {
        os_set_rt_priority(rt_priority);
    }

    // High-performance tuning: Attempt to set governor and lock latency
    if (assigned_core != -1) {
        os_tune_cpu_governor(assigned_core);
    }
    int dma_latency_fd = os_lock_dma_latency();

    MachServer server;
    char       socket_path[256];
    int        r;

    if (host) {
        r = mach_server_init(
            &server,
            NULL,
            host,
            base_port + worker_index,
            num_workers,
            assigned_core,
            enable_opus,
            opus_threads);
    } else {
        if (enable_opus) {
            snprintf(
                socket_path,
                sizeof(socket_path),
                "%s/%s.opus.sock",
                uds_dir,
                DEFAULT_UDS_NAME);
        } else {
            snprintf(
                socket_path,
                sizeof(socket_path),
                "%s/%s.%d.sock",
                uds_dir,
                DEFAULT_UDS_NAME,
                worker_index);
        }
        r = mach_server_init(
            &server,
            socket_path,
            NULL,
            0,
            num_workers,
            assigned_core,
            enable_opus,
            opus_threads);
    }

    if (r != 0) {
        LOGERR("Worker %d failed to initialize server", worker_index);
        if (dma_latency_fd >= 0) {
            close(dma_latency_fd);
        }
        if (assigned_core != -1) {
            os_restore_cpu_governor(assigned_core);
        }
        return 1;
    }

    r = mach_server_start(&server);
    LOGINF("Worker %d stopping.", worker_index);

    if (enable_opus) {
        opus_pool_shutdown();
    }

    if (dma_latency_fd >= 0) {
        close(dma_latency_fd);
    }

    if (assigned_core != -1) {
        os_restore_cpu_governor(assigned_core);
    }

    return r;
}

int main(int argc, char **argv) {
    // Ignore SIGPIPE to prevent the server from crashing when writing to a socket that the client
    // closed abruptly
    signal(SIGPIPE, SIG_IGN);

    char const *core_mask_str = mach_getenv("MACH_CORE_MASK");

    int         rt_priority = -1;
    char const *rt_env      = mach_getenv("MACH_RT_PRIORITY");
    if (rt_env)
        rt_priority = atoi(rt_env);

    int         fd_limit = 4096;
    char const *fd_env   = mach_getenv("MACH_FD_LIMIT");
    if (fd_env)
        fd_limit = atoi(fd_env);

    char        host_buf[256] = {0};
    char const *host          = mach_getenv("MACH_HOST");
    if (host) {
        strncpy(host_buf, host, sizeof(host_buf) - 1);
        host_buf[sizeof(host_buf) - 1] = '\0';
        host                           = host_buf;
    }

    int         base_port = 8000;
    char const *port_env  = mach_getenv("MACH_PORT");
    if (port_env)
        base_port = atoi(port_env);

    char const *uds_dir = mach_getenv("MACH_UDS_DIR");
    if (!uds_dir)
        uds_dir = DEFAULT_SOCKET_DIR;

    char const *vad_path = mach_getenv("MACH_VAD_DATA");

    bool        enable_opus = false;
    char const *opus_env    = mach_getenv("MACH_ENABLE_OPUS");
    if (opus_env && (strcmp(opus_env, "1") == 0 || strcasecmp(opus_env, "true") == 0)) {
        enable_opus = true;
    }

    uint32_t    opus_threads     = 0;
    char const *opus_threads_env = mach_getenv("MACH_OPUS_THREADS");
    if (opus_threads_env) {
        opus_threads = (uint32_t)atoi(opus_threads_env);
    }

    static struct option const long_options[] = {
        {"core-mask", required_argument, 0, 'c'},
        {"rt-priority", required_argument, 0, 'p'},
        {"host", required_argument, 0, 'H'},
        {"port", required_argument, 0, 'P'},
        {"uds-dir", required_argument, 0, 'D'},
        {"fd-limit", required_argument, 0, 'L'},
        {"vad-data", required_argument, 0, 'v'},
        {"enable-opus", no_argument, 0, 'o'},
        {"opus-threads", required_argument, 0, 't'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:H:P:D:L:v:ot:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'c':
            core_mask_str = optarg;
            break;
        case 'p':
            rt_priority = atoi(optarg);
            break;
        case 'H':
            strncpy(host_buf, optarg, sizeof(host_buf) - 1);
            host_buf[sizeof(host_buf) - 1] = '\0';
            host                           = host_buf;
            break;
        case 'P':
            base_port = atoi(optarg);
            break;
        case 'D':
            uds_dir = optarg;
            break;
        case 'L':
            fd_limit = atoi(optarg);
            break;
        case 'v':
            vad_path = optarg;
            break;
        case 'o':
            enable_opus = true;
            break;
        case 't':
            opus_threads = (uint32_t)atoi(optarg);
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
        }
    }

    int *cores       = NULL;
    int  num_workers = 1;
    parse_core_mask(core_mask_str, &cores, &num_workers);

    if (enable_opus) {
        LOGINF("Opus mode enabled. Forcing single worker process and disabling core-pinning.");
        num_workers = 1;
        cores[0]    = -1;
        if (opus_threads == 0) {
            opus_threads = sysconf(_SC_NPROCESSORS_ONLN);
            if (opus_threads < 4)
                opus_threads = 4;
        }
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    char cores_log[512] = {0};
    int  offset         = 0;
    for (int i = 0; i < num_workers; i++) {
        int written = 0;
        if (cores[i] == -1) {
            written = snprintf(
                cores_log + offset,
                sizeof(cores_log) - offset,
                "none%s",
                i < num_workers - 1 ? ", " : "");
        } else {
            written = snprintf(
                cores_log + offset,
                sizeof(cores_log) - offset,
                "%d%s",
                cores[i],
                i < num_workers - 1 ? ", " : "");
        }
        if (written > 0 && offset + written < (int)sizeof(cores_log)) {
            offset += written;
        }
    }
    LOGINF(
        "MachAudio Supervisor Service Starting with %d workers on cores: [%s]",
        num_workers,
        cores_log);
#ifdef __AVX2__
    LOGINF("SIMD: AVX2 extensions enabled");
#else
    LOGINF("SIMD: Scalar (AVX2 disabled)");
#endif
    os_set_fd_limit(fd_limit);

    // Environment variable is already loaded; args override it.
    VadTrainingData vad_data = vad_training_data_load(vad_path);

    pid_t *worker_pids = calloc((size_t)num_workers, sizeof(pid_t));
    if (!worker_pids)
        return 1;

    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            int r = run_worker(
                i,
                cores[i],
                rt_priority,
                host,
                base_port,
                uds_dir,
                (uint32_t)num_workers,
                enable_opus,
                opus_threads);
            vad_training_data_free(&vad_data);
            free(worker_pids);
            free(cores);
            exit(r);
        } else if (pid > 0) {
            worker_pids[i] = pid;
        } else {
            LOGERR("Fork failed for worker %d", i);
        }
    }

    LOGINF("Supervisor process with %d workers", num_workers);

    for (int i = 0; i < num_workers; i++) {
        tune_sqpoll_thread(worker_pids[i], rt_priority, cores[i]);
    }

    int active_workers = num_workers;
    while (active_workers > 0) {
        int   status;
        pid_t pid = wait(&status);
        if (pid > 0) {
            LOGINF("Worker process %d exited", pid);
            active_workers--;
        }
    }

    LOGINF("All workers finished. Supervisor exiting.");
    vad_training_data_free(&vad_data);
    free(worker_pids);
    free(cores);
    return 0;
}
