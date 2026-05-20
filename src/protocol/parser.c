#include "machaudio/protocol.h"

#include <arpa/inet.h>
#include <string.h>

bool protocol_validate_header(AudioMsgHeader const *const header) {
    if (unlikely(header == NULL)) {
        return false;
    }

    if (unlikely(header->magic != AUDIO_MAGIC)) {
        return false;
    }

    if (unlikely(header->version != AUDIO_VERSION)) {
        return false;
    }

    return true;
}

void protocol_decode_header(AudioMsgHeader *const header) {
    if (header == NULL) {
        return;
    }
    header->magic       = ntohl(header->magic);
    header->version     = ntohs(header->version);
    header->command     = ntohs(header->command);
    header->sequence_id = ntohl(header->sequence_id);
    header->payload_len = ntohl(header->payload_len);
}

void const *protocol_parse_next_buffer(
    void const *const                 payload_start,
    uint32_t const                    payload_len,
    uint32_t *const                   offset,
    struct audio_buffer_header *const out_header) {
    if (payload_start == NULL || offset == NULL || out_header == NULL) {
        return NULL;
    }

    // Check if we have enough space for the buffer header
    if (*offset + sizeof(struct audio_buffer_header) > payload_len) {
        return NULL;
    }

    struct audio_buffer_header const *const bh =
        (struct audio_buffer_header const *)((uint8_t const *)payload_start + *offset);

    // Copy and decode (float handling: copying bits is fine for network-to-host if IEEE 754)
    out_header->length = ntohl(bh->length);
    uint32_t vol_net_bits;
    memcpy(&vol_net_bits, &bh->volume, sizeof(uint32_t));
    out_header->volume = protocol_net_to_float(vol_net_bits);

    *offset += sizeof(struct audio_buffer_header);

    // Check if the data fits in the payload
    if (*offset + out_header->length > payload_len) {
        return NULL;
    }

    void const *const data_ptr = (uint8_t const *)payload_start + *offset;

    // Advance offset by data length, padded to 4-byte boundary
    *offset += (out_header->length + 3) & ~3U;

    return data_ptr;
}
