#ifndef MACHAUDIO_MACROS_H
#define MACHAUDIO_MACROS_H

#include <stdint.h>
#include <time.h>

/**
 * Branch prediction hints for the compiler.
 */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/**
 * High-resolution monotonic timestamp in nanoseconds.
 */
static inline uint64_t mach_hrtime(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

#endif // MACHAUDIO_MACROS_H
