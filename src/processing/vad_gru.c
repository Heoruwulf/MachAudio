#include "machaudio/vad_gru.h"
#include <math.h>
#include <stdint.h>
#include <string.h>
#include "vad_gru_weights.h"

#ifdef __AVX2__
#include <immintrin.h>
#endif

// Allocates and initializes the VadGruState from the arena.
// Guarantees 32-byte alignment for the allocated memory block to satisfy AVX alignment
// requirements.
VadGruState *vad_gru_init(Arena *const arena) {
    if (arena == NULL) {
        return NULL;
    }

    // Determine current address and offset for 32-byte alignment
    uintptr_t const addr   = (uintptr_t)(arena->buf + arena->curr);
    size_t const    offset = addr % 32;
    size_t const    pad    = (offset == 0) ? 0 : (32 - offset);

    if (pad > 0) {
        arena->curr += pad;
    }

    VadGruState *const state = arena_alloc(arena, sizeof(VadGruState));
    if (state == NULL) {
        return NULL;
    }

    vad_gru_reset(state);
    return state;
}

// Resets the VAD state by zeroing out the hidden state.
void vad_gru_reset(VadGruState *const state) {
    if (state != NULL) {
        memset(state->hidden_state, 0, sizeof(state->hidden_state));
    }
}

// Pure C11 reference implementation of the VAD frame processing.
float vad_gru_process_frame_c(
    VadGruState *const state,
    float const *const restrict features,
    size_t const num_features) {
    if (state == NULL || features == NULL || num_features != VAD_FEATURE_DIM) {
        return 0.0f;
    }

    float const *const h_prev = state->hidden_state;

    // input_proj[i] = dot(W_input[i], features) + B_input[i]
    // rec_proj[i] = dot(W_recurrent[i], h_prev) + B_recurrent[i]
    float input_proj[72];
    float rec_proj[72];

    for (size_t i = 0; i < 72; ++i) {
        float in_sum = 0.0f;
        for (size_t j = 0; j < 20; ++j) {
            in_sum += VAD_GRU_W_INPUT[i * 20 + j] * features[j];
        }
        input_proj[i] = in_sum + VAD_GRU_B_INPUT[i];

        float rec_sum = 0.0f;
        for (size_t j = 0; j < 24; ++j) {
            rec_sum += VAD_GRU_W_RECURRENT[i * 24 + j] * h_prev[j];
        }
        rec_proj[i] = rec_sum + VAD_GRU_B_RECURRENT[i];
    }

    float h_new[24];

    for (size_t k = 0; k < 24; ++k) {
        // Reset gate: r[k] = sigmoid(input_proj[k] + rec_proj[k])
        float const r_arg = input_proj[k] + rec_proj[k];
        float const r     = 1.0f / (1.0f + expf(-r_arg));

        // Update gate: z[k] = sigmoid(input_proj[24 + k] + rec_proj[24 + k])
        float const z_arg = input_proj[24 + k] + rec_proj[24 + k];
        float const z     = 1.0f / (1.0f + expf(-z_arg));

        // Candidate state: n[k] = tanh(input_proj[48 + k] + r * rec_proj[48 + k])
        float const n_arg = input_proj[48 + k] + r * rec_proj[48 + k];
        float const n     = tanhf(n_arg);

        // New hidden state: h_new[k] = (1 - z) * n + z * h_prev[k]
        h_new[k] = (1.0f - z) * n + z * h_prev[k];
    }

    // Save back new hidden state
    memcpy(state->hidden_state, h_new, sizeof(h_new));

    // Fully connected layer and output sigmoid
    float fc_sum = 0.0f;
    for (size_t k = 0; k < 24; ++k) {
        fc_sum += h_new[k] * VAD_FC_WEIGHT[k];
    }
    float const out_val = fc_sum + VAD_FC_BIAS[0];
    return 1.0f / (1.0f + expf(-out_val));
}

#ifdef __AVX2__

// Custom high-performance SIMD exp approximation for __m256.
// Approximates 2^f for f in [-0.5, 0.5] using a 5th-order Taylor polynomial.
static inline __m256 exp_ps(__m256 x) {
    // Clamp to prevent overflow/underflow
    x = _mm256_max_ps(x, _mm256_set1_ps(-88.0f));
    x = _mm256_min_ps(x, _mm256_set1_ps(88.0f));

    __m256 const log2e = _mm256_set1_ps(1.4426950408889634f);
    __m256 const y     = _mm256_mul_ps(x, log2e);

    // Round to nearest integer to isolate exponent
    __m256 const n = _mm256_round_ps(y, _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC);
    __m256 const f = _mm256_sub_ps(y, n);

    // Taylor series coefficients for 2^f
    __m256 const c1 = _mm256_set1_ps(0.6931471805599453f);
    __m256 const c2 = _mm256_set1_ps(0.2402265069591007f);
    __m256 const c3 = _mm256_set1_ps(0.0555041086648215f);
    __m256 const c4 = _mm256_set1_ps(0.0096181291076284f);
    __m256 const c5 = _mm256_set1_ps(0.0013333558146428f);

    __m256 p = c5;
    p        = _mm256_fmadd_ps(p, f, c4);
    p        = _mm256_fmadd_ps(p, f, c3);
    p        = _mm256_fmadd_ps(p, f, c2);
    p        = _mm256_fmadd_ps(p, f, c1);
    p        = _mm256_fmadd_ps(p, f, _mm256_set1_ps(1.0f));

    // Convert n to integer and shift to exponent field to compute 2^n
    __m256i const imm   = _mm256_add_epi32(_mm256_cvtps_epi32(n), _mm256_set1_epi32(127));
    __m256 const  pow2n = _mm256_castsi256_ps(_mm256_slli_epi32(imm, 23));

    return _mm256_mul_ps(p, pow2n);
}

// Custom SIMD sigmoid using exp_ps.
static inline __m256 sigmoid_ps(__m256 x) {
    __m256 const neg_x = _mm256_sub_ps(_mm256_setzero_ps(), x);
    __m256 const den   = _mm256_add_ps(_mm256_set1_ps(1.0f), exp_ps(neg_x));
    return _mm256_div_ps(_mm256_set1_ps(1.0f), den);
}

// Custom SIMD tanh using exp_ps.
static inline __m256 tanh_ps(__m256 x) {
    // abs(x) = andnot(sign_bit, x)
    __m256 const abs_x         = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), x);
    __m256 const neg_two_abs_x = _mm256_mul_ps(abs_x, _mm256_set1_ps(-2.0f));
    __m256 const exp_term      = exp_ps(neg_two_abs_x);

    __m256 const num = _mm256_sub_ps(_mm256_set1_ps(1.0f), exp_term);
    __m256 const den = _mm256_add_ps(_mm256_set1_ps(1.0f), exp_term);
    __m256 const res = _mm256_div_ps(num, den);

    // Restore original sign: res = sign(x) * res
    __m256 const sign_mask = _mm256_and_ps(x, _mm256_set1_ps(-0.0f));
    return _mm256_or_ps(res, sign_mask);
}

// AVX2 optimized implementation.
float vad_gru_process_frame_avx2(
    VadGruState *const state,
    float const *const restrict features,
    size_t const num_features) {
    if (state == NULL || features == NULL || num_features != VAD_FEATURE_DIM) {
        return 0.0f;
    }

    float const *const h_prev = state->hidden_state;

    alignas(32) float input_proj[72];
    alignas(32) float rec_proj[72];

    // Load features. VAD_FEATURE_DIM is 20: 8 + 8 + 4.
    __m256 const vx0 = _mm256_loadu_ps(&features[0]);
    __m256 const vx1 = _mm256_loadu_ps(&features[8]);
    __m128 const vx2 = _mm_loadu_ps(&features[16]);

    // Compute input projections (72 elements)
    for (size_t i = 0; i < 72; ++i) {
        __m256 const vw0 = _mm256_loadu_ps(&VAD_GRU_W_INPUT[i * 20 + 0]);
        __m256 const vw1 = _mm256_loadu_ps(&VAD_GRU_W_INPUT[i * 20 + 8]);
        __m128 const vw2 = _mm_loadu_ps(&VAD_GRU_W_INPUT[i * 20 + 16]);

        __m256 sum256 = _mm256_mul_ps(vx0, vw0);
        sum256        = _mm256_fmadd_ps(vx1, vw1, sum256);
        __m128 sum128 = _mm_mul_ps(vx2, vw2);

        // Vector reduction
        __m128 const low      = _mm256_castps256_ps128(sum256);
        __m128 const high     = _mm256_extractf128_ps(sum256, 1);
        __m128       combined = _mm_add_ps(low, high);
        combined              = _mm_add_ps(combined, sum128);

        combined = _mm_hadd_ps(combined, combined);
        combined = _mm_hadd_ps(combined, combined);

        float dot_val;
        _mm_store_ss(&dot_val, combined);
        input_proj[i] = dot_val + VAD_GRU_B_INPUT[i];
    }

    // Load recurrent state. VAD_HIDDEN_DIM is 24: 8 + 8 + 8.
    __m256 const vh0 = _mm256_loadu_ps(&h_prev[0]);
    __m256 const vh1 = _mm256_loadu_ps(&h_prev[8]);
    __m256 const vh2 = _mm256_loadu_ps(&h_prev[16]);

    // Compute recurrent projections (72 elements)
    for (size_t i = 0; i < 72; ++i) {
        __m256 const vw0 = _mm256_loadu_ps(&VAD_GRU_W_RECURRENT[i * 24 + 0]);
        __m256 const vw1 = _mm256_loadu_ps(&VAD_GRU_W_RECURRENT[i * 24 + 8]);
        __m256 const vw2 = _mm256_loadu_ps(&VAD_GRU_W_RECURRENT[i * 24 + 16]);

        __m256 sum256 = _mm256_mul_ps(vh0, vw0);
        sum256        = _mm256_fmadd_ps(vh1, vw1, sum256);
        sum256        = _mm256_fmadd_ps(vh2, vw2, sum256);

        __m128 const low      = _mm256_castps256_ps128(sum256);
        __m128 const high     = _mm256_extractf128_ps(sum256, 1);
        __m128       combined = _mm_add_ps(low, high);

        combined = _mm_hadd_ps(combined, combined);
        combined = _mm_hadd_ps(combined, combined);

        float dot_val;
        _mm_store_ss(&dot_val, combined);
        rec_proj[i] = dot_val + VAD_GRU_B_RECURRENT[i];
    }

    alignas(32) float h_new[24];

    // Compute gates and update hidden state in chunks of 8
    for (size_t g = 0; g < 3; ++g) {
        size_t const k_start = g * 8;

        __m256 const in_r  = _mm256_loadu_ps(&input_proj[k_start]);
        __m256 const rec_r = _mm256_loadu_ps(&rec_proj[k_start]);

        __m256 const in_z  = _mm256_loadu_ps(&input_proj[24 + k_start]);
        __m256 const rec_z = _mm256_loadu_ps(&rec_proj[24 + k_start]);

        __m256 const in_n  = _mm256_loadu_ps(&input_proj[48 + k_start]);
        __m256 const rec_n = _mm256_loadu_ps(&rec_proj[48 + k_start]);

        __m256 const prev_h = _mm256_loadu_ps(&h_prev[k_start]);

        // r = sigmoid(in_r + rec_r)
        __m256 const r = sigmoid_ps(_mm256_add_ps(in_r, rec_r));

        // z = sigmoid(in_z + rec_z)
        __m256 const z = sigmoid_ps(_mm256_add_ps(in_z, rec_z));

        // n = tanh(in_n + r * rec_n)
        __m256 const n = tanh_ps(_mm256_fmadd_ps(r, rec_n, in_n));

        // new_h = (1 - z) * n + z * prev_h
        __m256 const one         = _mm256_set1_ps(1.0f);
        __m256 const one_minus_z = _mm256_sub_ps(one, z);
        __m256 const new_h       = _mm256_fmadd_ps(one_minus_z, n, _mm256_mul_ps(z, prev_h));

        _mm256_store_ps(&h_new[k_start], new_h);
    }

    // Save back new hidden state
    memcpy(state->hidden_state, h_new, sizeof(h_new));

    // Fully connected layer and output sigmoid
    __m256 const vh_new0 = _mm256_load_ps(&h_new[0]);
    __m256 const vh_new1 = _mm256_load_ps(&h_new[8]);
    __m256 const vh_new2 = _mm256_load_ps(&h_new[16]);

    __m256 const vfc_w0 = _mm256_loadu_ps(&VAD_FC_WEIGHT[0]);
    __m256 const vfc_w1 = _mm256_loadu_ps(&VAD_FC_WEIGHT[8]);
    __m256 const vfc_w2 = _mm256_loadu_ps(&VAD_FC_WEIGHT[16]);

    __m256 sum256 = _mm256_mul_ps(vh_new0, vfc_w0);
    sum256        = _mm256_fmadd_ps(vh_new1, vfc_w1, sum256);
    sum256        = _mm256_fmadd_ps(vh_new2, vfc_w2, sum256);

    __m128 const low      = _mm256_castps256_ps128(sum256);
    __m128 const high     = _mm256_extractf128_ps(sum256, 1);
    __m128       combined = _mm_add_ps(low, high);

    combined = _mm_hadd_ps(combined, combined);
    combined = _mm_hadd_ps(combined, combined);

    float fc_val;
    _mm_store_ss(&fc_val, combined);

    float const out_val = fc_val + VAD_FC_BIAS[0];
    return 1.0f / (1.0f + expf(-out_val));
}

#endif // __AVX2__

// Dynamic dispatch to optimal implementation path
float vad_gru_process_frame(
    VadGruState *const state,
    float const *const restrict features,
    size_t const num_features) {
#ifdef __AVX2__
    return vad_gru_process_frame_avx2(state, features, num_features);
#else
    return vad_gru_process_frame_c(state, features, num_features);
#endif
}

// Extracts temporal log-energy features and runs VAD inference frame-by-frame on a raw PCM buffer.
float vad_gru_process_pcm(
    VadGruState *const   state,
    int16_t const *const pcm,
    size_t const         samples,
    int const            sample_rate,
    int const            channels) {
    if (state == NULL || pcm == NULL || samples < 1 || channels <= 0) {
        return -1.0f;
    }

    if (sample_rate != VAD_SAMPLE_RATE) {
        return -1.0f;
    }

    // Determine 20ms frame size for the given sample rate
    size_t const frame_size = (size_t)(sample_rate * 0.02);
    if (frame_size == 0) {
        return -1.0f;
    }

    float max_prob = 0.0f;
    float features[VAD_FEATURE_DIM];

    // Process frame-by-frame
    for (size_t offset = 0; offset + frame_size <= samples; offset += frame_size) {
        size_t const seg_size = frame_size / VAD_FEATURE_DIM;
        if (seg_size == 0) {
            continue;
        }

        for (size_t g = 0; g < VAD_FEATURE_DIM; ++g) {
            float energy = 0.0f;
            for (size_t s = 0; s < seg_size; ++s) {
                float val = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    val += (float)pcm[(offset + g * seg_size + s) * channels + c];
                }
                val /= channels;
                energy += fabsf(val) / 32768.0f; // Normalize to [-1.0, 1.0] range
            }
            energy /= seg_size;
            features[g] =
                logf(1.0f + energy * 10.0f); // log(1 + 10 * energy) temporal feature scaling
        }

        float const prob = vad_gru_process_frame(state, features, VAD_FEATURE_DIM);
        if (prob > max_prob) {
            max_prob = prob;
        }
    }

    return max_prob;
}
