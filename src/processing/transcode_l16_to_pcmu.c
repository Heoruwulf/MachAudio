#include "g711.h"
#include "machaudio/transcode.h"

#include <stddef.h>

int transcode_l16_to_pcmu(
    TranscodeSession *const session,
    int16_t const *restrict const in_data,
    size_t const in_len,
    uint8_t *restrict const out_data) {
    (void)session;

    if (unlikely(in_data == NULL || out_data == NULL)) {
        return -1;
    }

    for (size_t i = 0; i < in_len; ++i) {
        out_data[i] = linear_to_ulaw(in_data[i]);
    }

    return (int)in_len;
}
