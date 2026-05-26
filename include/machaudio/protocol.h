#ifndef MACHAUDIO_PROTOCOL_H
#define MACHAUDIO_PROTOCOL_H

#include <arpa/inet.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "machaudio/macros.h"

#define AUDIO_MAGIC   0x4D414348 // 'MACH' in ASCII
#define AUDIO_VERSION 0x0001

typedef enum {
    CMD_START          = 0x01, // Initializes a processing session
    CMD_INPUT          = 0x02, // Incoming audio data to be processed
    CMD_OUTPUT         = 0x03, // Reply from server containing processed buffer
    CMD_STOP           = 0x04, // Closes resources and terminates session
    CMD_ERROR          = 0x05, // Server error notification
    CMD_PING           = 0x06, // Keep-alive request
    CMD_PONG           = 0x07, // Keep-alive response
    CMD_DISCOVER       = 0x08, // Client requests cluster topology
    CMD_DISCOVER_REPLY = 0x09, // Server response with topology info
} AudioCommand;

typedef enum {
    ERR_NONE                = 0,
    ERR_INVALID_MAGIC       = 1,
    ERR_UNSUPPORTED_VERSION = 2,
    ERR_INVALID_COMMAND     = 3,
    ERR_INVALID_PAYLOAD     = 4,
    ERR_PROCESSING_FAILED   = 5,
    ERR_INTERNAL_ERROR      = 6,
} AudioErrorCode;

typedef struct {
    uint32_t magic;       // AUDIO_MAGIC
    uint16_t version;     // AUDIO_VERSION
    uint16_t command;     // AudioCommand
    uint32_t sequence_id; // For async correlation
    uint32_t payload_len; // Length of following payload
} AudioMsgHeader;

#define AUDIO_START_FLAGS_VAD_ENABLED 0x0001

// CMD_START payload (16 bytes, aligned)
struct audio_start_payload {
    uint8_t  in_payload_type; // e.g., 0 (PCMU), 8 (PCMA), 96 (L16), 111 (Opus)
    uint8_t  in_channels;     // 1 or 2
    uint16_t flags;           // AUDIO_START_FLAGS_*
    uint32_t in_sample_rate;  // e.g., 8000, 16000

    uint8_t  in_endian;        // 0 (none), 1 (little-endian), 2 (big-endian)
    uint8_t  out_payload_type; // e.g., 96 (L16)
    uint8_t  out_channels;     // 1 or 2
    uint8_t  out_endian;       // 0 (none), 1 (little-endian), 2 (big-endian)
    uint32_t out_sample_rate;  // e.g., 16000
};

// CMD_INPUT payload: Multi-buffer container
struct audio_input_payload {
    uint32_t num_buffers;
    uint32_t reserved; // Padding for 8-byte alignment if needed, or just 4-byte
};

// Header for each buffer within CMD_INPUT
struct audio_buffer_header {
    uint32_t length; // Length of raw data following this header
    float    volume; // IEEE 754 float, 0.0 to 1.0
};

// CMD_OUTPUT payload: duration + VAD probability + raw data
struct audio_output_payload {
    uint64_t duration_ns; // Time spent in transcoding pipeline in nanoseconds
    float    vad_prob;    // VAD probability (-1.0f if disabled, 0.0f to 1.0f if enabled)
    uint8_t  reserved[4]; // Padding to maintain 8-byte alignment for data[]
    uint8_t  data[];      // Flexible array member for raw audio
};

// CMD_ERROR payload
struct audio_error_payload {
    uint32_t error_code; // AudioErrorCode
};

// CMD_DISCOVER_REPLY payload (aligned to 8 bytes)
struct audio_discover_reply_payload {
    uint32_t num_workers; // Total number of running worker instances
    uint32_t reserved;    // Padding for alignment
};

/**
 * Validates the header.
 * Returns true if the header is valid (correct magic, supported version).
 */
bool protocol_validate_header(AudioMsgHeader const *const header);

/**
 * Decodes the header from network byte order to host byte order.
 */
void protocol_decode_header(AudioMsgHeader *const header);

/**
 * Parses the next buffer in a CMD_INPUT multi-buffer payload.
 * Returns a pointer to the raw audio data, or NULL if parsing fails or end of payload.
 */
void const *protocol_parse_next_buffer(
    void const *const                 payload_start,
    uint32_t const                    payload_len,
    uint32_t *const                   offset,
    struct audio_buffer_header *const out_header);

/**
 * Serializes a local float to network byte order uint32_t.
 */
static inline uint32_t protocol_float_to_net(float const value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return htonl(bits);
}

/**
 * Deserializes a network byte order uint32_t to a local float.
 */
static inline float protocol_net_to_float(uint32_t const value) {
    uint32_t const bits = ntohl(value);
    float          f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

#endif // MACHAUDIO_PROTOCOL_H
