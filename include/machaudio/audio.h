#ifndef MACHAUDIO_AUDIO_H
#define MACHAUDIO_AUDIO_H

#include <opus/opus.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    OpusDecoder *decoder;
    int          channels;
    int          sample_rate;
} MachAudioEngine;

/**
 * Initializes the audio engine.
 */
int audio_engine_init(MachAudioEngine *const engine, int const sample_rate, int const channels);

/**
 * Processes a chunk of audio data.
 * For now, this is a stub that might just decode Opus if requested.
 */
int audio_engine_process(
    MachAudioEngine *const engine,
    uint8_t const *restrict const data,
    size_t const len);

/**
 * Frees audio engine resources.
 */
void audio_engine_destroy(MachAudioEngine *const engine);

#endif // MACHAUDIO_AUDIO_H
