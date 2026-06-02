#include "machaudio/arena.h"
#include "machaudio/log.h"

#include <string.h>

static char const *const DEFAULT_ARENA_NAME = "unnamed";

static inline __attribute__((always_inline)) size_t align_up(size_t const n, size_t const align) {
    return (n + align - 1) & ~(align - 1);
}

void arena_init_impl(
    Arena *const      arena,
    void *const       buf,
    size_t const      size,
    char const *const name,
    char const       *file,
    int               line) {
    (void)file;
    (void)line;
    if (arena == NULL) {
        return;
    }
    arena->buf  = (uint8_t *)buf;
    arena->size = size;
    if (name != NULL && *name != '\0') {
        strncpy(arena->name, name, sizeof(arena->name) - 1);
        arena->name[sizeof(arena->name) - 1] = '\0';
    } else {
        strncpy(arena->name, DEFAULT_ARENA_NAME, sizeof(arena->name) - 1);
        arena->name[sizeof(arena->name) - 1] = '\0';
    }
    arena->curr = 0;

#ifdef LOG_ARENA
    LOGINF_LOC(file, line, "Initialized arena '%s' with size %zu bytes", arena->name, arena->size);
#endif
}

void *arena_alloc_impl(Arena *const arena, size_t const size, char const *file, int line) {
    (void)file;
    (void)line;
    if (arena == NULL)
        return NULL;

    if (arena->buf == NULL || arena->size == 0) {
        LOGERR_LOC(file, line, "Arena '%s' is not properly initialized", arena->name);
        return NULL;
    }

    size_t const aligned_curr = align_up(arena->curr, 8);
    if (aligned_curr + size > arena->size) {
        LOGERR_LOC(
            file,
            line,
            "Arena '%s' out of memory: requested %zu bytes, available %zu bytes",
            arena->name,
            size,
            arena->size - aligned_curr);
        return NULL;
    }

    void *const ptr = &arena->buf[aligned_curr];
    arena->curr     = aligned_curr + size;

#ifdef LOG_ARENA
    LOGDBG_LOC(
        file,
        line,
        "Arena '%s' allocated %zu bytes, used %zu/%zu bytes",
        arena->name,
        size,
        arena->curr,
        arena->size);
#endif

    return ptr;
}

void arena_reset_impl(Arena *const arena, char const *file, int line) {
    (void)file;
    (void)line;
    if (arena != NULL) {
        arena->curr = 0;
#ifdef LOG_ARENA
        LOGDBG_LOC(file, line, "Arena '%s' reset, used 0/%zu bytes", arena->name, arena->size);
#endif
    }
}

size_t arena_used_impl(Arena const *const arena, char const *file, int line) {
    (void)file;
    (void)line;
    if (arena == NULL) {
        return 0;
    }
#ifdef LOG_ARENA
    LOGDBG_LOC(file, line, "Arena '%s' used %zu/%zu bytes", arena->name, arena->curr, arena->size);
#endif
    return arena->curr;
}
