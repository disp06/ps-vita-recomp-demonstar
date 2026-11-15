#pragma once
#include <stdint.h>
void synth_init(void);
void synth_reset(void);
void synth_all_notes_off(void);
void synth_short(uint32_t msg);
void synth_long(const uint8_t *data, uint32_t len);
void synth_render(float *stereo, int frames);   /* adds into buffer */
