#include "unity.h"

extern void test_arena_init(void);
extern void test_arena_alloc(void);
extern void test_arena_alignment(void);
extern void test_arena_oom(void);
extern void test_arena_reset(void);

extern void test_protocol_validation_valid(void);
extern void test_protocol_validation_invalid_magic(void);
extern void test_protocol_validation_invalid_version(void);
extern void test_protocol_decoding(void);
extern void test_audio_output_payload_structure(void);
extern void test_protocol_multi_buffer_parsing(void);

extern void test_transcode_session_init(void);
extern void test_transcode_session_init_null(void);
extern void test_audio_process_transcode_echo(void);
extern void test_transcode_session_stop(void);
extern void test_transcode_pcmu_to_l16_bit_exact(void);
extern void test_transcode_pcmu_to_l16_invalid_size(void);
extern void test_transcode_pcma_to_l16_bit_exact(void);
extern void test_transcode_pcma_to_l16_invalid_size(void);
extern void test_transcode_g711_roundtrip_snr(void);
extern void test_resample_8k_to_48k_snr(void);
extern void test_resample_16k_to_48k_snr(void);
extern void test_resample_44k_to_48k_snr(void);
extern void test_resample_48k_to_16k_snr(void);
extern void test_opus_roundtrip_snr(void);

extern void test_resample_adv_48k_to_48k_snr(void);
extern void test_resample_adv_streaming(void);
extern void test_resample_adv_8k_to_48k_snr(void);
extern void test_resample_adv_48k_to_16k_anti_alias(void);

extern void test_mix_zero_sum(void);
extern void test_mix_standard_addition(void);
extern void test_mix_positive_saturation(void);
extern void test_mix_negative_saturation(void);
extern void test_mix_tail_processing(void);

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_arena_init);
    RUN_TEST(test_arena_alloc);
    RUN_TEST(test_arena_alignment);
    RUN_TEST(test_arena_oom);
    RUN_TEST(test_arena_reset);

    RUN_TEST(test_protocol_validation_valid);
    RUN_TEST(test_protocol_validation_invalid_magic);
    RUN_TEST(test_protocol_validation_invalid_version);
    RUN_TEST(test_protocol_decoding);
    RUN_TEST(test_audio_output_payload_structure);
    RUN_TEST(test_protocol_multi_buffer_parsing);

    RUN_TEST(test_transcode_session_init);
    RUN_TEST(test_transcode_session_init_null);
    RUN_TEST(test_audio_process_transcode_echo);
    RUN_TEST(test_transcode_session_stop);
    RUN_TEST(test_transcode_pcmu_to_l16_bit_exact);
    RUN_TEST(test_transcode_pcmu_to_l16_invalid_size);
    RUN_TEST(test_transcode_pcma_to_l16_bit_exact);
    RUN_TEST(test_transcode_pcma_to_l16_invalid_size);
    RUN_TEST(test_transcode_g711_roundtrip_snr);
    RUN_TEST(test_resample_8k_to_48k_snr);
    RUN_TEST(test_resample_16k_to_48k_snr);
    RUN_TEST(test_resample_44k_to_48k_snr);
    RUN_TEST(test_resample_48k_to_16k_snr);
    RUN_TEST(test_opus_roundtrip_snr);

    RUN_TEST(test_resample_adv_48k_to_48k_snr);
    RUN_TEST(test_resample_adv_streaming);
    RUN_TEST(test_resample_adv_8k_to_48k_snr);
    RUN_TEST(test_resample_adv_48k_to_16k_anti_alias);

    RUN_TEST(test_mix_zero_sum);
    RUN_TEST(test_mix_standard_addition);
    RUN_TEST(test_mix_positive_saturation);
    RUN_TEST(test_mix_negative_saturation);
    RUN_TEST(test_mix_tail_processing);

    return UNITY_END();
}
