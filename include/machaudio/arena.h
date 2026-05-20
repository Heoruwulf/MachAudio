#ifndef MACHAUDIO_ARENA_H
#define MACHAUDIO_ARENA_H

#include <stddef.h>
#include <stdint.h>

typedef struct Arena Arena;

struct Arena {
    uint8_t *buf;
    size_t   size;
    size_t   curr;
};

/**
 * Initializes an arena with a given buffer and size.
 * The buffer must be managed by the caller.
 */
void arena_init(Arena *const arena, void *const buf, size_t const size);

/**
 * Allocates a block of memory from the arena.
 * Returns NULL if the allocation fails (out of memory).
 * Ensures 8-byte alignment.
 */
void *arena_alloc(Arena *const arena, size_t const size);

/**
 * Resets the arena, making all its memory available for reuse.
 * Does not zero out the memory.
 */
void arena_reset(Arena *const arena);

/**
 * Returns the number of bytes used in the arena.
 */
size_t arena_used(Arena const *const arena);

#endif // MACHAUDIO_ARENA_H
