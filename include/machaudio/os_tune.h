#ifndef MACHAUDIO_OS_TUNE_H
#define MACHAUDIO_OS_TUNE_H

/**
 * Pins the current thread to the specified CPU core.
 * core_id: The logical core ID to pin to.
 * Returns 0 on success, -1 on failure.
 */
int os_pin_thread(int core_id);

/**
 * Sets the current thread to real-time priority (SCHED_FIFO).
 * priority: The priority value (1-99).
 * Returns 0 on success, -1 on failure.
 */
int os_set_rt_priority(int priority);

/**
 * Attempts to set the scaling governor for a specific CPU to 'performance'.
 * core_id: The logical core ID.
 * Returns 0 on success, -1 on failure.
 */
int os_tune_cpu_governor(int core_id);

/**
 * Restores the scaling governor for a specific CPU to its original value.
 * core_id: The logical core ID.
 * Returns 0 on success, -1 on failure.
 */
int os_restore_cpu_governor(int core_id);

/**
 * Locks the CPU DMA latency to 0 to prevent deep C-states.
 * Returns a file descriptor on success, -1 on failure.
 * The lock is held as long as the FD remains open.
 */
int os_lock_dma_latency(void);

/**
 * Attempts to raise the file descriptor limit (RLIMIT_NOFILE).
 * limit: The desired minimum soft limit.
 * Returns 0 on success, -1 on failure.
 */
int os_set_fd_limit(int limit);

#endif // MACHAUDIO_OS_TUNE_H
