#ifndef VAD_TRAINING_LOADER_H
#define VAD_TRAINING_LOADER_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    size_t       num_frames;
    float const *features; // 32-byte aligned guaranteed
    float const *labels;   // 32-byte aligned guaranteed
    bool         is_allocated;
} VadTrainingData;

VadTrainingData vad_training_data_load(char const *filepath);
void            vad_training_data_free(VadTrainingData *data);

#endif // VAD_TRAINING_LOADER_H
