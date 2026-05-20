#include "machaudio/arena.h"

#include <string.h>

static inline __attribute__((always_inline)) size_t align_up(size_t const n, size_t const align) {
    return (n + align - 1) & ~(align - 1);
}

void arena_init(Arena *const arena, void *const buf, size_t const size) {
    if (arena == NULL) {
        return;
    }
    arena->buf  = (uint8_t *)buf;
    arena->size = size;
    arena->curr = 0;
}

void *arena_alloc(Arena *const arena, size_t const size) {
    if (arena == NULL || arena->buf == NULL) {
        return NULL;
    }

    size_t const aligned_curr = align_up(arena->curr, 8);
    if (aligned_curr + size > arena->size) {
        return NULL;
    }

    void *const ptr = &arena->buf[aligned_curr];
    arena->curr     = aligned_curr + size;
    return ptr;
}

void arena_reset(Arena *const arena) {
    if (arena != NULL) {
        arena->curr = 0;
    }
}

size_t arena_used(Arena const *const arena) {
    if (arena == NULL) {
        return 0;
    }
    return arena->curr;
}
