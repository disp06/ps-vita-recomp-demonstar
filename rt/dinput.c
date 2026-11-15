/* DirectInput (keyboard + mouse) */
#include "win32.h"

#define METHOD(name, nargs) static uint32_t name##_impl(void); static void name(void) { uint32_t r = name##_impl(); R.eax = r; R.esp += 4 + 4 * (nargs); } static uint32_t name##_impl(void)
static uint32_t vt_di, vt_dev, di_obj;
static uint8_t dik[256];
/* buffered keyboard events */
typedef struct { uint32_t ofs, data, ts, seq; } dievt_t;
static dievt_t evq[128]; static int eh, et; static uint32_t seqn;

void dinput_update(const host_input_t *in) {
    static const struct { uint32_t bit; uint8_t sc; } m[] = {
        {HB_UP, 0xc8}, {HB_DOWN, 0xd0}, {HB_LEFT, 0xcb}, {HB_RIGHT, 0xcd}, {HB_FIRE, 0x1d},
        {HB_BOMB, 0x39}, {HB_START, 0x3c}, {HB_SELECT, 0x01}, {HB_TRIANGLE, 0x1c}, {HB_SQUARE, 0x3d},
    };
    for (unsigned k = 0; k < sizeof m / sizeof m[0]; k++) {
        uint8_t v = (in->buttons & m[k].bit) ? 0x80 : 0;
        if (m[k].bit & (HB_UP | HB_DOWN | HB_LEFT | HB_RIGHT)) {
            /* stick handled by caller via buttons */
        }
        if (dik[m[k].sc] != v) {
            dik[m[k].sc] = v;
            int n = (et + 1) & 127;
            if (n != eh) { evq[et] = (dievt_t){m[k].sc, v, host_ticks_ms(), seqn++}; et = n; }
        }
    }
}

void dinput_key_event(uint8_t sc, int down) {
    int n = (et + 1) & 127;
    if (n != eh) { evq[et] = (dievt_t){sc, down ? 0x80u : 0u, host_ticks_ms(), seqn++}; et = n; }
}
typedef struct { uint32_t obj; int kind; } dev_t_;   /* kind 0 keyboard 1 mouse */
METHOD(di_qi, 3) { wr32(ARG(2), ARG(0)); return 0; }
METHOD(di_addref, 1) { return 2; }
METHOD(di_release, 1) { return 0; }
METHOD(di_createdevice, 4) {
    uint32_t g = rd32(ARG(1));
    uint32_t o = guest_alloc(12);
    wr32(o, vt_dev);
    int kind = (g == 0x6F1D2B61u) ? 0 : (g == 0x6F1D2B60u) ? 1 : 2;
    wr32(o + 4, kind);
    rt_log("DirectInput CreateDevice guid %08x kind %d\n", g, kind);
    if (kind == 2) return 0x80004005u;
    wr32(ARG(2), o);
    return 0;
}
METHOD(di_enumdevices, 5) { return 0; }   /* no joysticks */
METHOD(di_getstatus, 2) { return 1; }      /* DI_NOTATTACHED */
METHOD(di_ok1, 2) { return 0; }
METHOD(di_init, 3) { return 0; }

METHOD(dv_getcaps, 2) { uint32_t c = ARG(1); uint32_t n = rd32(c); if (n < 8 || n > 512) n = 96; memset(MEM + c + 4, 0, n - 4); wr32(c + 4, 1); wr32(c + 8, 0x13); return 0; }
METHOD(dv_enumobjects, 4) { return 0; }
METHOD(dv_getprop, 3) { return 0x80004001u; }
METHOD(dv_setprop, 3) { return 0; }
METHOD(dv_acquire, 1) { return 0; }
METHOD(dv_unacquire, 1) { return 0; }
METHOD(dv_getstate, 3) {
    uint32_t o = ARG(0), n = ARG(1), p = ARG(2);
    win_pump_host();
    int kind = rd32(o + 4);
    if (kind == 0) { memcpy(MEM + p, dik, n > 256 ? 256 : n); }
    else memset(MEM + p, 0, n);
    return 0;
}
METHOD(dv_getdata, 5) {
    uint32_t o = ARG(0), sz = ARG(1), data = ARG(2), inout = ARG(3), flags = ARG(4);
    int kind = rd32(o + 4);
    uint32_t want = rd32(inout), got = 0;
    if (kind != 0) { wr32(inout, 0); return 0; }
    int h = eh;
    while (h != et && got < want) {
        if (data) {
            uint32_t a = data + got * sz;
            wr32(a, evq[h].ofs); wr32(a + 4, evq[h].data); wr32(a + 8, evq[h].ts); wr32(a + 12, evq[h].seq);
        }
        got++; h = (h + 1) & 127;
    }
    if (!(flags & 1)) eh = h;   /* DIGDD_PEEK */
    wr32(inout, got);
    return 0;
}
METHOD(dv_setdataformat, 2) { return 0; }
METHOD(dv_setevent, 2) { return 0; }
METHOD(dv_setcoop, 3) { return 0; }
METHOD(dv_getobjinfo, 4) { return 0x80004001u; }
METHOD(dv_getdevinfo, 2) { return 0x80004001u; }
METHOD(dv_unimpl2, 3) { return 0x80004001u; }
METHOD(dv_unimpl1, 2) { return 0x80004001u; }
METHOD(dv_unimpl4, 5) { return 0x80004001u; }
METHOD(dv_poll, 1) { return 0; }
METHOD(dv_unimpl3, 4) { return 0x80004001u; }

SHIM(DINPUT, DirectInputCreateA, 4) {
    if (!vt_di) {
        void (*a[8])(void) = {di_qi, di_addref, di_release, di_createdevice, di_enumdevices, di_getstatus, di_ok1, di_init};
        vt_di = make_vtable(a, 8);
        void (*b[27])(void) = {di_qi, di_addref, di_release, dv_getcaps, dv_enumobjects, dv_getprop, dv_setprop, dv_acquire, dv_unacquire,
                               dv_getstate, dv_getdata, dv_setdataformat, dv_setevent, dv_setcoop, dv_getobjinfo, dv_getdevinfo, di_ok1, di_init,
                               dv_unimpl4, dv_unimpl4, dv_unimpl2, dv_unimpl1, dv_unimpl1, dv_unimpl3, dv_unimpl1, dv_poll, dv_unimpl4};
        vt_dev = make_vtable(b, 27);
        di_obj = guest_alloc(8); wr32(di_obj, vt_di);
    }
    wr32(ARG(2), di_obj);
    return 0;
}
