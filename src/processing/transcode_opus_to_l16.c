#include <opus/opus.h>
#include <stddef.h>
#include "machaudio/transcode.h"

int transcode_opus_to_l16(
    TranscodeSession *const session,
    uint8_t const *restrict const in_data,
    size_t const in_len,
    int16_t *restrict const out_data) {

    if (unlikely(
            session == NULL || session->opus_decoder == NULL || in_data == NULL ||
            out_data == NULL))
    {
        return -1;
    }

    // Opus supports up to 120ms of audio per packet.
    // 120ms at 48kHz = 5760 samples per channel.
    int const max_samples_per_channel = 5760;

    int const decoded_samples = opus_decode(
        session->opus_decoder,
        in_data,
        (opus_int32)in_len,
        out_data,
        max_samples_per_channel,
        0); // 0 for no FEC

    return decoded_samples; // Returns number of samples per channel, or negative error code
}
