#ifndef MACHAUDIO_TRANSCODE_H
#define MACHAUDIO_TRANSCODE_H

#include "machaudio/arena.h"
#include "machaudio/protocol.h"
#include "machaudio/vad_gru.h"

#include <stdbool.h>
#include <stdint.h>

// Codec Constants from TelePortal spec
#define CODEC_PCMU 0
#define CODEC_PCMA 8
#define CODEC_L16  96
#define CODEC_OPUS 111

typedef enum {
    ENDIAN_NONE   = 0,
    ENDIAN_LITTLE = 1,
    ENDIAN_BIG    = 2,
} AudioEndian;

#define RESAMPLER_MAX_TAPS 1024

typedef struct {
    int16_t const *coeffs;
    int            taps;
    int            phases;
    int64_t        pos_fp; // 32.32 fixed point
    int16_t        delay_buf[RESAMPLER_MAX_TAPS];
} Resampler;

#include <opus/opus.h>

typedef struct {
    uint8_t  in_payload_type;
    uint8_t  in_channels;
    uint8_t  in_endian;
    uint32_t in_sample_rate;

    uint8_t  out_payload_type;
    uint8_t  out_channels;
    uint8_t  out_endian;
    uint32_t out_sample_rate;

    // Flags for hot-path decisions
    bool swap_in;
    bool swap_out;
    bool needs_resample;

    Resampler resampler;

    // Voice Activity Detection (VAD) state
    VadGruState *vad_state;
    bool         vad_enabled;
    float        last_vad_prob;

    OpusEncoder *opus_enc;
    OpusDecoder *opus_dec;
} TranscodeSession;

/**
 * Initializes a transcoding session based on the CMD_START payload.
 */
int transcode_session_init(
    TranscodeSession *const                 session,
    struct audio_start_payload const *const config);

/**
 * Cleans up resources associated with the transcoding session.
 */
void transcode_session_stop(TranscodeSession *const session);

/**
 * Resampler initialization and processing
 */
int resampler_init(Resampler *const r, uint32_t in_rate, uint32_t out_rate);

size_t resample_l16(
    TranscodeSession *const session,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const out_capacity);

/**
 * Advanced Resampler using Windowed-Sinc and AVX2.
 */
size_t resample_l16_advanced(
    TranscodeSession *const session,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    int16_t *restrict const out_data,
    size_t const out_capacity);

/**
 * Main Dispatcher (called upon receiving CMD_INPUT)
 */
int audio_process_transcode(
    TranscodeSession *const                 session,
    struct audio_input_payload const *const payload,
    size_t const                            payload_len,
    Arena *const                            out_arena);

// Specific transcoding functions

int transcode_pcmu_to_l16(
    TranscodeSession *const session,
    uint8_t const *restrict const in_data,
    size_t const in_len,
    int16_t *restrict const out_data);

int transcode_pcma_to_l16(
    TranscodeSession *const session,
    uint8_t const *restrict const in_data,
    size_t const in_len,
    int16_t *restrict const out_data);

int transcode_l16_to_pcmu(
    TranscodeSession *const session,
    int16_t const *restrict const in_data,
    size_t const in_len,
    uint8_t *restrict const out_data);

int transcode_l16_to_pcma(
    TranscodeSession *const session,
    int16_t const *restrict const in_data,
    size_t const in_len,
    uint8_t *restrict const out_data);

#endif // MACHAUDIO_TRANSCODE_H
