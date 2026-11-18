/* winmm: multimedia timers, joystick (none), MIDI out + MIDI streams */
#include "win32.h"
#include "synth.h"

SHIM(WINMM, timeGetTime, 0) { winmm_service(); return host_ticks_ms(); }
SHIM(WINMM, timeBeginPeriod, 1) { return 0; }
SHIM(WINMM, timeEndPeriod, 1) { return 0; }
SHIM(WINMM, timeGetDevCaps, 2) { wr32(ARG(0), 1); wr32(ARG(0) + 4, 1000000); return 0; }

typedef struct { int used; uint32_t delay, cb, user, flags, next, id; } mmtimer_t;
static mmtimer_t mmt[16];
SHIM(WINMM, timeSetEvent, 5) {
    for (int k = 0; k < 16; k++) if (!mmt[k].used) {
        mmt[k] = (mmtimer_t){1, ARG(0) ? ARG(0) : 1, ARG(2), ARG(3), ARG(4), host_ticks_ms() + ARG(0), (uint32_t)(k + 1)};
        rt_log("timeSetEvent(%u ms, cb=%08x, flags=%x) -> %d\n", ARG(0), ARG(2), ARG(4), k + 1);
        return k + 1;
    }
    return 0;
}
SHIM(WINMM, timeKillEvent, 1) { uint32_t id = ARG(0); if (id >= 1 && id <= 16) mmt[id - 1].used = 0; return 0; }

/* Joysticks: joystick 1 (id 0) is never present, so player 1 stays on the
   keyboard mapping of the Vita's own controls.  Joystick 2 (id 1) is the
   second controller (Bluetooth pad) when it is connected: the games assign it
   to player 2.  Buttons: 1 = fire, 2 = bomb, 3 = start, 4 = special. */
extern host_input_t g_last_input;
static int joy2_present(void) {
    static int polled;
    if (!polled) { polled = 1; win_pump_host(); }
    return g_last_input.p2_present;
}
/* DemonStar finds joysticks by winmm id and gives "Joystick 2" (id 1) to
   player 2; Secret Missions 1 numbers the DirectInput joysticks it found, so
   the only one (the Bluetooth pad) is its joystick 1 (id 0). */
#include "games.h"
static uint32_t joy_p2_id(void) { return g_game_idx == 1 ? 0 : 1; }
SHIM(WINMM, joyGetNumDevs, 0) { return 2; }
SHIM(WINMM, joyGetDevCapsA, 3) {
    uint32_t id = ARG(0), p = ARG(1), n = ARG(2);
    if (id != joy_p2_id() || !joy2_present()) return 167;   /* JOYERR_UNPLUGGED */
    if (n > 404) n = 404;
    memset(MEM + p, 0, n);
    if (n < 112) return 0;
    wr16(p, 0x2dc8); wr16(p + 2, 0x6001);
    strcpy((char *)MEM + p + 4, "Bluetooth Gamepad");
    wr32(p + 36, 0); wr32(p + 40, 65535); wr32(p + 44, 0); wr32(p + 48, 65535);
    wr32(p + 52, 0); wr32(p + 56, 65535);
    wr32(p + 60, 4); wr32(p + 64, 10); wr32(p + 68, 1000);
    wr32(p + 100, 2); wr32(p + 104, 2); wr32(p + 108, 4);
    return 0;
}
SHIM(WINMM, joyGetPosEx, 2) {
    uint32_t id = ARG(0), p = ARG(1);
    if (id != joy_p2_id() || !joy2_present()) return 167;
    const host_input_t *in = &g_last_input;
    uint32_t b = in->buttons2;
    int x = in->ax2, y = in->ay2;
    if (b & HB_LEFT) x = -128; else if (b & HB_RIGHT) x = 127; else if (x > -40 && x < 40) x = 0;
    if (b & HB_UP) y = -128; else if (b & HB_DOWN) y = 127; else if (y > -40 && y < 40) y = 0;
    uint32_t jb = 0;
    if (b & HB_FIRE) jb |= 1;
    if (b & HB_BOMB) jb |= 2;
    if (b & HB_START) jb |= 4;
    if (b & (HB_TRIANGLE | HB_SELECT)) jb |= 8;
    uint32_t xp = (uint32_t)((x + 128) * 65535 / 255), yp = (uint32_t)((y + 128) * 65535 / 255);
    if (x == 0) xp = 32767;
    if (y == 0) yp = 32767;
    wr32(p + 8, xp); wr32(p + 12, yp); wr32(p + 16, 32767); wr32(p + 20, 32767);
    wr32(p + 24, 32767); wr32(p + 28, 32767);
    wr32(p + 32, jb); wr32(p + 36, __builtin_popcount(jb)); wr32(p + 40, 0xffff);
    return 0;
}

/* ---------------- MIDI ---------------- */
#define HMIDI_OUT 0x5100u
#define HMIDI_STRM 0x5200u
typedef struct {
    int open; uint32_t cb, inst, cbtype;
    uint32_t timediv, tempo;
    uint32_t queue[64]; int qn;      /* MIDIHDR addresses */
    uint32_t cur_off; int playing;
    double next_ms;                   /* time when next event is due */
    int have_delta;
} mstream_t;
static mstream_t ms;
static uint32_t mo_cb, mo_inst, mo_cbtype;

SHIM(WINMM, midiOutGetNumDevs, 0) { return 1; }
SHIM(WINMM, midiOutGetDevCapsA, 3) {
    uint32_t p = ARG(1);
    memset(MEM + p, 0, ARG(2));
    wr16(p, 1); wr16(p + 2, 1); wr32(p + 4, 0x100);
    strcpy((char *)MEM + p + 8, "Software Synth");
    wr16(p + 40, 7); wr16(p + 42, 32); wr16(p + 44, 0xffff); wr32(p + 48, 1);
    return 0;
}
static void mo_callback(uint32_t h, uint32_t msg, uint32_t p1, uint32_t p2, uint32_t cbtype, uint32_t cb, uint32_t inst) {
    if (!cb) return;
    if (cbtype == 0x30000) { uint32_t a[5] = {h, msg, inst, p1, p2}; guest_call(cb, 5, a); }
    else if (cbtype == 0x10000) win_post(cb, msg, h, p1);
}
SHIM(WINMM, midiOutOpen, 5) {
    mo_cb = ARG(2); mo_inst = ARG(3); mo_cbtype = ARG(4) & 0x70000;
    wr32(ARG(0), HMIDI_OUT);
    synth_init();
    mo_callback(HMIDI_OUT, 0x3C7, 0, 0, mo_cbtype, mo_cb, mo_inst);  /* MOM_OPEN */
    return 0;
}
SHIM(WINMM, midiOutClose, 1) { if (ARG(0) == HMIDI_STRM) ms.open = 0; synth_reset(); return 0; }
SHIM(WINMM, midiOutShortMsg, 2) { synth_short(ARG(1)); return 0; }
SHIM(WINMM, midiOutLongMsg, 3) {
    uint32_t hdr = ARG(1);
    synth_long(MEM + rd32(hdr), rd32(hdr + 4));
    wr32(hdr + 16, (rd32(hdr + 16) | 1) & ~4u);
    mo_callback(ARG(0), 0x3C9, hdr, 0, mo_cbtype, mo_cb, mo_inst);
    return 0;
}
SHIM(WINMM, midiOutPrepareHeader, 3) { uint32_t h = ARG(1); wr32(h + 16, (rd32(h + 16) | 2) & ~1u); return 0; }
SHIM(WINMM, midiOutUnprepareHeader, 3) { uint32_t h = ARG(1); wr32(h + 16, rd32(h + 16) & ~2u); return 0; }

static void stream_done_all(void) {
    for (int k = 0; k < ms.qn; k++) {
        uint32_t h = ms.queue[k];
        wr32(h + 16, (rd32(h + 16) | 1) & ~4u);
    }
    int n = ms.qn; uint32_t qq[64]; memcpy(qq, ms.queue, sizeof qq);
    ms.qn = 0; ms.cur_off = 0;
    for (int k = 0; k < n; k++) mo_callback(HMIDI_STRM, 0x3C9, qq[k], 0, ms.cbtype, ms.cb, ms.inst);
}
SHIM(WINMM, midiOutReset, 1) {
    synth_reset();
    if (ARG(0) == HMIDI_STRM) { ms.playing = 0; stream_done_all(); }
    return 0;
}
SHIM(WINMM, midiStreamOpen, 6) {
    memset(&ms, 0, sizeof ms);
    ms.open = 1; ms.cb = ARG(3); ms.inst = ARG(4); ms.cbtype = ARG(5) & 0x70000;
    ms.timediv = 96; ms.tempo = 500000;
    wr32(ARG(0), HMIDI_STRM);
    synth_init();
    mo_callback(HMIDI_STRM, 0x3C7, 0, 0, ms.cbtype, ms.cb, ms.inst);
    return 0;
}
SHIM(WINMM, midiStreamClose, 1) { ms.playing = 0; stream_done_all(); ms.open = 0; synth_reset(); mo_callback(HMIDI_STRM, 0x3C8, 0, 0, ms.cbtype, ms.cb, ms.inst); return 0; }
SHIM(WINMM, midiStreamProperty, 3) {
    uint32_t p = ARG(1), f = ARG(2);
    if (f & 0x80000000u) {
        if (f & 1) ms.timediv = rd32(p + 4) & 0xffff;
        if (f & 2) ms.tempo = rd32(p + 4);
    } else {
        if (f & 1) wr32(p + 4, ms.timediv);
        if (f & 2) wr32(p + 4, ms.tempo);
    }
    return 0;
}
SHIM(WINMM, midiStreamOut, 3) {
    uint32_t h = ARG(1);
    if (ms.qn >= 64) return 1;
    wr32(h + 16, (rd32(h + 16) | 4) & ~1u);
    wr32(h + 28, 0);
    if (ms.qn == 0) { ms.cur_off = 0; ms.have_delta = 0; }
    ms.queue[ms.qn++] = h;
    return 0;
}
SHIM(WINMM, midiStreamRestart, 1) {
    if (!ms.playing) { ms.playing = 1; ms.next_ms = host_ticks_us() / 1000.0; ms.have_delta = 0; }
    return 0;
}
SHIM(WINMM, midiStreamPause, 1) { ms.playing = 0; synth_all_notes_off(); return 0; }

static void stream_service(void) {
    if (!ms.open || !ms.playing) return;
    double now = host_ticks_us() / 1000.0;
    int guard = 0;
    while (ms.qn > 0 && guard++ < 4096) {
        uint32_t h = ms.queue[0];
        uint32_t data = rd32(h), len = rd32(h + 8);
        if (ms.cur_off + 12 > len) {
            /* buffer finished */
            wr32(h + 16, (rd32(h + 16) | 1) & ~4u);
            memmove(ms.queue, ms.queue + 1, sizeof(uint32_t) * (ms.qn - 1));
            ms.qn--; ms.cur_off = 0;
            mo_callback(HMIDI_STRM, 0x3C9, h, 0, ms.cbtype, ms.cb, ms.inst);
            continue;
        }
        uint32_t e = data + ms.cur_off;
        uint32_t delta = rd32(e), ev = rd32(e + 8);
        if (!ms.have_delta) {
            ms.next_ms += (double)delta * ms.tempo / (1000.0 * (ms.timediv ? ms.timediv : 96));
            ms.have_delta = 1;
        }
        if (ms.next_ms > now) break;
        ms.have_delta = 0;
        uint32_t type = (ev >> 24) & 0xff, adv = 12;
        if (type & 0x80) {   /* long event */
            uint32_t l = ev & 0xffffff;
            if ((type & 0x3f) == 0) synth_long(MEM + e + 12, l);
            adv += (l + 3) & ~3u;
        } else {
            switch (type & 0x3f) {
            case 0: synth_short(ev & 0xffffff); break;
            case 1: ms.tempo = ev & 0xffffff; break;
            }
        }
        if (ev & 0x40000000u) mo_callback(HMIDI_STRM, 0x3CA, h, 0, ms.cbtype, ms.cb, ms.inst); /* MOM_POSITIONCB */
        ms.cur_off += adv;
        wr32(h + 28, ms.cur_off);
    }
    if (ms.next_ms < now - 500) ms.next_ms = now;   /* don't try to catch up after stalls */
}

static int in_service;
void winmm_service(void) {
    if (in_service) return;
    in_service = 1;
    uint32_t now = host_ticks_ms();
    for (int k = 0; k < 16; k++) {
        if (!mmt[k].used) continue;
        int fires = 0;
        while (mmt[k].used && (int32_t)(now - mmt[k].next) >= 0 && fires < 8) {
            uint32_t a[5] = {mmt[k].id, 0, mmt[k].user, 0, 0};
            uint32_t cb = mmt[k].cb, fl = mmt[k].flags;
            if (fl & 1) mmt[k].next += mmt[k].delay; else mmt[k].used = 0;   /* TIME_PERIODIC */
            if (fl & 0x10) { /* TIME_CALLBACK_EVENT_SET */ }
            else guest_call(cb, 5, a);
            fires++;
        }
        if (mmt[k].used && (int32_t)(now - mmt[k].next) > 200) mmt[k].next = now;
    }
    stream_service();
    in_service = 0;
}
