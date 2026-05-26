#include <immintrin.h>
#include <math.h>
#include <stdalign.h>
#include <string.h>
#include "machaudio/transcode.h"

/*
 * Polyphase FIR Resampler Implementation
 */

// 2-tap Polyphase FIR (Linear Interpolation)
// Padded to 8 taps for SIMD
alignas(32) static int16_t const COEFFS_8_48[6][8] = {
    {32767, 0, 0, 0, 0, 0, 0, 0},
    {27306, 5461, 0, 0, 0, 0, 0, 0},
    {21845, 10922, 0, 0, 0, 0, 0, 0},
    {16384, 16384, 0, 0, 0, 0, 0, 0},
    {10922, 21845, 0, 0, 0, 0, 0, 0},
    {5461, 27306, 0, 0, 0, 0, 0, 0}};

alignas(32) static int16_t const COEFFS_16_48[3][8] = {
    {32767, 0, 0, 0, 0, 0, 0, 0},
    {21845, 10922, 0, 0, 0, 0, 0, 0},
    {10922, 21845, 0, 0, 0, 0, 0, 0}};

alignas(32) static int16_t const COEFFS_24_48[2][8] = {
    {32767, 0, 0, 0, 0, 0, 0, 0},
    {16384, 16384, 0, 0, 0, 0, 0, 0}};

static inline bool is_rate_supported(uint32_t rate) {
    return rate == 8000 || rate == 16000 || rate == 24000 || rate == 32000 || rate == 44100 ||
           rate == 48000;
}

int resampler_init(Resampler *const r, uint32_t in_rate, uint32_t out_rate) {
    if (!is_rate_supported(in_rate) || !is_rate_supported(out_rate)) {
        return -1;
    }

    r->phases = 0; // Use general linear interpolation by default
    r->taps   = 2;
    r->coeffs = NULL;
    // Advanced resampler requires an initial delay to avoid reading future samples.
    // Delay is max(64, ceil( (in_rate / out_rate) * 64 ))
    double s = (double)in_rate / (double)out_rate;
    if (s < 1.0)
        s = 1.0;
    int64_t delay = (int64_t)ceil(s * 64.0);
    r->pos_fp     = -(delay << 32);

    memset(r->delay_buf, 0, sizeof(r->delay_buf));

    // Maintain optimized path for common integer upsampling ratios
    if (in_rate == 8000 && out_rate == 48000) {
        r->phases = 6;
        r->coeffs = (int16_t const *)COEFFS_8_48;
    } else if (in_rate == 16000 && out_rate == 48000) {
        r->phases = 3;
        r->coeffs = (int16_t const *)COEFFS_16_48;
    } else if (in_rate == 24000 && out_rate == 48000) {
        r->phases = 2;
        r->coeffs = (int16_t const *)COEFFS_24_48;
    }

    return 0;
}

size_t resample_l16(
    TranscodeSession *const session,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const out_capacity) {

    Resampler *const r        = &session->resampler;
    uint32_t const   in_rate  = session->in_sample_rate;
    uint32_t const   out_rate = session->out_sample_rate;

    if (in_rate == out_rate) {
        size_t const samples = in_samples < out_capacity ? in_samples : out_capacity;
        memcpy(out_data, in_data, samples * sizeof(int16_t));
        return samples;
    }

    size_t const out_samples = (size_t)((uint64_t)in_samples * out_rate / in_rate);
    if (out_samples > out_capacity)
        return 0;

    // Use optimized integer upsampling path if coeffs are available and Q=1
    if (r->coeffs && (out_rate % in_rate == 0)) {
        int const phases = r->phases;
        for (size_t i = 0; i < in_samples; ++i) {
            int16_t const x0 = in_data[i];
            int16_t const x1 = (i + 1 < in_samples) ? in_data[i + 1] : x0;

#ifdef __AVX2__
            // Old resampler kernels are only 8 taps (128 bits).
            // Cannot load 256 bits safely if it overruns the global array.
            // Disable AVX2 for this legacy path to keep it simple and safe.
            for (int p = 0; p < phases; ++p) {
                int32_t acc = (int32_t)x0 * r->coeffs[p * 8] + (int32_t)x1 * r->coeffs[p * 8 + 1];
                out_data[i * phases + p] = (int16_t)(acc >> 15);
            }
#else
            for (int p = 0; p < phases; ++p) {
                int32_t acc = (int32_t)x0 * r->coeffs[p * 8] + (int32_t)x1 * r->coeffs[p * 8 + 1];
                out_data[i * phases + p] = (int16_t)(acc >> 15);
            }
#endif
        }
        return out_samples;
    }

    // General Linear Interpolation using 32.32 fixed-point accumulator
    uint64_t const step_fp = ((uint64_t)in_rate << 32) / out_rate;
    uint64_t       pos_fp  = 0;

    for (size_t i = 0; i < out_samples; ++i) {
        size_t const   idx  = (size_t)(pos_fp >> 32);
        uint32_t const frac = (uint32_t)((pos_fp >> 16) & 0xFFFF); // 16-bit fraction

        int32_t const x0 = in_data[idx];
        int32_t const x1 = (idx + 1 < in_samples) ? in_data[idx + 1] : x0;

        // Linear interpolation: x0 + (x1 - x0) * frac / 65536
        int32_t const res = x0 + (((x1 - x0) * (int32_t)frac) >> 16);
        out_data[i]       = (int16_t)res;

        pos_fp += step_fp;
    }

    return out_samples;
}
