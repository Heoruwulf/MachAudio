#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "machaudio/arena.h"
#include "machaudio/vad_gru.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    // VAD requires VAD_FEATURE_DIM (20) float features per frame.
    // 20 floats = 80 bytes.
    if (size < sizeof(float) * VAD_FEATURE_DIM) {
        return 0;
    }

    // Initialize an aligned local arena for state allocation
    alignas(32) uint8_t arena_buf[512];
    Arena               arena;
    arena_init(&arena, arena_buf, sizeof(arena_buf), "fuzz");

    VadGruState *state = vad_gru_init(&arena);
    if (state == NULL) {
        return 0;
    }

    // Process input data in chunks of 20 floats
    size_t const num_frames = size / (sizeof(float) * VAD_FEATURE_DIM);
    size_t const max_frames =
        (num_frames > 50) ? 50 : num_frames; // Cap iterations to prevent fuzzer timeouts

    float features[VAD_FEATURE_DIM];
    for (size_t f = 0; f < max_frames; ++f) {
        memcpy(features, data + f * sizeof(float) * VAD_FEATURE_DIM, sizeof(features));

        // Execute the VAD inference step (testing both C and AVX2 robustness)
        float const prob = vad_gru_process_frame(state, features, VAD_FEATURE_DIM);
        (void)prob;
    }

    return 0;
}
