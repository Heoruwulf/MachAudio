#ifndef MACHAUDIO_MACROS_H
#define MACHAUDIO_MACROS_H

/**
 * Branch prediction hints for the compiler.
 */
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

#endif // MACHAUDIO_MACROS_H
