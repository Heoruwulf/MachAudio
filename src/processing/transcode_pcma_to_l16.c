#include "g711_tables.h"
#include "machaudio/transcode.h"

#include <immintrin.h>
#include <stddef.h>

int transcode_pcma_to_l16(
    TranscodeSession *const session,
    uint8_t const *restrict const in_data,
    size_t const in_len,
    int16_t *restrict const out_data) {
    (void)session;

    if (unlikely(in_data == NULL || out_data == NULL)) {
        return -1;
    }

    // Telephony ptime guarantee: We strictly require buffer sizes to be
    // multiples of 16 to ensure SIMD compatibility and protocol alignment.
    if (unlikely(in_len % 16 != 0)) {
        return -1;
    }

    size_t i = 0;

#ifdef __AVX2__
    // Tell compiler that we've already validated the length
#if defined(__clang__)
    __builtin_assume(in_len % 16 == 0);
#elif defined(__GNUC__)
    if (in_len % 16 != 0)
        __builtin_unreachable();
#endif

    // Process 16 samples at a time
    for (; i < in_len; i += 16) {
        // Load 16 bytes of PCMA indices
        __m128i const indices_16 = _mm_loadu_si128((__m128i const *)&in_data[i]);

        // Zero-extend lower 8 bytes to 8x 32-bit integers
        __m256i const indices_lo_32 = _mm256_cvtepu8_epi32(indices_16);
        // Zero-extend upper 8 bytes to 8x 32-bit integers
        __m128i const indices_hi_8  = _mm_unpackhi_epi64(indices_16, indices_16);
        __m256i const indices_hi_32 = _mm256_cvtepu8_epi32(indices_hi_8);

        // Gather 16 samples from the 32-bit LUT (using vpgatherdd)
        __m256i const samples_lo_32 =
            _mm256_i32gather_epi32((int const *)PCMA_TO_L16_LUT_32, indices_lo_32, 4);
        __m256i const samples_hi_32 =
            _mm256_i32gather_epi32((int const *)PCMA_TO_L16_LUT_32, indices_hi_32, 4);

        // Pack the 32-bit results down to 16-bit
        __m256i const packed    = _mm256_packs_epi32(samples_lo_32, samples_hi_32);
        __m256i const reordered = _mm256_permute4x64_epi64(packed, _MM_SHUFFLE(3, 1, 2, 0));

        // Store 32 bytes (16 samples)
        _mm256_storeu_si256((__m256i *)&out_data[i], reordered);
    }
#else
    for (; i < in_len; ++i) {
        out_data[i] = PCMA_TO_L16_LUT[in_data[i]];
    }
#endif

    return (int)in_len;
}
