#include <arpa/inet.h>
#include <string.h>
#include "machaudio/protocol.h"
#include "unity.h"

void test_protocol_validation_valid(void) {
    AudioMsgHeader header = {
        .magic       = AUDIO_MAGIC,
        .version     = AUDIO_VERSION,
        .command     = CMD_START,
        .sequence_id = 1,
        .payload_len = 14};

    TEST_ASSERT_TRUE(protocol_validate_header(&header));
}

void test_protocol_validation_invalid_magic(void) {
    AudioMsgHeader header = {
        .magic       = 0xDEADBEEF,
        .version     = AUDIO_VERSION,
        .command     = CMD_START,
        .sequence_id = 1,
        .payload_len = 14};

    TEST_ASSERT_FALSE(protocol_validate_header(&header));
}

void test_protocol_validation_invalid_version(void) {
    AudioMsgHeader header = {
        .magic       = AUDIO_MAGIC,
        .version     = 2,
        .command     = CMD_START,
        .sequence_id = 1,
        .payload_len = 14};

    TEST_ASSERT_FALSE(protocol_validate_header(&header));
}

void test_protocol_decoding(void) {
    AudioMsgHeader header;
    header.magic       = htonl(AUDIO_MAGIC);
    header.version     = htons(AUDIO_VERSION);
    header.command     = htons(CMD_INPUT);
    header.sequence_id = htonl(1234);
    header.payload_len = htonl(5678);

    protocol_decode_header(&header);

    TEST_ASSERT_EQUAL_UINT32(AUDIO_MAGIC, header.magic);
    TEST_ASSERT_EQUAL_UINT16(AUDIO_VERSION, header.version);
    TEST_ASSERT_EQUAL_UINT16(CMD_INPUT, header.command);
    TEST_ASSERT_EQUAL_UINT32(1234, header.sequence_id);
    TEST_ASSERT_EQUAL_UINT32(5678, header.payload_len);
}

void test_audio_output_payload_structure(void) {
    // AudioMsgHeader alignment
    TEST_ASSERT_EQUAL_UINT(16, sizeof(AudioMsgHeader));
    TEST_ASSERT_EQUAL_UINT(0, offsetof(AudioMsgHeader, magic));
    TEST_ASSERT_EQUAL_UINT(4, offsetof(AudioMsgHeader, version));
    TEST_ASSERT_EQUAL_UINT(6, offsetof(AudioMsgHeader, command));
    TEST_ASSERT_EQUAL_UINT(8, offsetof(AudioMsgHeader, sequence_id));
    TEST_ASSERT_EQUAL_UINT(12, offsetof(AudioMsgHeader, payload_len));

    // audio_start_payload alignment
    TEST_ASSERT_EQUAL_UINT(16, sizeof(struct audio_start_payload));
    TEST_ASSERT_EQUAL_UINT(0, offsetof(struct audio_start_payload, in_payload_type));
    TEST_ASSERT_EQUAL_UINT(4, offsetof(struct audio_start_payload, in_sample_rate));
    TEST_ASSERT_EQUAL_UINT(8, offsetof(struct audio_start_payload, out_payload_type));
    TEST_ASSERT_EQUAL_UINT(12, offsetof(struct audio_start_payload, out_sample_rate));

    // audio_output_payload alignment
    struct audio_output_payload payload;
    TEST_ASSERT_EQUAL_UINT(8, sizeof(payload.duration_ns));
    size_t offset_data = (uint8_t *)&payload.data - (uint8_t *)&payload.duration_ns;
    TEST_ASSERT_EQUAL_UINT(8, offset_data);

    // audio_discover_reply_payload alignment
    TEST_ASSERT_EQUAL_UINT(8, sizeof(struct audio_discover_reply_payload));
    TEST_ASSERT_EQUAL_UINT(0, offsetof(struct audio_discover_reply_payload, num_workers));
}

void test_protocol_multi_buffer_parsing(void) {
    uint8_t  payload[64] = {0};
    uint32_t offset      = 0;

    // 2 buffers
    struct audio_input_payload *container = (struct audio_input_payload *)payload;
    container->num_buffers                = htonl(2);
    offset += sizeof(struct audio_input_payload);

    // Buffer 1: 10 bytes, vol 0.5
    struct audio_buffer_header *h1 = (struct audio_buffer_header *)(payload + offset);
    h1->length                     = htonl(10);
    uint32_t vol05_bits_net        = protocol_float_to_net(0.5f);
    memcpy(&h1->volume, &vol05_bits_net, 4);

    offset += sizeof(struct audio_buffer_header);
    memset(payload + offset, 0xAA, 10);
    offset += (10 + 3) & ~3U; // Padded to 12

    // Buffer 2: 5 bytes, vol 1.0
    struct audio_buffer_header *h2 = (struct audio_buffer_header *)(payload + offset);
    h2->length                     = htonl(5);
    uint32_t vol10_bits_net        = protocol_float_to_net(1.0f);
    memcpy(&h2->volume, &vol10_bits_net, 4);

    offset += sizeof(struct audio_buffer_header);
    memset(payload + offset, 0xBB, 5);
    offset += (5 + 3) & ~3U; // Padded to 8

    uint32_t                   payload_len  = offset;
    uint32_t                   parse_offset = sizeof(struct audio_input_payload);
    struct audio_buffer_header out_h;
    void const                *data;

    // Parse 1
    data = protocol_parse_next_buffer(payload, payload_len, &parse_offset, &out_h);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT32(10, out_h.length);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, out_h.volume);
    TEST_ASSERT_EQUAL_UINT8(0xAA, *(uint8_t const *)data);

    // Parse 2
    data = protocol_parse_next_buffer(payload, payload_len, &parse_offset, &out_h);
    TEST_ASSERT_NOT_NULL(data);
    TEST_ASSERT_EQUAL_UINT32(5, out_h.length);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out_h.volume);
    TEST_ASSERT_EQUAL_UINT8(0xBB, *(uint8_t const *)data);

    // Parse 3 (end)
    data = protocol_parse_next_buffer(payload, payload_len, &parse_offset, &out_h);
    TEST_ASSERT_NULL(data);
}
