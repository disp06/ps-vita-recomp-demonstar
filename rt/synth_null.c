#include "synth.h"
void synth_init(void) {}
void synth_reset(void) {}
void synth_all_notes_off(void) {}
void synth_short(uint32_t msg) { (void)msg; }
void synth_long(const uint8_t *d, uint32_t l) { (void)d; (void)l; }
void synth_render(float *s, int f) { (void)s; (void)f; }
