#include "machaudio/transcode.h"
#include <arpa/inet.h>
#include <string.h>
#include "machaudio/mix.h"

// Utility to determine if host is little-endian
static inline bool is_host_little_endian(void) {
    uint16_t const x = 0x01;
    return *((uint8_t *)&x) == 1;
}

int transcode_session_init(
    TranscodeSession *const                 session,
    struct audio_start_payload const *const config) {
    if (unlikely(session == NULL || config == NULL)) {
        return -1;
    }

    memset(session, 0, sizeof(TranscodeSession));

    session->in_payload_type = config->in_payload_type;
    session->in_channels     = config->in_channels;
    session->in_endian       = config->in_endian;
    session->in_sample_rate  = ntohl(config->in_sample_rate);

    session->out_payload_type = config->out_payload_type;
    session->out_channels     = config->out_channels;
    session->out_endian       = config->out_endian;
    session->out_sample_rate  = ntohl(config->out_sample_rate);

    session->vad_enabled   = (ntohs(config->flags) & AUDIO_START_FLAGS_VAD_ENABLED) != 0;
    session->vad_state     = NULL;
    session->last_vad_prob = session->vad_enabled ? 0.0f : -1.0f;

    bool const host_le = is_host_little_endian();

    // Determine if we need to swap input L16
    if (session->in_payload_type == CODEC_L16) {
        if ((session->in_endian == ENDIAN_LITTLE && !host_le) ||
            (session->in_endian == ENDIAN_BIG && host_le))
        {
            session->swap_in = true;
        }
    }

    // Determine if we need to swap output L16
    if (session->out_payload_type == CODEC_L16) {
        if ((session->out_endian == ENDIAN_LITTLE && !host_le) ||
            (session->out_endian == ENDIAN_BIG && host_le))
        {
            session->swap_out = true;
        }
    }

    session->needs_resample = (session->in_sample_rate != session->out_sample_rate);
    if (session->needs_resample) {
        if (resampler_init(
                &session->resampler,
                session->in_sample_rate,
                session->out_sample_rate) != 0)
        {
            return -1;
        }
    }

    int err = 0;
    if (session->in_payload_type == CODEC_OPUS) {
        session->opus_dec =
            opus_decoder_create(session->in_sample_rate, session->in_channels, &err);
        if (err != OPUS_OK || !session->opus_dec)
            return -1;
    }

    if (session->out_payload_type == CODEC_OPUS) {
        session->opus_enc = opus_encoder_create(
            session->out_sample_rate,
            session->out_channels,
            OPUS_APPLICATION_VOIP,
            &err);
        if (err != OPUS_OK || !session->opus_enc) {
            if (session->opus_dec)
                opus_decoder_destroy(session->opus_dec);
            return -1;
        }
    }

    return 0;
}

void transcode_session_stop(TranscodeSession *const session) {
    if (unlikely(session == NULL)) {
        return;
    }

    if (session->opus_enc)
        opus_encoder_destroy(session->opus_enc);
    if (session->opus_dec)
        opus_decoder_destroy(session->opus_dec);

    memset(session, 0, sizeof(TranscodeSession));
}

// Swaps endianness of 16-bit samples in place
static void swap_endian_l16(int16_t *restrict const data, size_t const samples) {
    for (size_t i = 0; i < samples; ++i) {
        uint16_t const val = (uint16_t)data[i];
        data[i]            = (int16_t)((val << 8) | (val >> 8));
    }
}

int audio_process_transcode(
    TranscodeSession *const                 session,
    struct audio_input_payload const *const payload,
    size_t const                            payload_len,
    Arena *const                            out_arena) {

    size_t const initial_curr = out_arena->curr;
    if (unlikely(session == NULL || payload == NULL || out_arena == NULL)) {
        return -1;
    }

    uint32_t const num_buffers = ntohl(payload->num_buffers);
    if (num_buffers == 0) {
        return 0;
    }

    // 1. Decode / Prepare L16 Input and Mix Multiple Buffers
    int16_t *l16_buf     = NULL;
    size_t   l16_samples = 0;

    uint32_t offset = sizeof(struct audio_input_payload);
    for (uint32_t b = 0; b < num_buffers; ++b) {
        struct audio_buffer_header bh;
        void const *const          current_buffer_data =
            protocol_parse_next_buffer(payload, (uint32_t)payload_len, &offset, &bh);

        if (current_buffer_data == NULL) {
            break; // Stop parsing if payload is exhausted or malformed
        }

        size_t const         in_data_len = bh.length;
        uint8_t const *const in_data     = (uint8_t const *)current_buffer_data;
        (void)bh.volume; // Future: apply volume scaling

        int16_t *decode_buf     = NULL;
        size_t   decode_samples = 0;

        if (session->in_payload_type == CODEC_PCMU) {
            decode_samples = in_data_len;
            decode_buf     = arena_alloc(out_arena, decode_samples * sizeof(int16_t));
            if (!decode_buf)
                return -1;
            transcode_pcmu_to_l16(session, in_data, in_data_len, decode_buf);
        } else if (session->in_payload_type == CODEC_PCMA) {
            decode_samples = in_data_len;
            decode_buf     = arena_alloc(out_arena, decode_samples * sizeof(int16_t));
            if (!decode_buf)
                return -1;
            transcode_pcma_to_l16(session, in_data, in_data_len, decode_buf);
        } else if (session->in_payload_type == CODEC_L16) {
            decode_samples = in_data_len / (sizeof(int16_t) * session->in_channels);
            decode_buf     = arena_alloc(out_arena, in_data_len);
            if (!decode_buf)
                return -1;
            memcpy(decode_buf, in_data, in_data_len);
            if (session->swap_in) {
                swap_endian_l16(decode_buf, decode_samples * session->in_channels);
            }
        } else if (session->in_payload_type == CODEC_OPUS) {
            decode_buf = arena_alloc(out_arena, 5760 * session->in_channels * sizeof(int16_t));
            if (!decode_buf)
                return -1;
            int ret = opus_decode(session->opus_dec, in_data, in_data_len, decode_buf, 5760, 0);
            if (ret < 0)
                return -1;
            decode_samples = ret;
        } else {
            return -1;
        }

        if (b == 0) {
            l16_buf     = decode_buf;
            l16_samples = decode_samples;
        } else {
            // Mix additional buffers into the first one
            size_t const mix_samples =
                (decode_samples < l16_samples) ? decode_samples : l16_samples;
            size_t const total_elements = mix_samples * session->in_channels;
            mix_l16_avx2(l16_buf, decode_buf, l16_buf, total_elements);
        }
    }

    if (l16_buf == NULL || l16_samples == 0) {
        return -1;
    }

    // Run VAD on the combined mixed input stream if VAD is enabled and the input is L16 16kHz
    session->last_vad_prob = -1.0f;
    if (session->vad_enabled && session->vad_state != NULL &&
        session->in_sample_rate == VAD_SAMPLE_RATE)
    {
        session->last_vad_prob = vad_gru_process_pcm(
            session->vad_state,
            l16_buf,
            l16_samples,
            VAD_SAMPLE_RATE,
            (int)session->in_channels);
    }

    // 2. Resample (if needed)
    int16_t *resampled_buf     = l16_buf;
    size_t   resampled_samples = l16_samples;

    if (session->needs_resample) {
        size_t const out_cap =
            (l16_samples * session->out_sample_rate / session->in_sample_rate) + 128;
        resampled_buf = arena_alloc(out_arena, out_cap * session->out_channels * sizeof(int16_t));
        if (!resampled_buf)
            return -1;
        resampled_samples =
            resample_l16_advanced(session, l16_buf, l16_samples, resampled_buf, out_cap);
    }

    // Run VAD on the resampled stream if VAD is enabled, output is 16kHz, and we haven't run VAD
    // yet
    if (session->vad_enabled && session->vad_state != NULL && session->last_vad_prob == -1.0f &&
        session->out_sample_rate == VAD_SAMPLE_RATE)
    {
        session->last_vad_prob = vad_gru_process_pcm(
            session->vad_state,
            resampled_buf,
            resampled_samples,
            VAD_SAMPLE_RATE,
            (int)session->out_channels);
    }

    if (session->out_payload_type == CODEC_PCMU) {
        size_t const   out_len  = resampled_samples;
        uint8_t *const pcmu_out = arena_alloc(out_arena, out_len);
        if (!pcmu_out)
            return -1;
        transcode_l16_to_pcmu(session, resampled_buf, resampled_samples, pcmu_out);
        memmove((uint8_t *)out_arena->buf + initial_curr, pcmu_out, out_len);
        out_arena->curr = initial_curr + out_len;
    } else if (session->out_payload_type == CODEC_PCMA) {
        size_t const   out_len  = resampled_samples;
        uint8_t *const pcma_out = arena_alloc(out_arena, out_len);
        if (!pcma_out)
            return -1;
        transcode_l16_to_pcma(session, resampled_buf, resampled_samples, pcma_out);
        memmove((uint8_t *)out_arena->buf + initial_curr, pcma_out, out_len);
        out_arena->curr = initial_curr + out_len;
    } else if (session->out_payload_type == CODEC_L16) {
        size_t const out_len = resampled_samples * session->out_channels * sizeof(int16_t);
        if (session->swap_out) {
            swap_endian_l16(resampled_buf, resampled_samples * session->out_channels);
        }
        memmove((uint8_t *)out_arena->buf + initial_curr, resampled_buf, out_len);
        out_arena->curr = initial_curr + out_len;
    } else if (session->out_payload_type == CODEC_OPUS) {
        uint8_t *opus_out = arena_alloc(out_arena, 4000);
        if (!opus_out)
            return -1;
        int ret = opus_encode(session->opus_enc, resampled_buf, resampled_samples, opus_out, 4000);
        if (ret < 0)
            return -1;
        memmove((uint8_t *)out_arena->buf + initial_curr, opus_out, ret);
        out_arena->curr = initial_curr + ret;
    }

    return 0;
}
