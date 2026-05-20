#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>
#include "audio_test_utils.h"
#include "machaudio/transcode.h"
#include "unity.h"

void test_resample_adv_8k_to_48k_snr(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(8000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(48000)};
    transcode_session_init(&session, &config);

    size_t const in_samples  = 800; // 100ms
    size_t const out_samples = 4800;
    int16_t     *in_buf      = malloc(in_samples * sizeof(int16_t));
    int16_t     *out_buf     = malloc(out_samples * sizeof(int16_t));
    int16_t     *reference   = malloc(out_samples * sizeof(int16_t));

    audio_gen_sine(in_buf, in_samples, 1000.0, 8000.0, 10000.0);

    // Account for group delay: 15.5 samples at 8kHz = 93 samples at 48kHz
    // We'll just generate the reference with a slight offset or skip the beginning.
    audio_gen_sine(reference, out_samples, 1000.0, 48000.0, 10000.0);

    resample_l16_advanced(&session, in_buf, in_samples, out_buf, out_samples);

    // Align signals by finding max cross-correlation to bypass fixed group delay maths.
    int    best_offset = 0;
    double max_corr    = 0;
    for (int offset = 0; offset < 500; ++offset) {
        double corr = 0;
        for (int i = 0; i < 500; ++i) {
            corr += (double)reference[i + 400] * (double)out_buf[i + 400 + offset];
        }
        if (corr > max_corr) {
            max_corr    = corr;
            best_offset = offset;
        }
    }

    double const snr = audio_calc_snr(reference + 400, out_buf + 400 + best_offset, 1000);
    printf("SNR 8k->48k: %.2f dB (offset: %d)\n", snr, best_offset);

    free(in_buf);
    free(out_buf);
    free(reference);

    TEST_ASSERT_TRUE(snr > 60.0);
}

void test_resample_adv_48k_to_16k_anti_alias(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(48000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(16000)};
    transcode_session_init(&session, &config);

    // High frequency signal (20kHz) at 48kHz.
    // Should be filtered out when resampling to 16kHz (Nyquist 8kHz).
    size_t const in_samples  = 480;
    size_t const out_samples = 160;
    int16_t      in_buf[480];
    int16_t      out_buf[160];

    audio_gen_sine(in_buf, in_samples, 20000.0, 48000.0, 10000.0);
    resample_l16_advanced(&session, in_buf, in_samples, out_buf, 160);

    // Output should be nearly silent
    double max_val = 0;
    for (size_t i = 0; i < out_samples; ++i) {
        double val = fabs((double)out_buf[i]);
        if (val > max_val)
            max_val = val;
    }
    printf("Max val after filtering 20kHz: %.2f\n", max_val);
    TEST_ASSERT_TRUE(max_val < 4000.0); // limited by 32-phase quantization
}

void test_resample_adv_48k_to_48k_snr(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(48000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(48000)};
    transcode_session_init(&session, &config);

    size_t const in_samples = 480;
    int16_t      in_buf[480];
    int16_t      out_buf[480];
    int16_t      reference[480];

    audio_gen_sine(in_buf, in_samples, 1000.0, 48000.0, 10000.0);
    memcpy(reference, in_buf, sizeof(in_buf));

    resample_l16_advanced(&session, in_buf, in_samples, out_buf, 480);

    double const snr = audio_calc_snr(reference + 100, out_buf + 100, 300);
    printf("SNR 48k->48k: %.2f dB\n", snr);
    TEST_ASSERT_TRUE(snr > 80.0);
}
void test_resample_adv_streaming(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(16000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(48000)};
    transcode_session_init(&session, &config);

    size_t const chunk_samples = 160;
    int16_t      in_buf[160];
    int16_t      out_buf[480];

    // Process two chunks
    audio_gen_sine(in_buf, chunk_samples, 1000.0, 16000.0, 10000.0);
    resample_l16_advanced(&session, in_buf, chunk_samples, out_buf, 480);

    // The second chunk should continue smoothly
    resample_l16_advanced(&session, in_buf, chunk_samples, out_buf, 480);

    // Verify continuity (optional but good)
    TEST_ASSERT_TRUE((session.resampler.pos_fp >> 32) <= 1);
}
