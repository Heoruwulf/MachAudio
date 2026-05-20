#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <uv.h>
#include "machaudio/log.h"
#include "machaudio/os_tune.h"
#include "machaudio/server.h"

#define DEFAULT_SOCKET_DIR  "/tmp"
#define DEFAULT_SOCKET_NAME "machaudio"

static void on_signal(uv_signal_t *handle, int signum) {
    LOGINF("Received signal %s, shutting down...", log_get_signal_name(signum));
    MachServer *const server = (MachServer *)handle->data;
    mach_server_stop(server);
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

    uv_loop_t *const loop = uv_default_loop();
    MachServer       server;
    char             socket_path[256];
    int              r;

    if (host) {
        r = mach_server_init(&server, loop, NULL, host, base_port + worker_index, num_workers);
    } else {
        snprintf(
            socket_path,
            sizeof(socket_path),
            "%s/%s.%d.sock",
            uds_dir,
            DEFAULT_UDS_NAME,
            worker_index);
        r = mach_server_init(&server, loop, socket_path, NULL, 0, num_workers);
    }

    if (r != 0) {
        LOGERR("Worker %d failed to initialize server", worker_index);
        return 1;
    }

    if (mach_server_start(&server) != 0) {
        LOGERR("Worker %d failed to start server", worker_index);
        return 1;
    }

    uv_signal_t sig_int, sig_term;
    uv_signal_init(loop, &sig_int);
    sig_int.data = &server;
    uv_signal_start(&sig_int, on_signal, SIGINT);
    uv_signal_init(loop, &sig_term);
    sig_term.data = &server;
    uv_signal_start(&sig_term, on_signal, SIGTERM);

    r = uv_run(loop, UV_RUN_DEFAULT);
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
            return 1;
        }
    }

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
