#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <sched.h>
#include <string.h>
#include <errno.h>
#include "machaudio/log.h"
#include "machaudio/os_tune.h"
#include "machaudio/server.h"

#define DEFAULT_SOCKET_DIR  "/tmp"
#define DEFAULT_SOCKET_NAME "machaudio"

// Supervisory logic to identify and tune the kernel SQPOLL thread
static void tune_sqpoll_thread(pid_t worker_pid, int rt_priority, int core) {
    char path[256];
    snprintf(path, sizeof(path), "/proc/%d/task", worker_pid);
    
    for (int attempt = 0; attempt < 10; attempt++) {
        DIR *dir = opendir(path);
        if (!dir) return;

        bool found = false;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            
            char comm_path[1024];
            snprintf(comm_path, sizeof(comm_path), "%s/%s/comm", path, ent->d_name);
            FILE *f = fopen(comm_path, "r");
            if (f) {
                char comm[256] = {0};
                if (fgets(comm, sizeof(comm), f)) {
                    if (strncmp(comm, "iou-sqp", 7) == 0) {
                        pid_t tid = atoi(ent->d_name);
                        LOGINF("Found SQPOLL thread %d for worker %d, tuning...", tid, worker_pid);
                        
                        if (rt_priority > 0) {
                            struct sched_param sp = { .sched_priority = rt_priority };
                            if (sched_setscheduler(tid, SCHED_FIFO, &sp) < 0) {
                                LOGERR("Failed to set SCHED_FIFO for SQPOLL thread %d: %s", tid, strerror(errno));
                            }
                        }
                        
                        cpu_set_t cpuset;
                        CPU_ZERO(&cpuset);
                        CPU_SET(core, &cpuset);
                        if (sched_setaffinity(tid, sizeof(cpu_set_t), &cpuset) < 0) {
                            LOGERR("Failed to set CPU affinity for SQPOLL thread %d: %s", tid, strerror(errno));
                        }
                        found = true;
                    }
                }
                fclose(f);
            }
        }
        closedir(dir);
        if (found) break;
        usleep(50000); // 50ms wait
    }
}

static void print_usage(char const *const prog) {
    fprintf(stderr, "Usage: %s [options]\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c, --cpu-core <N>    Base CPU core for pinning (default: 0)\n");
    fprintf(stderr, "  -p, --rt-priority <N> Set SCHED_FIFO priority N (1-99)\n");
    fprintf(stderr, "  -w, --workers <N>     Number of worker processes to fork (default: 1)\n");
    fprintf(stderr, "  -H, --host <IP>       TCP host to listen on (activates TCP mode)\n");
    fprintf(stderr, "  -P, --port <N>        TCP base port (default: 8000)\n");
    fprintf(stderr, "  -D, --uds-dir <dir>   Directory for UDS sockets (default: /tmp)\n");
    fprintf(stderr, "  -L, --fd-limit <N>    Set max open files limit (default: 4096)\n");
    fprintf(stderr, "  -h, --help            Show this help message\n");
}

int run_worker(
    int         worker_index,
    int         base_core,
    int         rt_priority,
    char const *host,
    int         base_port,
    char const *uds_dir,
    uint32_t    num_workers) {
    LOGINF("Worker %d (PID %d) starting...", worker_index, getpid());

    // Apply OS tuning
    os_pin_thread(base_core + worker_index);
    if (rt_priority > 0) {
        os_set_rt_priority(rt_priority);
    }

    // High-performance tuning: Attempt to set governor and lock latency
    os_tune_cpu_governor(base_core + worker_index);
    int dma_latency_fd = os_lock_dma_latency();

    MachServer server;
    char       socket_path[256];
    int        r;

    if (host) {
        r = mach_server_init(&server, NULL, host, base_port + worker_index, num_workers, base_core + worker_index);
    } else {
        snprintf(
            socket_path,
            sizeof(socket_path),
            "%s/%s.%d.sock",
            uds_dir,
            DEFAULT_UDS_NAME,
            worker_index);
        r = mach_server_init(&server, socket_path, NULL, 0, num_workers, base_core + worker_index);
    }

    if (r != 0) {
        LOGERR("Worker %d failed to initialize server", worker_index);
        if (dma_latency_fd >= 0) {
            close(dma_latency_fd);
        }
        os_restore_cpu_governor(base_core + worker_index);
        return 1;
    }

    r = mach_server_start(&server);
    LOGINF("Worker %d stopping.", worker_index);

    if (dma_latency_fd >= 0) {
        close(dma_latency_fd);
    }

    os_restore_cpu_governor(base_core + worker_index);

    return r;
}

int main(int argc, char **argv) {
    int         cpu_core      = 0;
    int         rt_priority   = -1;
    int         num_workers   = 1;
    int         fd_limit      = 4096;
    char        host_buf[256] = {0};
    char const *host          = NULL;
    int         base_port     = 8000;
    char const *uds_dir       = DEFAULT_SOCKET_DIR;

    static struct option const long_options[] = {
        {"cpu-core", required_argument, 0, 'c'},
        {"rt-priority", required_argument, 0, 'p'},
        {"workers", required_argument, 0, 'w'},
        {"host", required_argument, 0, 'H'},
        {"port", required_argument, 0, 'P'},
        {"uds-dir", required_argument, 0, 'D'},
        {"fd-limit", required_argument, 0, 'L'},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};

    int opt;
    while ((opt = getopt_long(argc, argv, "c:p:w:H:P:D:L:h", long_options, NULL)) != -1) {
        switch (opt) {
        case 'c':
            cpu_core = atoi(optarg);
            break;
        case 'p':
            rt_priority = atoi(optarg);
            break;
        case 'w':
            num_workers = atoi(optarg);
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
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
        }
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);

    LOGINF("MachAudio Supervisor Service Starting...");
#ifdef __AVX2__
    LOGINF("SIMD: AVX2 extensions enabled");
#else
    LOGINF("SIMD: Scalar (AVX2 disabled)");
#endif
    os_set_fd_limit(fd_limit);

    pid_t *worker_pids = calloc((size_t)num_workers, sizeof(pid_t));
    if (!worker_pids)
        return 1;

    for (int i = 0; i < num_workers; i++) {
        pid_t pid = fork();
        if (pid == 0) {
            // Child process
            int r = run_worker(
                i,
                cpu_core,
                rt_priority,
                host,
                base_port,
                uds_dir,
                (uint32_t)num_workers);
            free(worker_pids);
            exit(r);
        } else if (pid > 0) {
            worker_pids[i] = pid;
        } else {
            LOGERR("Fork failed for worker %d", i);
        }
    }

    LOGINF("Supervisor process with %d workers", num_workers);

    for (int i = 0; i < num_workers; i++) {
        tune_sqpoll_thread(worker_pids[i], rt_priority, cpu_core + i);
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
    free(worker_pids);
    return 0;
}
