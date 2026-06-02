#ifndef MACHAUDIO_LOG_H
#define MACHAUDIO_LOG_H

#include <signal.h>
#include <stdio.h>

/**
 * @file log.h
 * @brief Logging macros for MachAudio.
 *
 * Provides LOGINF, LOGERR, and LOGDBG macros.
 * LOGDBG is only active when DEBUG is defined.
 */

static inline char const *log_get_signal_name(int signum) {
    switch (signum) {
    case SIGINT:
        return "SIGINT";
    case SIGTERM:
        return "SIGTERM";
    case SIGQUIT:
        return "SIGQUIT";
    case SIGHUP:
        return "SIGHUP";
    case SIGKILL:
        return "SIGKILL";
    case SIGUSR1:
        return "SIGUSR1";
    case SIGUSR2:
        return "SIGUSR2";
    default:
        return "UNKNOWN";
    }
}

#define LOGINF(...)                                                                                \
    do {                                                                                           \
        fprintf(stdout, "[INFO] [%s:%d] ", __FILE__, __LINE__);                                    \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)

#define LOGINF_LOC(file, line, ...)                                                                \
    do {                                                                                           \
        fprintf(stdout, "[INFO] [%s:%d] ", file, line);                                            \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)

#define LOGERR(...)                                                                                \
    do {                                                                                           \
        fprintf(stderr, "[ERRO] [%s:%d] ", __FILE__, __LINE__);                                    \
        fprintf(stderr, __VA_ARGS__);                                                              \
        fprintf(stderr, "\n");                                                                     \
        fflush(stderr);                                                                            \
    } while (0)

#define LOGERR_LOC(file, line, ...)                                                                \
    do {                                                                                           \
        fprintf(stderr, "[ERRO] [%s:%d] ", file, line);                                            \
        fprintf(stderr, __VA_ARGS__);                                                              \
        fprintf(stderr, "\n");                                                                     \
        fflush(stderr);                                                                            \
    } while (0)

#if defined(LOG_DEBUG)
#define LOGDBG(...)                                                                                \
    do {                                                                                           \
        fprintf(stdout, "[DBUG]\t[%s:%d] ", __FILE__, __LINE__);                                   \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)

#define LOGDBG_LOC(file, line, ...)                                                                \
    do {                                                                                           \
        fprintf(stdout, "[DBUG]\t[%s:%d] ", file, line);                                           \
        fprintf(stdout, __VA_ARGS__);                                                              \
        fprintf(stdout, "\n");                                                                     \
        fflush(stdout);                                                                            \
    } while (0)
#else
#define LOGDBG(...)                                                                                \
    do {                                                                                           \
    } while (0)

#define LOGDBG_LOC(file, line, ...)                                                                \
    do {                                                                                           \
    } while (0)
#endif

#endif // MACHAUDIO_LOG_H
