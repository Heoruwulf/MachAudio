#ifndef MACHAUDIO_ARENA_H
#define MACHAUDIO_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct Arena Arena;

struct Arena {
    char     name[32]; // name for logging/debugging purposes
    uint8_t *buf;
    size_t   size;
    size_t   curr;
};

/**
 * Implementation functions that accept caller file and line details.
 * Do not call these directly; use the macro wrappers below.
 */
void arena_init_impl(
    Arena *const      arena,
    void *const       buf,
    size_t const      size,
    char const *const name,
    char const       *file,
    int               line);
void  *arena_alloc_impl(Arena *const arena, size_t const size, char const *file, int line);
void   arena_reset_impl(Arena *const arena, char const *file, int line);
size_t arena_used_impl(Arena const *const arena, char const *file, int line);

/**
 * Macro wrappers providing caller file and line instrumentation.
 */
#define arena_init(arena, buf, size, name)                                                         \
    arena_init_impl((arena), (buf), (size), (name), __FILE__, __LINE__)
#define arena_alloc(arena, size) arena_alloc_impl((arena), (size), __FILE__, __LINE__)
#define arena_reset(arena)       arena_reset_impl((arena), __FILE__, __LINE__)
#define arena_used(arena)        arena_used_impl((arena), __FILE__, __LINE__)

#endif // MACHAUDIO_ARENA_H
