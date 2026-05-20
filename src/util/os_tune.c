#include "machaudio/os_tune.h"
#include "machaudio/log.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

int os_pin_thread(int const core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);

    if (sched_setaffinity(0, sizeof(cpu_set_t), &cpuset) != 0) {
        LOGERR("Failed to set CPU affinity: %s", strerror(errno));
        return -1;
    }
    LOGINF("Pinned thread to CPU core %d", core_id);
    return 0;
}

/**
 * Sets the scheduling policy to SCHED_FIFO (real-time) with the given priority.
 *
 * Privilege Requirements (Non-Root):
 * Requires the CAP_SYS_NICE capability to set real-time priorities.
 * Execute: sudo setcap 'cap_sys_nice=ep' ./build/bin/machaudio
 */
int os_set_rt_priority(int const priority) {
    struct sched_param param;
    param.sched_priority = priority;

    if (sched_setscheduler(0, SCHED_FIFO, &param) != 0) {
        LOGERR(
            "Failed to set RT priority (%d): %s. (Requires CAP_SYS_NICE)",
            priority,
            strerror(errno));
        return -1;
    }
    LOGINF("Set thread scheduling to SCHED_FIFO, priority %d", priority);
    return 0;
}

#define MAX_CORES 256
static char g_original_governors[MAX_CORES][32];
static int  g_governor_modified[MAX_CORES] = {0};

/**
 * Attempts to set the scaling governor for a specific CPU to 'performance'.
 *
 * Privilege Requirements (Non-Root):
 * Many systems restrict writes to the sysfs cpufreq path.
 * If running as a non-root user, you may need a custom udev rule or
 * elevated permissions on the specific sysfs path to succeed without root.
 */
int os_tune_cpu_governor(int const core_id) {
    if (core_id < 0 || core_id >= MAX_CORES) {
        LOGERR("Invalid core ID for governor tuning: %d", core_id);
        return -1;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", core_id);

    // 1. Read and save original governor
    FILE *f = fopen(path, "r");
    if (!f) {
        LOGERR("Could not open %s for reading: %s", path, strerror(errno));
        return -1;
    }

    if (fgets(g_original_governors[core_id], sizeof(g_original_governors[core_id]), f) == NULL) {
        LOGERR("Failed to read original governor from %s", path);
        fclose(f);
        return -1;
    }
    fclose(f);

    // Strip newline if present
    char *newline = strchr(g_original_governors[core_id], '\n');
    if (newline) {
        *newline = '\0';
    }

    // 2. Set to performance
    f = fopen(path, "w");
    if (!f) {
        LOGERR("Could not open %s for writing: %s", path, strerror(errno));
        return -1;
    }

    if (fprintf(f, "performance") < 0) {
        LOGERR("Failed to write to %s: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }

    fclose(f);
    g_governor_modified[core_id] = 1;
    LOGINF(
        "Set CPU %d governor to 'performance' (was '%s')",
        core_id,
        g_original_governors[core_id]);
    return 0;
}

int os_restore_cpu_governor(int const core_id) {
    if (core_id < 0 || core_id >= MAX_CORES || !g_governor_modified[core_id]) {
        return 0;
    }

    char path[128];
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_governor", core_id);

    FILE *f = fopen(path, "w");
    if (!f) {
        LOGERR("Could not open %s for restoration: %s", path, strerror(errno));
        return -1;
    }

    if (fprintf(f, "%s", g_original_governors[core_id]) < 0) {
        LOGERR("Failed to restore governor to %s: %s", path, strerror(errno));
        fclose(f);
        return -1;
    }

    fclose(f);
    g_governor_modified[core_id] = 0;
    LOGINF("Restored CPU %d governor to '%s'", core_id, g_original_governors[core_id]);
    return 0;
}

/**
 * Locks the CPU DMA latency to 0 to prevent deep C-states (micro-sleep).
 * The lock remains active as long as the file descriptor is kept open.
 *
 * Privilege Requirements (Non-Root):
 * By default, only root can write to /dev/cpu_dma_latency.
 * To use this as a normal user, modify the permissions (e.g., sudo chmod 0666 /dev/cpu_dma_latency)
 * or create a persistent udev rule:
 * ACTION=="add", SUBSYSTEM=="misc", KERNEL=="cpu_dma_latency", MODE="0666"
 */
int os_lock_dma_latency(void) {
    int fd = open("/dev/cpu_dma_latency", O_WRONLY);
    if (fd < 0) {
        LOGERR("Could not open /dev/cpu_dma_latency: %s", strerror(errno));
        return -1;
    }

    int32_t latency = 0;
    if (write(fd, &latency, sizeof(latency)) != sizeof(latency)) {
        LOGERR("Failed to write to /dev/cpu_dma_latency: %s", strerror(errno));
        close(fd);
        return -1;
    }

    LOGINF("Locked CPU DMA latency to 0 (preventing deep C-states)");
    return fd;
}

int os_set_fd_limit(int const limit) {
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0) {
        LOGERR("Failed to get RLIMIT_NOFILE: %s", strerror(errno));
        return -1;
    }

    LOGINF("Current RLIMIT_NOFILE: soft=%ld, hard=%ld", (long)rl.rlim_cur, (long)rl.rlim_max);

    if (rl.rlim_cur < (rlim_t)limit) {
        rl.rlim_cur = (rlim_t)limit;
        if (rl.rlim_max < (rlim_t)limit) {
            rl.rlim_max = (rlim_t)limit;
        }

        if (setrlimit(RLIMIT_NOFILE, &rl) != 0) {
            LOGERR("Failed to raise RLIMIT_NOFILE to %d: %s", limit, strerror(errno));
            return -1;
        }
        LOGINF("Raised RLIMIT_NOFILE to %d", limit);
    }
    return 0;
}
