#include <arpa/inet.h>
#include <string.h>
#include "../../src/processing/g711_tables.h"
#include "audio_test_utils.h"
#include "machaudio/transcode.h"
#include "unity.h"

void test_transcode_session_init(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_endian        = ENDIAN_BIG,
        .in_sample_rate   = htonl(8000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_endian       = ENDIAN_LITTLE,
        .out_sample_rate  = htonl(8000)};

    int r = transcode_session_init(&session, &config);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_UINT32(8000, session.in_sample_rate);
}

void test_audio_process_transcode_echo(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(8000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(8000)};
    transcode_session_init(&session, &config);

    Arena   arena;
    uint8_t arena_buf[2048];
    arena_init(&arena, arena_buf, sizeof(arena_buf), "transcode");

    uint8_t                     raw_payload[64] = {0};
    struct audio_input_payload *input_payload   = (struct audio_input_payload *)raw_payload;
    input_payload->num_buffers                  = htonl(1);

    struct audio_buffer_header *buf_header =
        (struct audio_buffer_header *)(raw_payload + sizeof(struct audio_input_payload));
    buf_header->length    = htonl(6);
    uint32_t vol_bits_net = protocol_float_to_net(1.0f);
    memcpy(&buf_header->volume, &vol_bits_net, 4);

    uint8_t      *data          = (uint8_t *)(raw_payload + sizeof(struct audio_input_payload) +
                                sizeof(struct audio_buffer_header));
    uint8_t const sample_data[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    memcpy(data, sample_data, 6);

    size_t payload_len =
        sizeof(struct audio_input_payload) + sizeof(struct audio_buffer_header) + 8; // padded

    int r = audio_process_transcode(&session, input_payload, payload_len, &arena);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_UINT32(6, arena_used(&arena));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sample_data, arena.buf, 6);
}

void test_transcode_session_init_null(void) {
    int r = transcode_session_init(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

void test_transcode_session_stop(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {0};
    transcode_session_init(&session, &config);

    transcode_session_stop(&session);
    TEST_ASSERT_EQUAL_UINT8(0, session.in_payload_type);
}

void test_transcode_pcmu_to_l16_bit_exact(void) {
    // Tests are expanded to 16 samples to satisfy SIMD ptime constraints
    uint8_t const pcmu_in[16] = {
        0x00,
        0xFF,
        0x80,
        0x7F,
        0x00,
        0xFF,
        0x80,
        0x7F,
        0x00,
        0xFF,
        0x80,
        0x7F,
        0x00,
        0xFF,
        0x80,
        0x7F};
    int16_t const expected_l16[16] =
        {-32124, 0, 32124, 0, -32124, 0, 32124, 0, -32124, 0, 32124, 0, -32124, 0, 32124, 0};
    int16_t out_l16[16];

    transcode_pcmu_to_l16(NULL, pcmu_in, 16, out_l16);
    TEST_ASSERT_EQUAL_INT16_ARRAY(expected_l16, out_l16, 16);
}

void test_transcode_pcmu_to_l16_invalid_size(void) {
    uint8_t pcmu_in[15] = {0};
    int16_t out_l16[15] = {0};
    int     r           = transcode_pcmu_to_l16(NULL, pcmu_in, 15, out_l16);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

void test_transcode_pcma_to_l16_bit_exact(void) {
    uint8_t const pcma_in[16] = {
        0x00,
        0xD5,
        0x80,
        0x55,
        0x00,
        0xD5,
        0x80,
        0x55,
        0x00,
        0xD5,
        0x80,
        0x55,
        0x00,
        0xD5,
        0x80,
        0x55};
    int16_t out_l16[16];
    transcode_pcma_to_l16(NULL, pcma_in, 16, out_l16);

    for (int i = 0; i < 4; i++) {
        TEST_ASSERT_EQUAL_INT16(PCMA_TO_L16_LUT[0x00], out_l16[i * 4 + 0]);
        TEST_ASSERT_EQUAL_INT16(PCMA_TO_L16_LUT[0xD5], out_l16[i * 4 + 1]);
    }
}

void test_transcode_pcma_to_l16_invalid_size(void) {
    uint8_t pcma_in[15] = {0};
    int16_t out_l16[15] = {0};
    int     r           = transcode_pcma_to_l16(NULL, pcma_in, 15, out_l16);
    TEST_ASSERT_EQUAL_INT(-1, r);
}

void test_transcode_g711_roundtrip_snr(void) {
    size_t const samples = 160;
    int16_t      original[160];
    uint8_t      encoded[160];
    int16_t      decoded[160];

    audio_gen_sine(original, samples, 1000.0, 8000.0, 10000.0);

    transcode_l16_to_pcmu(NULL, original, samples, encoded);
    transcode_pcmu_to_l16(NULL, encoded, samples, decoded);
    double const snr_u = audio_calc_snr(original, decoded, samples);
    TEST_ASSERT_TRUE(snr_u > 35.0);

    transcode_l16_to_pcma(NULL, original, samples, encoded);
    transcode_pcma_to_l16(NULL, encoded, samples, decoded);
    double const snr_a = audio_calc_snr(original, decoded, samples);
    TEST_ASSERT_TRUE(snr_a > 35.0);
}

void test_resample_8k_to_48k_snr(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(8000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(48000)};
    transcode_session_init(&session, &config);

    size_t const in_samples  = 160;
    size_t const out_samples = in_samples * 6;
    int16_t      in_buf[160];
    int16_t      out_buf[960];
    int16_t      reference[960];

    audio_gen_sine(in_buf, in_samples, 1000.0, 8000.0, 10000.0);
    audio_gen_sine(reference, out_samples, 1000.0, 48000.0, 10000.0);

    resample_l16(&session, in_buf, in_samples, out_buf, out_samples);

    double const snr = audio_calc_snr(reference, out_buf, out_samples);
    TEST_ASSERT_TRUE(snr > 20.0);
}

void test_resample_16k_to_48k_snr(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(16000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(48000)};
    int r = transcode_session_init(&session, &config);
    TEST_ASSERT_EQUAL_INT(0, r);

    size_t const in_samples  = 160;
    size_t const out_samples = in_samples * 3;
    int16_t      in_buf[160];
    int16_t      out_buf[480];
    int16_t      reference[480];

    audio_gen_sine(in_buf, in_samples, 1000.0, 16000.0, 10000.0);
    audio_gen_sine(reference, out_samples, 1000.0, 48000.0, 10000.0);

    resample_l16(&session, in_buf, in_samples, out_buf, out_samples);

    double const snr = audio_calc_snr(reference, out_buf, out_samples);
    TEST_ASSERT_TRUE(snr > 20.0);
}

void test_resample_44k_to_48k_snr(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(44100),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(48000)};
    int r = transcode_session_init(&session, &config);
    TEST_ASSERT_EQUAL_INT(0, r);

    size_t const in_samples  = 441;
    size_t const out_samples = 480;
    int16_t      in_buf[441];
    int16_t      out_buf[480];
    int16_t      reference[480];

    audio_gen_sine(in_buf, in_samples, 1000.0, 44100.0, 10000.0);
    audio_gen_sine(reference, out_samples, 1000.0, 48000.0, 10000.0);

    resample_l16(&session, in_buf, in_samples, out_buf, out_samples);

    double const snr = audio_calc_snr(reference, out_buf, out_samples);
    TEST_ASSERT_TRUE(snr > 20.0);
}

void test_resample_48k_to_16k_snr(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_L16,
        .in_channels      = 1,
        .in_sample_rate   = htonl(48000),
        .out_payload_type = CODEC_L16,
        .out_channels     = 1,
        .out_sample_rate  = htonl(16000)};
    int r = transcode_session_init(&session, &config);
    TEST_ASSERT_EQUAL_INT(0, r);

    size_t const in_samples  = 480;
    size_t const out_samples = 160;
    int16_t      in_buf[480];
    int16_t      out_buf[160];
    int16_t      reference[160];

    audio_gen_sine(in_buf, in_samples, 1000.0, 48000.0, 10000.0);
    audio_gen_sine(reference, out_samples, 1000.0, 16000.0, 10000.0);

    resample_l16(&session, in_buf, in_samples, out_buf, out_samples);

    double const snr = audio_calc_snr(reference, out_buf, out_samples);
    TEST_ASSERT_TRUE(snr > 20.0);
}

void test_opus_roundtrip_snr(void) {
    TranscodeSession           session;
    struct audio_start_payload config = {
        .in_payload_type  = CODEC_OPUS,
        .in_channels      = 1,
        .in_sample_rate   = htonl(48000),
        .out_payload_type = CODEC_OPUS,
        .out_channels     = 1,
        .out_sample_rate  = htonl(48000)};
    int r = transcode_session_init(&session, &config);
    if (r != 0) {
        TEST_IGNORE_MESSAGE("Opus not supported or init failed");
        return;
    }

    size_t const samples = 960;
    int16_t      original[960];
    uint8_t      encoded[1000];
    int16_t      decoded[960];

    audio_gen_sine(original, samples, 1000.0, 48000.0, 10000.0);

    int encoded_len = transcode_l16_to_opus(&session, original, samples, encoded, 1000);
    TEST_ASSERT_TRUE(encoded_len > 0);

    int decoded_samples = transcode_opus_to_l16(&session, encoded, encoded_len, decoded);
    TEST_ASSERT_TRUE(decoded_samples > 0);

    int16_t out_val = 0;
    for (size_t i = 0; i < (size_t)decoded_samples; i++)
        out_val |= decoded[i];
    TEST_ASSERT_TRUE(out_val != 0);

    transcode_session_stop(&session);
}
