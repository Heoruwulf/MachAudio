#include <stdio.h>
#include <stdlib.h>
#include "machaudio/audio.h"
#include "machaudio/log.h"

int audio_engine_init(MachAudioEngine *const engine, int const sample_rate, int const channels) {
    if (engine == NULL) {
        return -1;
    }

    engine->sample_rate = sample_rate;
    engine->channels    = channels;
    return 0;
}

int audio_engine_process(
    MachAudioEngine *const engine,
    uint8_t const *restrict const data,
    size_t const len) {
    if (engine == NULL || data == NULL) {
        return -1;
    }

    (void)len;
    LOGDBG("Audio engine processing %zu bytes of data", len);
    return 0;
}

void audio_engine_destroy(MachAudioEngine *const engine) { (void)engine; }
