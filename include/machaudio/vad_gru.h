#ifndef MACHAUDIO_VAD_GRU_H
#define MACHAUDIO_VAD_GRU_H

#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include "machaudio/arena.h"

#define VAD_FEATURE_DIM 20
#define VAD_HIDDEN_DIM  24
#define VAD_SAMPLE_RATE 16000

typedef struct VadGruState VadGruState;

struct VadGruState {
    alignas(32) float hidden_state[VAD_HIDDEN_DIM];
};

/**
 * Initializes the VAD state using memory allocated from the provided Arena.
 * Guarantees 32-byte alignment for the allocated memory block to satisfy AVX alignment
 * requirements. Returns NULL if the allocation fails.
 */
VadGruState *vad_gru_init(Arena *const arena);

/**
 * Resets the VAD state by zeroing out the hidden state.
 */
void vad_gru_reset(VadGruState *const state);

/**
 * Processes a single frame of features and returns the voice activity probability (0.0 to 1.0).
 * @param state Pointer to the VAD state.
 * @param features Pointer to the feature vector (must have length VAD_FEATURE_DIM).
 * @param num_features Size of the feature vector (must be VAD_FEATURE_DIM).
 */
float vad_gru_process_frame(
    VadGruState *const state,
    float const *const restrict features,
    size_t const num_features);

/**
 * Extracts temporal log-energy features and runs VAD inference frame-by-frame on a raw PCM buffer.
 * Returns the maximum voice activity probability (0.0f to 1.0f) calculated across all frames.
 */
float vad_gru_process_pcm(
    VadGruState *const   state,
    int16_t const *const pcm,
    size_t const         samples,
    int const            sample_rate,
    int const            channels);

#endif // MACHAUDIO_VAD_GRU_H
