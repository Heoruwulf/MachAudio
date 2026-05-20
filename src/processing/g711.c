#include <stdint.h>
#include "machaudio/transcode.h"

/*
 * G.711 PCMU (u-law) and PCMA (a-law) Encoding
 */

uint8_t linear_to_ulaw(int16_t pcm_val) {
    int16_t mask;
    int16_t seg;
    uint8_t uval;

    if (pcm_val < 0) {
        pcm_val = -pcm_val;
        mask    = 0x7F;
    } else {
        mask = 0xFF;
    }
    if (pcm_val > 32635)
        pcm_val = 32635;
    pcm_val += 128;

    if (pcm_val < 256)
        seg = 0;
    else if (pcm_val < 512)
        seg = 1;
    else if (pcm_val < 1024)
        seg = 2;
    else if (pcm_val < 2048)
        seg = 3;
    else if (pcm_val < 4096)
        seg = 4;
    else if (pcm_val < 8192)
        seg = 5;
    else if (pcm_val < 16384)
        seg = 6;
    else
        seg = 7;

    uval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 3)) & 0xF));
    return (uint8_t)(uval ^ mask);
}

uint8_t linear_to_alaw(int16_t pcm_val) {
    int16_t mask;
    int16_t seg;
    uint8_t aval;

    if (pcm_val >= 0) {
        mask = 0xD5;
    } else {
        mask    = 0x55;
        pcm_val = -pcm_val - 1;
    }

    if (pcm_val < 0)
        pcm_val = 0;

    if (pcm_val < 256)
        seg = 0;
    else if (pcm_val < 512)
        seg = 1;
    else if (pcm_val < 1024)
        seg = 2;
    else if (pcm_val < 2048)
        seg = 3;
    else if (pcm_val < 4096)
        seg = 4;
    else if (pcm_val < 8192)
        seg = 5;
    else if (pcm_val < 16384)
        seg = 6;
    else
        seg = 7;

    if (seg == 0)
        aval = (uint8_t)((pcm_val >> 4) & 0x0F);
    else
        aval = (uint8_t)((seg << 4) | ((pcm_val >> (seg + 3)) & 0x0F));

    return (uint8_t)(aval ^ mask);
}
