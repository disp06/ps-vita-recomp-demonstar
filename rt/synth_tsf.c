/* General MIDI synth (TinySoundFont + GeneralUser GS soundfont) */
#define TSF_IMPLEMENTATION
#define TSF_NO_STDIO_
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tsf.h"
#include "synth.h"
#include "host.h"
void rt_log(const char *fmt, ...);

static tsf *T;
static int tried;
const char *host_soundfont_path(void);

void synth_init(void) {
    if (T || tried) return;
    tried = 1;
    const char *p = host_soundfont_path();
    T = tsf_load_filename(p);
    if (!T) { rt_log("soundfont %s not found, music disabled\n", p); return; }
    tsf_set_output(T, TSF_STEREO_INTERLEAVED, HOST_AUDIO_RATE, -3.0f);
    tsf_set_max_voices(T, 48);
    synth_reset();
}
void synth_reset(void) {
    if (!T) return;
    host_audio_lock();
    tsf_reset(T);
    for (int c = 0; c < 16; c++) {
        tsf_channel_set_bank_preset(T, c, c == 9 ? 128 : 0, 0);
        tsf_channel_set_volume(T, c, 100 / 127.0f);
        tsf_channel_set_pan(T, c, 0.5f);
        tsf_channel_set_pitchrange(T, c, 2.0f);
    }
    host_audio_unlock();
}
void synth_all_notes_off(void) {
    if (!T) return;
    host_audio_lock();
    tsf_note_off_all(T);
    host_audio_unlock();
}
void synth_short(uint32_t m) {
    if (!T) return;
    int st = m & 0xff, d1 = (m >> 8) & 0x7f, d2 = (m >> 16) & 0x7f, ch = st & 15;
    static int dbg = -1; if (dbg < 0) dbg = getenv("DS_MIDIDBG") != NULL;
    if (dbg && (st & 0xf0) != 0x80 && !((st & 0xf0) == 0x90 && !d2)) rt_log("midi %02x %02x %02x\n", st, d1, d2);
    host_audio_lock();
    switch (st & 0xf0) {
    case 0x80: tsf_channel_note_off(T, ch, d1); break;
    case 0x90: if (d2) tsf_channel_note_on(T, ch, d1, d2 / 127.0f); else tsf_channel_note_off(T, ch, d1); break;
    case 0xb0: tsf_channel_midi_control(T, ch, d1, d2); break;
    case 0xc0: tsf_channel_set_presetnumber(T, ch, d1, ch == 9); break;
    case 0xe0: tsf_channel_set_pitchwheel(T, ch, d1 | (d2 << 7)); break;
    }
    host_audio_unlock();
}
void synth_long(const uint8_t *d, uint32_t len) {
    /* GM/GS reset sysex */
    if (len >= 4 && d[0] == 0xf0) {
        if ((len >= 6 && d[1] == 0x7e && d[3] == 0x09) || (len >= 9 && d[1] == 0x41 && d[4] == 0x12 && d[5] == 0x40 && d[6] == 0x00 && d[7] == 0x7f))
            synth_reset();
    }
}
void synth_render(float *buf, int frames) {
    if (!T) return;
    host_audio_lock();
    tsf_render_float(T, buf, frames, 1);
    host_audio_unlock();
}
