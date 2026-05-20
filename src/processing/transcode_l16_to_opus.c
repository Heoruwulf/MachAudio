#include <opus/opus.h>
#include <stddef.h>
#include "machaudio/transcode.h"

int transcode_l16_to_opus(
    TranscodeSession *const session,
    int16_t const *restrict const in_data,
    size_t const in_samples,
    uint8_t *restrict const out_data,
    size_t const max_out_len) {

    if (unlikely(
            session == NULL || session->opus_encoder == NULL || in_data == NULL ||
            out_data == NULL))
    {
        return -1;
    }

    // The OPUS_APPLICATION_VOIP profile is already set on this encoder instance
    // when transcode_session_init creates it.

    int const encoded_bytes = opus_encode(
        session->opus_encoder,
        in_data,
        (int)in_samples,
        out_data,
        (opus_int32)max_out_len);

    return encoded_bytes; // Returns length of the encoded packet, or negative error code
}
