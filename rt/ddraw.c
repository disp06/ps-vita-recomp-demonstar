/* DirectDraw (v1/v2/v3 interfaces) emulation on top of an 8bpp software framebuffer */
#include "win32.h"

uint8_t *screen_pix; int screen_w = 640, screen_h = 480, screen_pitch;
uint32_t screen_pal[256];

typedef struct {
    int used; uint32_t obj; uint32_t caps; int w, h, pitch; uint32_t bits;
    int back;          /* index of attached back buffer, -1 */
    int primary;
    uint32_t ck_lo, ck_hi; int has_ck;
    int pal;           /* palette index or -1 */
    int locked;
    int refs;
} surf_t;
#define MAXS 128
static surf_t surfs[MAXS];
typedef struct { int used; uint32_t obj; uint32_t pal[256]; int refs; } dpal_t;
static dpal_t pals[16];
static int primary_idx = -1, primary_pal = -1;
static int disp_w = 640, disp_h = 480, disp_bpp = 8;

static uint32_t vt_dd1, vt_dd2, vt_surf, vt_pal, vt_clip;
static uint32_t dd_obj1, dd_obj2;

static int surf_of(uint32_t obj) { return obj ? (int)rd32(obj + 4) : -1; }
static surf_t *S(uint32_t obj) { int i = surf_of(obj); return (i >= 0 && i < MAXS && surfs[i].used) ? &surfs[i] : NULL; }

#define METHOD(name, nargs) static uint32_t name##_impl(void); static void name(void) { uint32_t r = name##_impl(); R.eax = r; R.esp += 4 + 4 * (nargs); } static uint32_t name##_impl(void)
#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define E_NOINTERFACE 0x80004002u

void video_present(const uint8_t *pix, int w, int h, int pitch) {
    host_present(pix, w, h, pitch, screen_pal);
    win_pump_host();
}
static void present_primary(void) {
    if (primary_idx < 0) return;
    surf_t *p = &surfs[primary_idx];
    if (primary_pal >= 0) memcpy(screen_pal, pals[primary_pal].pal, sizeof screen_pal);
    video_present(MEM + p->bits, p->w, p->h, p->pitch);
}

static uint32_t new_surface(int w, int h, uint32_t caps) {
    int i;
    for (i = 0; i < MAXS; i++) if (!surfs[i].used) break;
    if (i == MAXS) return 0;
    surf_t *s = &surfs[i];
    memset(s, 0, sizeof *s);
    s->used = 1; s->w = w; s->h = h; s->pitch = (w + 3) & ~3; s->caps = caps; s->back = -1; s->pal = -1; s->refs = 1;
    s->bits = guest_alloc(s->pitch * h + 64);
    s->obj = guest_alloc(16);
    wr32(s->obj, vt_surf); wr32(s->obj + 4, i);
    return s->obj;
}

/* ---- IUnknown ---- */
METHOD(com_addref, 1) { return 2; }
METHOD(com_release, 1) { return 0; }
METHOD(com_unimpl0, 1) { return DDERR_GENERIC; }
METHOD(com_unimpl1, 2) { return DDERR_GENERIC; }
METHOD(com_unimpl2, 3) { return DDERR_GENERIC; }
METHOD(com_unimpl3, 4) { return DDERR_GENERIC; }
METHOD(com_unimpl4, 5) { return DDERR_GENERIC; }
METHOD(com_unimpl5, 6) { return DDERR_GENERIC; }
METHOD(com_ok0, 1) { return DD_OK; }
METHOD(com_ok1, 2) { return DD_OK; }
METHOD(com_ok2, 3) { return DD_OK; }
METHOD(com_ok3, 4) { return DD_OK; }
METHOD(com_ok4, 5) { return DD_OK; }

/* ---- IDirectDraw ---- */
METHOD(dd_qi, 3) {
    uint32_t g = rd32(ARG(1)), out = ARG(2);
    /* IID_IDirectDraw2 = B3A6F3E0-2B43-11CF-A2DE-00AA00B93356 */
    if (g == 0xB3A6F3E0u) { wr32(out, dd_obj2); return DD_OK; }
    if (g == 0x6C14DB80u) { wr32(out, dd_obj1); return DD_OK; }
    rt_log("IDirectDraw::QueryInterface %08x unsupported\n", g);
    wr32(out, 0);
    return E_NOINTERFACE;
}
METHOD(dd_createclipper, 4) {
    uint32_t o = guest_alloc(8); wr32(o, vt_clip);
    wr32(ARG(2), o);
    return DD_OK;
}
METHOD(dd_createpalette, 5) {
    uint32_t ents = ARG(2), out = ARG(3);
    int i;
    for (i = 0; i < 16; i++) if (!pals[i].used) break;
    if (i == 16) return DDERR_GENERIC;
    dpal_t *p = &pals[i];
    memset(p, 0, sizeof *p); p->used = 1; p->refs = 1;
    for (int k = 0; k < 256; k++) {
        uint32_t e = rd32(ents + 4 * k);
        p->pal[k] = ((e & 0xff) << 16) | (e & 0xff00) | ((e >> 16) & 0xff);
    }
    p->obj = guest_alloc(8); wr32(p->obj, vt_pal); wr32(p->obj + 4, i);
    wr32(out, p->obj);
    return DD_OK;
}
METHOD(dd_createsurface, 4) {
    uint32_t sd = ARG(1), out = ARG(2);
    uint32_t flags = rd32(sd + 4), caps = rd32(sd + 104);
    int w = (flags & 4) ? (int)rd32(sd + 12) : disp_w;
    int h = (flags & 2) ? (int)rd32(sd + 8) : disp_h;
    uint32_t o;
    if (caps & 0x200) {   /* primary */
        o = new_surface(disp_w, disp_h, caps | 0x4000);
        surf_t *p = S(o);
        p->primary = 1;
        primary_idx = surf_of(o);
        if ((flags & 0x20) && rd32(sd + 20) >= 1) {
            uint32_t b = new_surface(disp_w, disp_h, 0x4 | 0x4000);
            p->back = surf_of(b);
        }
        rt_log("CreateSurface primary %dx%d backbuffers=%u\n", disp_w, disp_h, (flags & 0x20) ? rd32(sd + 20) : 0);
    } else {
        o = new_surface(w, h, caps);
    }
    if (!o) return DDERR_GENERIC;
    surf_t *s = S(o);
    if (flags & 0x10000) { s->has_ck = 1; s->ck_lo = rd32(sd + 64); s->ck_hi = rd32(sd + 68); }
    wr32(out, o);
    return DD_OK;
}
static void fill_ddsd(uint32_t sd, surf_t *s) {
    memset(MEM + sd + 4, 0, 104);
    wr32(sd + 4, 1 | 2 | 4 | 8 | 0x1000);
    wr32(sd + 8, s ? s->h : disp_h); wr32(sd + 12, s ? s->w : disp_w);
    wr32(sd + 16, s ? s->pitch : disp_w);
    if (s) wr32(sd + 36, s->bits);
    /* pixel format: 8bpp palettized */
    wr32(sd + 72, 32); wr32(sd + 76, 0x20 | 0x40); wr32(sd + 84, 8);
    wr32(sd + 104, s ? s->caps : 0);
}
METHOD(dd_enumdisplaymodes, 5) {
    uint32_t cb = ARG(4), ctx = ARG(3);
    static const int modes[][3] = {{320, 200, 8}, {320, 240, 8}, {640, 400, 8}, {640, 480, 8}, {640, 480, 16}, {800, 600, 8}};
    uint32_t sd = guest_alloc(108);
    for (unsigned k = 0; k < sizeof modes / sizeof modes[0]; k++) {
        memset(MEM + sd, 0, 108); wr32(sd, 108);
        wr32(sd + 4, 2 | 4 | 8 | 0x1000);
        wr32(sd + 8, modes[k][1]); wr32(sd + 12, modes[k][0]); wr32(sd + 16, modes[k][0] * modes[k][2] / 8);
        wr32(sd + 72, 32); wr32(sd + 76, modes[k][2] == 8 ? 0x60 : 0x40); wr32(sd + 84, modes[k][2]);
        if (modes[k][2] == 16) { wr32(sd + 88, 0xf800); wr32(sd + 92, 0x7e0); wr32(sd + 96, 0x1f); }
        uint32_t a[2] = {sd, ctx};
        if (guest_call(cb, 2, a) == 0) break;
    }
    guest_free(sd);
    return DD_OK;
}
METHOD(dd_getcaps, 3) {
    uint32_t d = ARG(1), h = ARG(2);
    if (d) { uint32_t n = rd32(d); if (n < 8 || n > 512) n = 316; memset(MEM + d + 4, 0, n - 4); wr32(d + 4, 0x40 | 0x200); wr32(d + 60, 8 * 1024 * 1024); wr32(d + 64, 8 * 1024 * 1024); }
    if (h) { uint32_t n = rd32(h); if (n < 8 || n > 512) n = 316; memset(MEM + h + 4, 0, n - 4); wr32(h + 4, 0x40); }
    return DD_OK;
}
METHOD(dd_getdisplaymode, 2) { fill_ddsd(ARG(1), NULL); return DD_OK; }
METHOD(dd_setcoop, 3) { rt_log("SetCooperativeLevel(%08x)\n", ARG(2)); return DD_OK; }
static uint32_t setmode(int w, int h, int bpp) {
    rt_log("SetDisplayMode %dx%dx%d\n", w, h, bpp);
    if (bpp != 8) return DDERR_GENERIC;
    disp_w = w; disp_h = h; disp_bpp = bpp;
    screen_w = w; screen_h = h;
    host_video_init(w, h);
    return DD_OK;
}
METHOD(dd_setdisplaymode1, 4) { return setmode(ARG(1), ARG(2), ARG(3)); }
METHOD(dd_setdisplaymode2, 6) { return setmode(ARG(1), ARG(2), ARG(3)); }
METHOD(dd_waitvb, 3) {
    static uint32_t last;
    if (g_netplay) { rt_advance_us(16667); return DD_OK; }
    uint32_t now = host_ticks_ms();
    if (now - last < 14) host_sleep_ms(14 - (now - last));
    last = host_ticks_ms();
    return DD_OK;
}
METHOD(dd_getvidmem, 4) { if (ARG(2)) wr32(ARG(2), 8 << 20); if (ARG(3)) wr32(ARG(3), 8 << 20); return DD_OK; }
METHOD(dd_restoremode, 1) { return DD_OK; }
METHOD(dd_scanline, 2) { if (ARG(1)) wr32(ARG(1), 0); return DD_OK; }
METHOD(dd_vbstatus, 2) { if (ARG(1)) wr32(ARG(1), 1); return DD_OK; }
METHOD(dd_monfreq, 2) { if (ARG(1)) wr32(ARG(1), 60); return DD_OK; }

/* ---- IDirectDrawSurface ---- */
METHOD(s_qi, 3) {
    /* allow requesting Surface2/3: same object (vtable is a superset) */
    wr32(ARG(2), ARG(0));
    return DD_OK;
}
METHOD(s_release, 1) { surf_t *s = S(ARG(0)); if (s && --s->refs <= 0 && !s->primary) { /* keep memory: games may reuse */ s->refs = 0; } return 0; }
static void rect_of(uint32_t r, surf_t *s, int *x0, int *y0, int *x1, int *y1) {
    if (r) { *x0 = rd32(r); *y0 = rd32(r + 4); *x1 = rd32(r + 8); *y1 = rd32(r + 12); }
    else { *x0 = 0; *y0 = 0; *x1 = s->w; *y1 = s->h; }
}
static void do_blt(surf_t *d, int dx0, int dy0, int dx1, int dy1, surf_t *s, int sx0, int sy0, int sx1, int sy1, int ck) {
    int dw = dx1 - dx0, dh = dy1 - dy0, sw = sx1 - sx0, sh = sy1 - sy0;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    uint8_t key = (uint8_t)s->ck_lo;
    for (int y = 0; y < dh; y++) {
        int yy = dy0 + y; if (yy < 0 || yy >= d->h) continue;
        int sy = sy0 + (int)((int64_t)y * sh / dh); if (sy < 0 || sy >= s->h) continue;
        uint8_t *dp = MEM + d->bits + yy * d->pitch;
        const uint8_t *sp = MEM + s->bits + sy * s->pitch;
        if (dw == sw && !ck && dx0 >= 0 && dx1 <= d->w && sx0 >= 0 && sx1 <= s->w) { memmove(dp + dx0, sp + sx0, dw); continue; }
        for (int x = 0; x < dw; x++) {
            int xx = dx0 + x; if (xx < 0 || xx >= d->w) continue;
            int sx = sx0 + (int)((int64_t)x * sw / dw); if (sx < 0 || sx >= s->w) continue;
            uint8_t c = sp[sx];
            if (ck && c == key) continue;
            dp[xx] = c;
        }
    }
}
METHOD(s_blt, 6) {
    surf_t *d = S(ARG(0));
    uint32_t dr = ARG(1), so = ARG(2), sr = ARG(3), fl = ARG(4), fx = ARG(5);
    if (!d) return DDERR_GENERIC;
    int dx0, dy0, dx1, dy1;
    rect_of(dr, d, &dx0, &dy0, &dx1, &dy1);
    if (fl & 0x400) {   /* COLORFILL */
        uint8_t c = (uint8_t)rd32(fx + 80);
        if (dx0 < 0) dx0 = 0; if (dy0 < 0) dy0 = 0; if (dx1 > d->w) dx1 = d->w; if (dy1 > d->h) dy1 = d->h;
        for (int y = dy0; y < dy1; y++) memset(MEM + d->bits + y * d->pitch + dx0, c, dx1 - dx0);
    } else {
        surf_t *s = S(so);
        if (!s) return DDERR_GENERIC;
        int sx0, sy0, sx1, sy1;
        rect_of(sr, s, &sx0, &sy0, &sx1, &sy1);
        do_blt(d, dx0, dy0, dx1, dy1, s, sx0, sy0, sx1, sy1, (fl & 0x8000) && s->has_ck);
    }
    if (d->primary) present_primary();
    return DD_OK;
}
METHOD(s_bltfast, 6) {
    surf_t *d = S(ARG(0)), *s = S(ARG(3));
    int x = ARG(1), y = ARG(2);
    uint32_t sr = ARG(4), tr = ARG(5);
    if (!d || !s) return DDERR_GENERIC;
    int sx0, sy0, sx1, sy1;
    rect_of(sr, s, &sx0, &sy0, &sx1, &sy1);
    do_blt(d, x, y, x + sx1 - sx0, y + sy1 - sy0, s, sx0, sy0, sx1, sy1, (tr & 1) && s->has_ck);
    if (d->primary) present_primary();
    return DD_OK;
}
METHOD(s_flip, 3) {
    surf_t *p = S(ARG(0));
    if (!p) return DDERR_GENERIC;
    if (p->back >= 0) {
        surf_t *b = &surfs[p->back];
        uint32_t t = p->bits; p->bits = b->bits; b->bits = t;
    }
    present_primary();
    return DD_OK;
}
METHOD(s_getattached, 3) {
    surf_t *p = S(ARG(0));
    if (!p || p->back < 0) return DDERR_GENERIC;
    wr32(ARG(2), surfs[p->back].obj);
    return DD_OK;
}
METHOD(s_getcaps, 2) { surf_t *s = S(ARG(0)); wr32(ARG(1), s ? s->caps : 0); return DD_OK; }
METHOD(s_getsurfacedesc, 2) { fill_ddsd(ARG(1), S(ARG(0))); return DD_OK; }
METHOD(s_getpixelformat, 2) { uint32_t p = ARG(1); memset(MEM + p + 4, 0, 28); wr32(p, 32); wr32(p + 4, 0x60); wr32(p + 12, 8); return DD_OK; }
METHOD(s_islost, 1) { return DD_OK; }
METHOD(s_restore, 1) { return DD_OK; }
METHOD(s_lock, 5) {
    surf_t *s = S(ARG(0));
    if (!s) return DDERR_GENERIC;
    uint32_t sd = ARG(2), r = ARG(1);
    fill_ddsd(sd, s);
    if (r) wr32(sd + 36, s->bits + rd32(r + 4) * s->pitch + rd32(r));
    s->locked = 1;
    return DD_OK;
}
METHOD(s_unlock, 2) {
    surf_t *s = S(ARG(0));
    if (s) { s->locked = 0; if (s->primary) present_primary(); }
    return DD_OK;
}
METHOD(s_setcolorkey, 3) {
    surf_t *s = S(ARG(0));
    if (!s) return DDERR_GENERIC;
    if (ARG(2)) { s->has_ck = 1; s->ck_lo = rd32(ARG(2)); s->ck_hi = rd32(ARG(2) + 4); } else s->has_ck = 0;
    return DD_OK;
}
METHOD(s_getcolorkey, 3) { surf_t *s = S(ARG(0)); if (!s || !s->has_ck) return DDERR_GENERIC; wr32(ARG(2), s->ck_lo); wr32(ARG(2) + 4, s->ck_hi); return DD_OK; }
METHOD(s_setpalette, 2) {
    surf_t *s = S(ARG(0));
    uint32_t po = ARG(1);
    int pi = po ? (int)rd32(po + 4) : -1;
    if (s) { s->pal = pi; if (s->primary) primary_pal = pi; }
    if (s && s->primary && pi >= 0) memcpy(screen_pal, pals[pi].pal, sizeof screen_pal);
    return DD_OK;
}
METHOD(s_getpalette, 2) {
    surf_t *s = S(ARG(0));
    if (!s || s->pal < 0) return DDERR_GENERIC;
    wr32(ARG(1), pals[s->pal].obj);
    return DD_OK;
}
METHOD(s_getdc, 2) { wr32(ARG(1), H_DC + 1); return DD_OK; }
METHOD(s_releasedc, 2) { return DD_OK; }
METHOD(s_setclipper, 2) { return DD_OK; }
METHOD(s_getbltstatus, 2) { return DD_OK; }
METHOD(s_getflipstatus, 2) { return DD_OK; }
METHOD(s_getddinterface, 2) { wr32(ARG(1), dd_obj1); return DD_OK; }

/* ---- IDirectDrawPalette ---- */
METHOD(p_getentries, 5) {
    uint32_t o = ARG(0); int i = rd32(o + 4);
    uint32_t base = ARG(2), n = ARG(3), e = ARG(4);
    for (uint32_t k = 0; k < n && base + k < 256; k++) {
        uint32_t c = pals[i].pal[base + k];
        wr32(e + 4 * k, ((c >> 16) & 0xff) | (c & 0xff00) | ((c & 0xff) << 16));
    }
    return DD_OK;
}
METHOD(p_setentries, 5) {
    uint32_t o = ARG(0); int i = rd32(o + 4);
    uint32_t start = ARG(2), n = ARG(3), e = ARG(4);
    for (uint32_t k = 0; k < n && start + k < 256; k++) {
        uint32_t c = rd32(e + 4 * k);
        pals[i].pal[start + k] = ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
    }
    if (i == primary_pal) memcpy(screen_pal, pals[i].pal, sizeof screen_pal);
    return DD_OK;
}
METHOD(p_getcaps, 2) { wr32(ARG(1), 4 | 0x40); return DD_OK; }

/* ---- IDirectDrawClipper ---- */
METHOD(c_sethwnd, 3) { return DD_OK; }
METHOD(c_gethwnd, 2) { wr32(ARG(1), main_hwnd); return DD_OK; }

static void build_vtables(void) {
    void (*dd1[23])(void) = {
        dd_qi, com_addref, com_release, com_ok0, dd_createclipper, dd_createpalette, dd_createsurface, com_unimpl2,
        dd_enumdisplaymodes, com_unimpl4, com_ok0, dd_getcaps, dd_getdisplaymode, com_unimpl2, com_unimpl1,
        dd_monfreq, dd_scanline, dd_vbstatus, com_unimpl1, dd_restoremode, dd_setcoop, dd_setdisplaymode1, dd_waitvb};
    void (*dd2[24])(void);
    memcpy(dd2, dd1, sizeof dd1);
    dd2[21] = dd_setdisplaymode2;
    dd2[23] = dd_getvidmem;
    vt_dd1 = make_vtable(dd1, 23);
    vt_dd2 = make_vtable(dd2, 24);
    void (*sv[40])(void) = {
        s_qi, com_addref, s_release, com_ok1, com_unimpl1, s_blt, com_unimpl3, s_bltfast, com_ok2, com_unimpl2,
        com_unimpl3, s_flip, s_getattached, s_getbltstatus, s_getcaps, com_unimpl1, s_getcolorkey, s_getdc, s_getflipstatus, com_unimpl2,
        s_getpalette, s_getpixelformat, s_getsurfacedesc, com_unimpl2, s_islost, s_lock, s_releasedc, s_restore, s_setclipper, s_setcolorkey,
        com_unimpl2, s_setpalette, s_unlock, com_unimpl5, com_ok1, com_unimpl2, s_getddinterface, com_ok1, com_ok1, com_unimpl2};
    vt_surf = make_vtable(sv, 40);
    void (*pv[7])(void) = {com_unimpl2, com_addref, com_release, p_getcaps, p_getentries, com_unimpl3, p_setentries};
    vt_pal = make_vtable(pv, 7);
    void (*cv[9])(void) = {com_unimpl2, com_addref, com_release, com_unimpl3, c_gethwnd, com_unimpl2, com_unimpl1, com_unimpl2, c_sethwnd};
    vt_clip = make_vtable(cv, 9);
}

SHIM(DDRAW, DirectDrawCreate, 3) {
    if (!vt_dd1) build_vtables();
    if (!dd_obj1) {
        dd_obj1 = guest_alloc(8); wr32(dd_obj1, vt_dd1);
        dd_obj2 = guest_alloc(8); wr32(dd_obj2, vt_dd2);
    }
    wr32(ARG(1), dd_obj1);
    rt_log("DirectDrawCreate -> %08x\n", dd_obj1);
    return DD_OK;
}
