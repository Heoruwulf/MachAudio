#include <immintrin.h>
#include <math.h>
#include <string.h>
#include "machaudio/macros.h"
#include "machaudio/transcode.h"
#include "resample_sinc_table.h"

/*
 * Advanced Resampler Implementation (Windowed-Sinc)
 */

static inline int16_t clamp_to_int16(double val) {
    double r = round(val);
    if (r > 32767.0)
        return 32767;
    if (r < -32768.0)
        return -32768;
    return (int16_t)r;
}

#ifdef __AVX2__
/**
 * Vector MAC for 128 taps using AVX2.
 * coeffs must be 32-byte aligned.
 */
static inline float
mac_128_taps_avx2(int16_t const *restrict const in, int16_t const *restrict const coeffs) {
    __m256 v_sum0 = _mm256_setzero_ps();
    __m256 v_sum1 = _mm256_setzero_ps();

    for (int i = 0; i < 128; i += 32) {
        __m256i const v_in0 = _mm256_loadu_si256((__m256i const *)&in[i]);
        __m256i const v_c0  = _mm256_load_si256((__m256i const *)&coeffs[i]);
        __m256i const v_m0  = _mm256_madd_epi16(v_in0, v_c0); // 8x 32-bit ints
        v_sum0              = _mm256_add_ps(v_sum0, _mm256_cvtepi32_ps(v_m0));

        __m256i const v_in1 = _mm256_loadu_si256((__m256i const *)&in[i + 16]);
        __m256i const v_c1  = _mm256_load_si256((__m256i const *)&coeffs[i + 16]);
        __m256i const v_m1  = _mm256_madd_epi16(v_in1, v_c1);
        v_sum1              = _mm256_add_ps(v_sum1, _mm256_cvtepi32_ps(v_m1));
    }

    __m256 v_sum = _mm256_add_ps(v_sum0, v_sum1);

    // Horizontal sum of 8 floats
    __m128 v_low  = _mm256_castps256_ps128(v_sum);
    __m128 v_high = _mm256_extractf128_ps(v_sum, 1);
    v_low         = _mm_add_ps(v_low, v_high);

    v_low = _mm_hadd_ps(v_low, v_low);
    v_low = _mm_hadd_ps(v_low, v_low);

    return _mm_cvtss_f32(v_low);
}

/**
 * AVX2 kernel for downsampling. Processes 8 output samples concurrently.
 */
static inline void mac_downsample_avx2(
    Resampler *restrict const r,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const  count,
    double const  ratio,
    int64_t const step_fp) {

    double const inv_s      = ratio;
    double const s          = 1.0 / ratio;
    double const step       = s;
    int const    center_off = (SINC_TAPS / 2) - 1;

    __m256 const  v_inv_s     = _mm256_set1_ps((float)inv_s);
    __m256 const  v_center    = _mm256_set1_ps((float)center_off);
    __m256 const  v_phases    = _mm256_set1_ps((float)SINC_PHASES);
    __m256i const v_phases_m1 = _mm256_set1_epi32(SINC_PHASES - 1);
    __m256 const  v_step      = _mm256_set1_ps((float)step);
    __m256 const  v_offsets   = _mm256_setr_ps(0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f);
    __m256 const  v_norm      = _mm256_set1_ps((float)ratio / 32767.0f);

    int16_t const *const base_ptr = &SINC_COEFFS[0][0];

    for (size_t i = 0; i < count; ++i) {
        int64_t const pos_fp = r->pos_fp;
        double const  t0 = (double)(pos_fp >> 32) + (double)(pos_fp & 0xFFFFFFFF) / 4294967296.0;
        __m256 const  v_t =
            _mm256_add_ps(_mm256_set1_ps((float)t0), _mm256_mul_ps(v_offsets, v_step));

        double const t7        = t0 + 7.0 * step;
        int const    k_min_all = (int)ceil(t0 - s * (double)center_off);
        int const    k_max_all = (int)floor(t7 + s * (double)(SINC_TAPS - 1 - center_off));

        __m256 v_acc = _mm256_setzero_ps();

        for (int k = k_min_all; k <= k_max_all; ++k) {
            __m256 const v_k = _mm256_set1_ps((float)k);
            __m256 const v_val_idx =
                _mm256_add_ps(_mm256_mul_ps(_mm256_sub_ps(v_t, v_k), v_inv_s), v_center);

            __m256 const  v_jf = _mm256_floor_ps(v_val_idx);
            __m256i const v_j  = _mm256_cvtps_epi32(v_jf);

            // Mask for j in [0, 127]
            __m256i const v_mask = _mm256_and_si256(
                _mm256_cmpgt_epi32(v_j, _mm256_set1_epi32(-1)),
                _mm256_cmpgt_epi32(_mm256_set1_epi32(SINC_TAPS), v_j));

            __m256 const  v_alpha = _mm256_sub_ps(v_val_idx, v_jf);
            __m256i const v_pp =
                _mm256_cvtps_epi32(_mm256_floor_ps(_mm256_mul_ps(v_alpha, v_phases)));
            __m256i const v_pp_clamped =
                _mm256_max_epi32(_mm256_setzero_si256(), _mm256_min_epi32(v_pp, v_phases_m1));

            // Ensure j is also clamped for safe gathering, even if masked out later
            __m256i const v_j_clamped = _mm256_max_epi32(
                _mm256_setzero_si256(),
                _mm256_min_epi32(v_j, _mm256_set1_epi32(SINC_TAPS - 1)));

            // Index = (pp * 128 + j_clamped) * 2
            __m256i const v_idx = _mm256_slli_epi32(
                _mm256_add_epi32(_mm256_slli_epi32(v_pp_clamped, 7), v_j_clamped),
                1);

            // Gather 8 coefficients (read 32-bit, sign-extend lower 16-bit)
            __m256i const v_c32 = _mm256_i32gather_epi32((int const *)base_ptr, v_idx, 1);
            __m256i const v_c16 = _mm256_srai_epi32(_mm256_slli_epi32(v_c32, 16), 16);

            int16_t sample;
            if (unlikely(k < 0)) {
                sample = r->delay_buf[RESAMPLER_MAX_TAPS + k < 0 ? 0 : RESAMPLER_MAX_TAPS + k];
            } else if (unlikely(k >= (int)in_samples)) {
                sample = in_data[in_samples - 1];
            } else {
                sample = in_data[k];
            }

            __m256 const v_in   = _mm256_set1_ps((float)sample);
            __m256       v_prod = _mm256_mul_ps(v_in, _mm256_cvtepi32_ps(v_c16));

            // Apply mask
            v_prod = _mm256_and_ps(v_prod, _mm256_castsi256_ps(v_mask));

            v_acc = _mm256_add_ps(v_acc, v_prod);
        }

        v_acc = _mm256_mul_ps(v_acc, v_norm);

        // Round to nearest and convert to int16
        __m256i const v_out32 = _mm256_cvtps_epi32(
            _mm256_round_ps(v_acc, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));

        // Extract 8 int32, pack with saturation to int16, and store
        __m256i const v_packed   = _mm256_packs_epi32(v_out32, v_out32);
        __m256i const v_permuted = _mm256_permute4x64_epi64(v_packed, _MM_SHUFFLE(3, 2, 2, 0));
        _mm_storeu_si128((__m128i *)&out_data[i * 8], _mm256_castsi256_si128(v_permuted));

        r->pos_fp += 8 * step_fp;
    }
}
#endif

size_t resample_l16_advanced(
    TranscodeSession *const session,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const out_capacity) {

    Resampler *const r        = &session->resampler;
    uint32_t const   in_rate  = session->in_sample_rate;
    uint32_t const   out_rate = session->out_sample_rate;

    if (unlikely(in_rate == out_rate)) {
        size_t const samples = in_samples < out_capacity ? in_samples : out_capacity;
        memcpy(out_data, in_data, samples * sizeof(int16_t));
        return samples;
    }

    double const  ratio   = (double)out_rate / (double)in_rate;
    double const  step    = 1.0 / ratio;
    int64_t const step_fp = (int64_t)(step * (double)(1ULL << 32));

    size_t const out_samples = (size_t)((double)in_samples * ratio);
    if (unlikely(out_samples > out_capacity))
        return 0;

    int const center_off = (SINC_TAPS / 2) - 1;

    // Downsampling stretch factor
    double const s     = ratio < 1.0 ? 1.0 / ratio : 1.0;
    double const inv_s = 1.0 / s;

    for (size_t i = 0; i < out_samples; ++i) {
        int64_t const pos_fp = r->pos_fp;
        int           idx    = (int)(pos_fp >> 32);
        double        frac   = (double)(pos_fp & 0xFFFFFFFF) / (double)(1ULL << 32);

        double dacc = 0.0;

        if (likely(ratio >= 1.0)) {
            // Upsampling path: Polyphase FIR
            int const            phase  = (int)(frac * SINC_PHASES);
            int const            p      = phase >= SINC_PHASES ? SINC_PHASES - 1 : phase;
            int16_t const *const coeffs = SINC_COEFFS[p];

            // Check if we are near boundaries
            if (likely(idx >= center_off && idx < (int)in_samples - (SINC_TAPS - center_off))) {
                int16_t const *const in_ptr = &in_data[idx - center_off];
#ifdef __AVX2__
                float const acc = mac_128_taps_avx2(in_ptr, coeffs);
                dacc            = (double)acc / 32767.0;
#else
                for (int j = 0; j < SINC_TAPS; ++j) {
                    dacc += (double)in_ptr[j] * (double)coeffs[j];
                }
                dacc /= 32767.0;
#endif
            } else {
                for (int j = 0; j < SINC_TAPS; ++j) {
                    int const k = (int)idx - center_off + j;
                    int16_t   sample;
                    if (k < 0) {
                        sample = r->delay_buf[RESAMPLER_MAX_TAPS + k];
                    } else if (k >= (int)in_samples) {
                        sample = in_data[in_samples - 1];
                    } else {
                        sample = in_data[k];
                    }
                    dacc += (double)sample * ((double)coeffs[j] / 32767.0);
                }
            }
        } else {
            // Downsampling path: Direct Convolution with stretched kernel
#ifdef __AVX2__
            size_t const remaining   = out_samples - i;
            size_t const avx8_blocks = remaining / 8;
            if (avx8_blocks > 0) {
                mac_downsample_avx2(
                    r,
                    in_data,
                    in_samples,
                    &out_data[i],
                    avx8_blocks,
                    ratio,
                    step_fp);
                i += avx8_blocks * 8 - 1;

                // Update local loop variables for remaining scalar processing
                // Note: The loop will increment 'i' and we will evaluate pos_fp again at the top
                // Wait, if we break, we don't need to update. If we continue, we need to update
                // pos_fp for scalar fallback? Actually, if we just continue to the next iteration
                // of the for-loop, pos_fp is read at the top of the loop! So we can just
                // `continue;` and the loop will increment `i` and read the new `r->pos_fp`!
                continue;
            }
#endif
            double const t     = (double)idx + frac;
            int const    k_min = (int)ceil(t - s * (double)center_off);
            int const    k_max = (int)floor(t + s * (double)(SINC_TAPS - 1 - center_off));

            for (int k = k_min; k <= k_max; ++k) {
                double const x_target = (t - (double)k) * inv_s;
                double const val_idx  = x_target + (double)center_off;
                int const    j        = (int)floor(val_idx);
                double const alpha    = val_idx - (double)j;

                if (j >= 0 && j < SINC_TAPS) {
                    int const p  = (int)(alpha * SINC_PHASES);
                    int const pp = p >= SINC_PHASES ? SINC_PHASES - 1 : p;

                    int16_t sample;
                    if (k < 0) {
                        sample =
                            r->delay_buf[RESAMPLER_MAX_TAPS + k < 0 ? 0 : RESAMPLER_MAX_TAPS + k];
                    } else if (k >= (int)in_samples) {
                        sample = in_data[in_samples - 1];
                    } else {
                        sample = in_data[k];
                    }
                    dacc += (double)sample * (double)SINC_COEFFS[pp][j];
                }
            }
            dacc = (dacc / 32767.0) * ratio;
        }

        out_data[i] = clamp_to_int16(dacc);
        r->pos_fp += step_fp;
    }

    // Update history
    if (in_samples >= RESAMPLER_MAX_TAPS) {
        memcpy(
            r->delay_buf,
            &in_data[in_samples - RESAMPLER_MAX_TAPS],
            RESAMPLER_MAX_TAPS * sizeof(int16_t));
    } else {
        memmove(
            r->delay_buf,
            &r->delay_buf[in_samples],
            (RESAMPLER_MAX_TAPS - in_samples) * sizeof(int16_t));
        memcpy(
            &r->delay_buf[RESAMPLER_MAX_TAPS - in_samples],
            in_data,
            in_samples * sizeof(int16_t));
    }

    r->pos_fp -= ((int64_t)in_samples << 32);

    return out_samples;
}
