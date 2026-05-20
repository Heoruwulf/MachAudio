#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "machaudio/protocol.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < sizeof(AudioMsgHeader)) {
        return 0;
    }

    // Use a copy to allow mutation without altering original fuzzer input if needed
    uint8_t *buffer = malloc(size);
    if (!buffer)
        return 0;
    memcpy(buffer, data, size);

    AudioMsgHeader header;
    memcpy(&header, buffer, sizeof(AudioMsgHeader));
    protocol_decode_header(&header);

    if (protocol_validate_header(&header)) {
        // Only parse further if the header looks valid
        if (header.command == CMD_INPUT && header.payload_len <= (size - sizeof(AudioMsgHeader))) {
            uint32_t                   offset = sizeof(struct audio_input_payload);
            struct audio_buffer_header bh;

            // Limit loop to prevent timeouts on maliciously crafted payloads indicating infinite
            // buffers
            int max_iters = 1000;
            while (max_iters-- > 0) {
                void const *buf_data = protocol_parse_next_buffer(
                    buffer + sizeof(AudioMsgHeader),
                    header.payload_len,
                    &offset,
                    &bh);
                if (buf_data == NULL) {
                    break; // Parsing finished or failed safely
                }
            }
        }
    }

    free(buffer);
    return 0;
}
