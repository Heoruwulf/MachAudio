#ifndef MACHAUDIO_MIX_H
#define MACHAUDIO_MIX_H

#include <stddef.h>
#include <stdint.h>

/**
 * Mixes two 16-bit linear PCM little-endian audio buffers into one output buffer
 * using AVX2 saturated addition.
 *
 * @param in_a    First input buffer
 * @param in_b    Second input buffer
 * @param out     Output buffer (must be large enough to hold `samples`)
 * @param samples Number of 16-bit samples to process per buffer
 */
void mix_l16_avx2(
    int16_t const *const in_a,
    int16_t const *restrict const in_b,
    int16_t *const out,
    size_t const   samples);

#endif // MACHAUDIO_MIX_H
