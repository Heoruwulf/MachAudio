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

#include <stdlib.h>

/**
 * Gets an environment variable and returns NULL if it is unset or an empty string.
 */
static inline char const *mach_getenv(char const *name) {
    char const *val = getenv(name);
    if (val && val[0] == '\0') {
        return NULL;
    }
    return val;
}

#endif // MACHAUDIO_MACROS_H
