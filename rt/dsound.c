/* DirectSound emulation + software mixer */
#include "win32.h"
#include "synth.h"

typedef struct {
    int used; uint32_t obj; uint32_t data; uint32_t size;
    int channels, bits, rate;
    int playing, loop;
    uint64_t pos;          /* 32.32 fixed point byte-frame position */
    uint32_t freq;
    int vol;               /* hundredths of dB, 0 = full */
    int pan;
    int primary;
    int refs;
} sbuf_t;
#define MAXB 128
static sbuf_t bufs[MAXB];
static uint32_t vt_ds, vt_buf, ds_obj;
static int voltab_ready;
static float vol_lin[1001];

#define METHOD(name, nargs) static uint32_t name##_impl(void); static void name(void) { uint32_t r = name##_impl(); R.eax = r; R.esp += 4 + 4 * (nargs); } static uint32_t name##_impl(void)
#define DS_OK 0
#define DSERR_GENERIC 0x80004005u

static sbuf_t *B(uint32_t obj) { if (!obj) return NULL; uint32_t i = rd32(obj + 4); return (i < MAXB && bufs[i].used) ? &bufs[i] : NULL; }

static float db_to_lin(int v) {   /* v in 1/100 dB, -10000..0 */
    if (v >= 0) return 1.0f;
    if (v <= -10000) return 0.0f;
    if (!voltab_ready) { for (int k = 0; k <= 1000; k++) vol_lin[k] = powf(10.0f, -k * 10 / 2000.0f); voltab_ready = 1; }
    return vol_lin[(-v) / 10];
}

static uint32_t new_buffer(uint32_t size, uint32_t wfx) {
    int i;
    for (i = 0; i < MAXB; i++) if (!bufs[i].used) break;
    if (i == MAXB) return 0;
    sbuf_t *b = &bufs[i];
    memset(b, 0, sizeof *b);
    b->used = 1; b->refs = 1;
    b->size = size;
    b->channels = wfx ? rd16(wfx + 2) : 2;
    b->rate = wfx ? rd32(wfx + 4) : 22050;
    b->bits = wfx ? rd16(wfx + 14) : 16;
    b->freq = b->rate;
    b->data = size ? guest_alloc(size + 16) : 0;
    b->obj = guest_alloc(8);
    wr32(b->obj, vt_buf); wr32(b->obj + 4, i);
    return b->obj;
}

METHOD(ds_qi, 3) { wr32(ARG(2), 0); return 0x80004002u; }
METHOD(ds_addref, 1) { return 2; }
METHOD(ds_release, 1) { return 0; }
METHOD(ds_createbuffer, 4) {
    uint32_t d = ARG(1), out = ARG(2);
    uint32_t flags = rd32(d + 4), bytes = rd32(d + 8), wfx = rd32(d + 16);
    uint32_t o;
    if (flags & 1) { o = new_buffer(0, 0); B(o)->primary = 1; }
    else o = new_buffer(bytes, wfx);
    if (!o) return DSERR_GENERIC;
    wr32(out, o);
    return DS_OK;
}
METHOD(ds_getcaps, 2) { uint32_t c = ARG(1); uint32_t n = rd32(c); if (n < 8 || n > 512) n = 96; memset(MEM + c + 4, 0, n - 4); wr32(c + 4, 0x1 | 0x4 | 0x10 | 0x20 | 0x400 | 0x800); wr32(c + 8, 8000); wr32(c + 12, 48000); return DS_OK; }
METHOD(ds_duplicate, 3) {
    sbuf_t *s = B(ARG(1));
    if (!s) return DSERR_GENERIC;
    uint32_t o = new_buffer(0, 0);
    sbuf_t *d = B(o);
    s = B(ARG(1));
    d->data = s->data; d->size = s->size; d->channels = s->channels; d->bits = s->bits; d->rate = s->rate; d->freq = s->freq;
    d->vol = s->vol; d->pan = s->pan;
    wr32(ARG(2), o);
    return DS_OK;
}
METHOD(ds_setcoop, 3) { return DS_OK; }
METHOD(ds_ok1, 1) { return DS_OK; }
METHOD(ds_speaker_get, 2) { if (ARG(1)) wr32(ARG(1), 4); return DS_OK; }
METHOD(ds_speaker_set, 2) { return DS_OK; }
METHOD(ds_init, 2) { return DS_OK; }

METHOD(b_qi, 3) { wr32(ARG(2), 0); return 0x80004002u; }
METHOD(b_release, 1) {
    sbuf_t *b = B(ARG(0));
    if (b) { host_audio_lock(); b->playing = 0; b->used = 0; host_audio_unlock(); }
    return 0;
}
METHOD(b_getcaps, 2) { sbuf_t *b = B(ARG(0)); uint32_t c = ARG(1); wr32(c + 4, 0xe0); wr32(c + 8, b ? b->size : 0); return DS_OK; }
METHOD(b_getpos, 3) {
    sbuf_t *b = B(ARG(0));
    if (!b) return DSERR_GENERIC;
    int fs = b->channels * b->bits / 8;
    uint32_t p = b->size ? (uint32_t)((b->pos >> 32) * fs) % b->size : 0;
    if (ARG(1)) wr32(ARG(1), p);
    if (ARG(2)) wr32(ARG(2), b->size ? (p + 1024 * fs) % b->size : 0);
    return DS_OK;
}
METHOD(b_getformat, 4) {
    sbuf_t *b = B(ARG(0)); uint32_t w = ARG(1);
    if (w && b) { wr16(w, 1); wr16(w + 2, b->channels); wr32(w + 4, b->rate); wr32(w + 8, b->rate * b->channels * b->bits / 8);
                  wr16(w + 12, b->channels * b->bits / 8); wr16(w + 14, b->bits); }
    if (ARG(3)) wr32(ARG(3), 16);
    return DS_OK;
}
METHOD(b_getvolume, 2) { sbuf_t *b = B(ARG(0)); wr32(ARG(1), b ? b->vol : 0); return DS_OK; }
METHOD(b_getpan, 2) { sbuf_t *b = B(ARG(0)); wr32(ARG(1), b ? b->pan : 0); return DS_OK; }
METHOD(b_getfreq, 2) { sbuf_t *b = B(ARG(0)); wr32(ARG(1), b ? b->freq : 0); return DS_OK; }
METHOD(b_getstatus, 2) { sbuf_t *b = B(ARG(0)); wr32(ARG(1), b && b->playing ? (1 | (b->loop ? 4 : 0)) : 0); return DS_OK; }
METHOD(b_init, 3) { return DS_OK; }
METHOD(b_lock, 8) {
    sbuf_t *b = B(ARG(0));
    if (!b) return DSERR_GENERIC;
    uint32_t off = ARG(1), n = ARG(2), flags = ARG(7);
    if (b->primary || !b->size) { wr32(ARG(3), 0); wr32(ARG(4), 0); if (ARG(5)) wr32(ARG(5), 0); if (ARG(6)) wr32(ARG(6), 0); return DS_OK; }
    if (flags & 2) { off = 0; n = b->size; }
    if (flags & 1) { uint32_t p; int fs = b->channels * b->bits / 8; p = (uint32_t)((b->pos >> 32) * fs) % b->size; off = (p + 1024 * fs) % b->size; }
    off %= b->size;
    if (n > b->size) n = b->size;
    uint32_t n1 = n, n2 = 0;
    if (off + n > b->size) { n1 = b->size - off; n2 = n - n1; }
    wr32(ARG(3), b->data + off); wr32(ARG(4), n1);
    if (ARG(5)) wr32(ARG(5), n2 ? b->data : 0);
    if (ARG(6)) wr32(ARG(6), n2);
    return DS_OK;
}
METHOD(b_play, 4) {
    sbuf_t *b = B(ARG(0));
    if (!b) return DSERR_GENERIC;
    host_audio_lock();
    if (!b->playing) {
        if (!b->loop) {}
    }
    b->playing = 1; b->loop = ARG(3) & 1;
    host_audio_unlock();
    return DS_OK;
}
METHOD(b_setpos, 2) {
    sbuf_t *b = B(ARG(0));
    if (!b) return DSERR_GENERIC;
    int fs = b->channels * b->bits / 8;
    host_audio_lock(); b->pos = (uint64_t)(ARG(1) / fs) << 32; host_audio_unlock();
    return DS_OK;
}
METHOD(b_setformat, 2) { return DS_OK; }
METHOD(b_setvolume, 2) { sbuf_t *b = B(ARG(0)); if (b) b->vol = (int32_t)ARG(1); return DS_OK; }
METHOD(b_setpan, 2) { sbuf_t *b = B(ARG(0)); if (b) b->pan = (int32_t)ARG(1); return DS_OK; }
METHOD(b_setfreq, 2) { sbuf_t *b = B(ARG(0)); if (b) b->freq = ARG(1) ? ARG(1) : b->rate; return DS_OK; }
METHOD(b_stop, 1) { sbuf_t *b = B(ARG(0)); if (b) { host_audio_lock(); b->playing = 0; host_audio_unlock(); } return DS_OK; }
METHOD(b_unlock, 5) { return DS_OK; }
METHOD(b_restore, 1) { return DS_OK; }

static void build(void) {
    void (*dv[11])(void) = {ds_qi, ds_addref, ds_release, ds_createbuffer, ds_getcaps, ds_duplicate, ds_setcoop, ds_ok1, ds_speaker_get, ds_speaker_set, ds_init};
    vt_ds = make_vtable(dv, 11);
    void (*bv[21])(void) = {b_qi, ds_addref, b_release, b_getcaps, b_getpos, b_getformat, b_getvolume, b_getpan, b_getfreq, b_getstatus,
                            b_init, b_lock, b_play, b_setpos, b_setformat, b_setvolume, b_setpan, b_setfreq, b_stop, b_unlock, b_restore};
    vt_buf = make_vtable(bv, 21);
}

static uint32_t create(uint32_t out) {
    if (!vt_ds) { build(); ds_obj = guest_alloc(8); wr32(ds_obj, vt_ds); host_audio_init(); }
    wr32(out, ds_obj);
    rt_log("DirectSoundCreate -> %08x\n", ds_obj);
    return DS_OK;
}
SHIM(DSOUND, DirectSoundCreate, 3) { return create(ARG(1)); }
SHIM(DSOUND, ord1, 3) { return create(ARG(1)); }

void dsound_service(void) {}

/* ---- mixer (called from the host audio thread) ---- */
static float mixbuf[4096 * 2];
void audio_mix(int16_t *out, int frames) {
    if (frames > 4096) frames = 4096;
    memset(mixbuf, 0, sizeof(float) * 2 * frames);
    static int nosfx = -1;
    if (nosfx < 0) nosfx = getenv("DS_NOSFX") != NULL;
    host_audio_lock();
    for (int i = 0; i < MAXB && !nosfx; i++) {
        sbuf_t *b = &bufs[i];
        if (!b->used || !b->playing || b->primary || !b->size) continue;
        int fs = b->channels * b->bits / 8;
        uint32_t nfr = b->size / fs;
        if (!nfr) continue;
        uint64_t step = ((uint64_t)b->freq << 32) / HOST_AUDIO_RATE;
        float v = db_to_lin(b->vol);
        float l = v, r = v;
        if (b->pan < 0) r *= db_to_lin(b->pan);
        else if (b->pan > 0) l *= db_to_lin(-b->pan);
        const uint8_t *d = MEM + b->data;
        uint64_t pos = b->pos;
        for (int k = 0; k < frames; k++) {
            uint32_t f = (uint32_t)(pos >> 32);
            if (f >= nfr) {
                if (b->loop) { pos -= (uint64_t)nfr << 32; f = (uint32_t)(pos >> 32); if (f >= nfr) { pos = 0; f = 0; } }
                else { b->playing = 0; pos = 0; break; }
            }
            float sl, sr;
            if (b->bits == 8) {
                sl = (d[f * fs] - 128) / 128.0f;
                sr = b->channels == 2 ? (d[f * fs + 1] - 128) / 128.0f : sl;
            } else {
                int16_t a, c;
                memcpy(&a, d + f * fs, 2);
                if (b->channels == 2) memcpy(&c, d + f * fs + 2, 2); else c = a;
                sl = a / 32768.0f; sr = c / 32768.0f;
            }
            mixbuf[2 * k] += sl * l;
            mixbuf[2 * k + 1] += sr * r;
            pos += step;
        }
        b->pos = pos;
    }
    host_audio_unlock();
    synth_render(mixbuf, frames);
    for (int k = 0; k < 2 * frames; k++) {
        float s = mixbuf[k] * 32767.0f;
        if (s > 32767.0f) s = 32767.0f;
        if (s < -32768.0f) s = -32768.0f;
        out[k] = (int16_t)s;
    }
}
