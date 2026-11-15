#include "win32.h"

typedef struct { int used, type; uint32_t bits; int w, h, pitch, bpp, topdown; uint32_t pal[256]; } gobj_t;
#define MAXG 64
static gobj_t gobjs[MAXG];
#define GT_BITMAP 1
#define GT_PALETTE 2
#define GT_STOCK 3
static gobj_t *G(uint32_t h) {
    if (h < H_GDI || h >= H_GDI + MAXG) return NULL;
    gobj_t *g = &gobjs[h - H_GDI];
    return g->used ? g : NULL;
}
static uint32_t gnew(int type) {
    for (int k = 1; k < MAXG; k++) if (!gobjs[k].used) { memset(&gobjs[k], 0, sizeof gobjs[k]); gobjs[k].used = 1; gobjs[k].type = type; return H_GDI + k; }
    return 0;
}

/* DCs: H_DC+1 = screen/window DC, others are memory DCs */
typedef struct { int used; uint32_t bmp, pal; } dc_t;
static dc_t dcs[16];
static dc_t *D(uint32_t h) { if (h <= H_DC + 1 || h >= H_DC + 16) return NULL; return dcs[h - H_DC].used ? &dcs[h - H_DC] : NULL; }
static uint32_t screen_dc_pal;

SHIM(GDI32, CreateCompatibleDC, 1) {
    for (int k = 2; k < 16; k++) if (!dcs[k].used) { memset(&dcs[k], 0, sizeof dcs[k]); dcs[k].used = 1; return H_DC + k; }
    return 0;
}
SHIM(GDI32, DeleteDC, 1) { dc_t *d = D(ARG(0)); if (d) d->used = 0; return 1; }
SHIM(GDI32, CreateDIBSection, 6) {
    uint32_t bmi = ARG(1), usage = ARG(2), ppv = ARG(3);
    int w = (int32_t)rd32(bmi + 4), h = (int32_t)rd32(bmi + 8), bpp = rd16(bmi + 14);
    uint32_t clr = rd32(bmi + 32);
    uint32_t hb = gnew(GT_BITMAP);
    gobj_t *g = G(hb);
    g->w = w; g->topdown = h < 0; g->h = h < 0 ? -h : h; g->bpp = bpp;
    g->pitch = ((w * bpp / 8) + 3) & ~3;
    g->bits = guest_alloc(g->pitch * g->h + 16);
    if (bpp == 8) {
        int n = clr ? (int)clr : 256;
        uint32_t ct = bmi + rd32(bmi);
        for (int k = 0; k < n && k < 256; k++) {
            if (usage == 0) { uint32_t q = rd32(ct + 4 * k); g->pal[k] = q & 0xffffff; }
            else g->pal[k] = k * 0x010101;
        }
    }
    if (ppv) wr32(ppv, g->bits);
    rt_log("CreateDIBSection %dx%d bpp=%d usage=%u -> bits %08x\n", w, h, bpp, usage, g->bits);
    return hb;
}
SHIM(GDI32, SetDIBColorTable, 4) {
    dc_t *d = D(ARG(0));
    if (!d) return 0;
    gobj_t *g = G(d->bmp);
    if (!g) return 0;
    uint32_t start = ARG(1), n = ARG(2), p = ARG(3);
    for (uint32_t k = 0; k < n && start + k < 256; k++) g->pal[start + k] = rd32(p + 4 * k) & 0xffffff;
    return n;
}
SHIM(GDI32, SelectObject, 2) {
    dc_t *d = D(ARG(0));
    gobj_t *g = G(ARG(1));
    if (d && g && g->type == GT_BITMAP) { uint32_t o = d->bmp; d->bmp = ARG(1); return o ? o : H_GDI; }
    return H_GDI;
}
SHIM(GDI32, DeleteObject, 1) { gobj_t *g = G(ARG(0)); if (g) { if (g->bits) guest_free(g->bits); g->used = 0; } return 1; }
SHIM(GDI32, GetStockObject, 1) { return H_GDI; }
SHIM(GDI32, SetBkMode, 2) { return 1; }
SHIM(GDI32, TextOutA, 5) { return 1; }
SHIM(GDI32, GetDeviceCaps, 2) {
    switch (ARG(1)) {
    case 12: return 8;      /* BITSPIXEL */
    case 14: return 1;      /* PLANES */
    case 8: return 640;     /* HORZRES */
    case 10: return 480;    /* VERTRES */
    case 38: return 0x100 | 0x1;  /* RASTERCAPS: RC_PALETTE | RC_BITBLT */
    case 104: return 256;   /* SIZEPALETTE */
    case 106: return 20;    /* NUMRESERVED */
    case 24: return 256;    /* NUMCOLORS */
    }
    return 0;
}
/* palettes */
SHIM(GDI32, CreatePalette, 1) {
    uint32_t lp = ARG(0);
    uint32_t h = gnew(GT_PALETTE);
    gobj_t *g = G(h);
    int n = rd16(lp + 2);
    for (int k = 0; k < n && k < 256; k++) {
        uint32_t e = rd32(lp + 4 + 4 * k);
        g->pal[k] = ((e & 0xff) << 16) | (e & 0xff00) | ((e >> 16) & 0xff);
    }
    return h;
}
SHIM(GDI32, SelectPalette, 3) { uint32_t o = screen_dc_pal; if (ARG(0) == H_DC + 1) screen_dc_pal = ARG(1); else { dc_t *d = D(ARG(0)); if (d) d->pal = ARG(1); } return o ? o : H_GDI; }
SHIM(GDI32, RealizePalette, 1) {
    gobj_t *g = G(screen_dc_pal);
    if (g) memcpy(screen_pal, g->pal, sizeof screen_pal);
    return 256;
}
SHIM(GDI32, SetSystemPaletteUse, 2) { return 1; }
SHIM(GDI32, GetSystemPaletteEntries, 4) {
    uint32_t s = ARG(1), n = ARG(2), p = ARG(3);
    if (!p) return 256;
    for (uint32_t k = 0; k < n && s + k < 256; k++) {
        uint32_t c = screen_pal[s + k];
        wr32(p + 4 * k, ((c >> 16) & 0xff) | (c & 0xff00) | ((c & 0xff) << 16));
    }
    return n;
}
static void blit_to_screen(uint32_t srcdc) {
    dc_t *d = D(srcdc);
    if (!d) return;
    gobj_t *g = G(d->bmp);
    if (!g || g->bpp != 8) return;
    /* the DIB colour table defines the colours */
    memcpy(screen_pal, g->pal, sizeof screen_pal);
    if (g->topdown) video_present(MEM + g->bits, g->w, g->h, g->pitch);
    else video_present(MEM + g->bits + (g->h - 1) * g->pitch, g->w, g->h, -g->pitch);
}
SHIM(GDI32, BitBlt, 9) {
    if (ARG(0) == H_DC + 1) blit_to_screen(ARG(5));
    return 1;
}
SHIM(GDI32, StretchBlt, 11) {
    if (ARG(0) == H_DC + 1) blit_to_screen(ARG(5));
    return 1;
}
SHIM(GDI32, PatBlt, 6) { return 1; }
