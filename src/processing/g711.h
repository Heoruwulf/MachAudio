#ifndef MACHAUDIO_G711_H
#define MACHAUDIO_G711_H

#include <stdint.h>

uint8_t linear_to_ulaw(int16_t pcm_val);
uint8_t linear_to_alaw(int16_t pcm_val);

#endif // MACHAUDIO_G711_H
