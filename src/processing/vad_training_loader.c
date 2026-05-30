#include "vad_training_loader.h"
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "machaudio/log.h"
#include "vad_training_data.h"

#define VAD_RECORD_SIZE 84 // 20 features + 1 label = 21 floats = 84 bytes

VadTrainingData vad_training_data_load(char const *filepath) {
    VadTrainingData data = {
        .num_frames   = VAD_NUM_FRAMES,
        .features     = VAD_TRAIN_FEATURES,
        .labels       = VAD_TRAIN_LABELS,
        .is_allocated = false};

    if (filepath == NULL || filepath[0] == '\0') {
        LOGINF("Using built-in VAD training data (%d frames).", VAD_NUM_FRAMES);
        return data;
    }

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        LOGERR(
            "Failed to open custom VAD training data: %s. Falling back to built-in data.",
            filepath);
        return data;
    }

    fseek(f, 0, SEEK_END);
    long total_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (total_size <= 0 || (total_size % VAD_RECORD_SIZE) != 0) {
        LOGERR(
            "Invalid VAD training data size (%ld bytes). Must be a multiple of %d bytes. Falling "
            "back to built-in data.",
            total_size,
            VAD_RECORD_SIZE);
        fclose(f);
        return data;
    }

    size_t num_frames = total_size / VAD_RECORD_SIZE;

    // Allocate 32-byte aligned buffers
    size_t features_size        = num_frames * VAD_MEL_BANDS * sizeof(float);
    size_t padded_features_size = (features_size + 31) & ~31;
    float *dynamic_features     = aligned_alloc(32, padded_features_size);

    size_t labels_size        = num_frames * sizeof(float);
    size_t padded_labels_size = (labels_size + 31) & ~31;
    float *dynamic_labels     = aligned_alloc(32, padded_labels_size);

    if (!dynamic_features || !dynamic_labels) {
        LOGERR("Failed to allocate aligned memory for custom VAD training data. Falling back to "
               "built-in data.");
        if (dynamic_features)
            free(dynamic_features);
        if (dynamic_labels)
            free(dynamic_labels);
        fclose(f);
        return data;
    }

    assert(((uintptr_t)dynamic_features % 32) == 0);
    assert(((uintptr_t)dynamic_labels % 32) == 0);

    // Read and scatter data
    float record[VAD_MEL_BANDS + 1];
    for (size_t i = 0; i < num_frames; ++i) {
        if (fread(record, sizeof(float), VAD_MEL_BANDS + 1, f) != (VAD_MEL_BANDS + 1)) {
            LOGERR(
                "Failed to read record %zu from VAD training data. Falling back to built-in data.",
                i);
            free(dynamic_features);
            free(dynamic_labels);
            fclose(f);
            return data;
        }
        memcpy(&dynamic_features[i * VAD_MEL_BANDS], record, VAD_MEL_BANDS * sizeof(float));
        dynamic_labels[i] = record[VAD_MEL_BANDS];
    }

    fclose(f);

    data.num_frames   = num_frames;
    data.features     = dynamic_features;
    data.labels       = dynamic_labels;
    data.is_allocated = true;

    LOGINF(
        "Successfully loaded custom VAD training data from %s (%zu frames).",
        filepath,
        num_frames);

    return data;
}

void vad_training_data_free(VadTrainingData *data) {
    if (data && data->is_allocated) {
        free((void *)data->features);
        free((void *)data->labels);
        data->features     = NULL;
        data->labels       = NULL;
        data->is_allocated = false;
        data->num_frames   = 0;
    }
}
