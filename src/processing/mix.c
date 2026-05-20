#include "machaudio/mix.h"
#include <immintrin.h>
#include <stdint.h>
#include "machaudio/macros.h"

/**
 * Scalar fallback for saturated addition.
 */
static inline int16_t clamp_add_int16(int16_t const a, int16_t const b)
    __attribute__((always_inline));
static inline int16_t clamp_add_int16(int16_t const a, int16_t const b) {
    int32_t const sum = (int32_t)a + (int32_t)b;
    if (unlikely(sum > 32767)) {
        return 32767;
    }
    if (unlikely(sum < -32768)) {
        return -32768;
    }
    return (int16_t)sum;
}

void mix_l16_avx2(
    int16_t const *const in_a,
    int16_t const *restrict const in_b,
    int16_t *const out,
    size_t const   samples) {

    // Process 16 samples (32 bytes) at a time using AVX2.
    // _mm256_adds_epi16 performs signed saturated addition on 16x 16-bit integers.
    size_t const simd_samples = samples & ~((size_t)15);

    for (size_t i = 0; i < simd_samples; i += 16) {
        __m256i const a_vec = _mm256_loadu_si256((__m256i const *)(in_a + i));
        __m256i const b_vec = _mm256_loadu_si256((__m256i const *)(in_b + i));

        __m256i const out_vec = _mm256_adds_epi16(a_vec, b_vec);

        _mm256_storeu_si256((__m256i *)(out + i), out_vec);
    }

    // Scalar fallback for tail elements (if samples is not a multiple of 16)
    for (size_t i = simd_samples; i < samples; ++i) {
        out[i] = clamp_add_int16(in_a[i], in_b[i]);
    }
}
