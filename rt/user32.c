#include "win32.h"

uint8_t vk_state[256];
int app_quit_requested;
uint32_t main_hwnd;

/* ---------- resources ---------- */
static uint32_t img_base_ = 0x400000;
static int res_lookup_dir(uint32_t rbase, uint32_t dir, uint32_t id, uint32_t *out) {
    uint16_t nnamed = rd16(dir + 12), nid = rd16(dir + 14);
    uint32_t e = dir + 16;
    for (int k = 0; k < nnamed + nid; k++, e += 8) {
        uint32_t name = rd32(e), off = rd32(e + 4);
        if (!(name & 0x80000000u) && (name == id || id == 0xffffffffu)) { *out = off; return 1; }
    }
    (void)rbase;
    return 0;
}
int res_find(uint32_t type, uint32_t id, uint32_t *addr, uint32_t *size) {
    uint32_t pe = img_base_ + rd32(img_base_ + 0x3c);
    uint32_t rva = rd32(pe + 24 + 96 + 2 * 8);
    if (!rva) return 0;
    uint32_t rb = img_base_ + rva, off;
    if (!res_lookup_dir(rb, rb, type, &off) || !(off & 0x80000000u)) return 0;
    if (!res_lookup_dir(rb, rb + (off & 0x7fffffff), id, &off) || !(off & 0x80000000u)) return 0;
    if (!res_lookup_dir(rb, rb + (off & 0x7fffffff), 0xffffffffu, &off)) return 0;
    uint32_t de = rb + (off & 0x7fffffff);
    *addr = img_base_ + rd32(de);
    *size = rd32(de + 4);
    return 1;
}

/* ---------- classes & windows ---------- */
typedef struct { char name[64]; uint32_t proc; } wclass_t;
static wclass_t classes[16]; static int nclasses;
typedef struct {
    int used; uint32_t proc; uint32_t style, exstyle; int x, y, w, h;
    uint32_t userdata, menu, parent; int dialog; uint32_t dlgresult; int ended;
    uint32_t wndextra[8];
} wnd_t;
#define MAXWND 32
static wnd_t wnds[MAXWND];
static wnd_t *W(uint32_t h) {
    if (h < H_WINDOW || h >= H_WINDOW + MAXWND * 4) return NULL;
    wnd_t *w = &wnds[(h - H_WINDOW) / 4];
    return w->used ? w : NULL;
}

SHIM(USER32, RegisterClassA, 1) {
    uint32_t wc = ARG(0);
    wclass_t *c = &classes[nclasses++];
    c->proc = rd32(wc + 4);
    uint32_t nm = rd32(wc + 36);
    if (nm >= 0x10000) strncpy(c->name, gstr(nm), 63); else snprintf(c->name, 64, "#%u", nm);
    rt_log("RegisterClassA(%s) proc=%08x\n", c->name, c->proc);
    return 0xc000 + nclasses;
}

/* message queue */
typedef struct { uint32_t hwnd, msg, wp, lp, time; } qmsg_t;
static qmsg_t q[256]; static int qh, qt;
void win_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    int n = (qt + 1) & 255;
    if (n == qh) return;
    q[qt].hwnd = hwnd; q[qt].msg = msg; q[qt].wp = wp; q[qt].lp = lp; q[qt].time = host_ticks_ms();
    qt = n;
}
uint32_t win_send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    wnd_t *w = W(hwnd);
    if (!w || !w->proc) return 0;
    uint32_t a[4] = {hwnd, msg, wp, lp};
    return guest_call(w->proc, 4, a);
}

SHIM(USER32, CreateWindowExA, 12) {
    uint32_t ex = ARG(0), cls = ARG(1), style = ARG(3);
    int x = ARG(4), y = ARG(5), w = ARG(6), h = ARG(7);
    uint32_t parent = ARG(8), menu = ARG(9), param = ARG(11);
    char cname[64];
    if (cls >= 0x10000) strncpy(cname, gstr(cls), 63); else snprintf(cname, 64, "#%u", cls);
    cname[63] = 0;
    uint32_t proc = 0;
    for (int k = 0; k < nclasses; k++) if (!strcasecmp(classes[k].name, cname)) proc = classes[k].proc;
    int idx = -1;
    for (int k = 0; k < MAXWND; k++) if (!wnds[k].used) { idx = k; break; }
    if (idx < 0) return 0;
    wnd_t *wn = &wnds[idx];
    memset(wn, 0, sizeof *wn);
    wn->used = 1; wn->proc = proc; wn->style = style; wn->exstyle = ex; wn->x = x; wn->y = y; wn->w = w; wn->h = h;
    wn->menu = menu; wn->parent = parent;
    uint32_t hwnd = H_WINDOW + 4 * idx;
    rt_log("CreateWindowExA(%s, '%s') -> %08x proc=%08x\n", cname, ARG(2) ? gstr(ARG(2)) : "", hwnd, proc);
    if (!main_hwnd && proc) main_hwnd = hwnd;
    if (proc) {
        uint32_t cs = guest_alloc(48);
        wr32(cs, param); wr32(cs + 4, H_MODULE); wr32(cs + 8, menu); wr32(cs + 12, parent);
        wr32(cs + 16, h); wr32(cs + 20, w); wr32(cs + 24, y); wr32(cs + 28, x); wr32(cs + 32, style);
        wr32(cs + 36, ARG(2)); wr32(cs + 40, cls); wr32(cs + 44, ex);
        uint32_t keep[8]; memcpy(keep, &R, sizeof keep);
        win_send(hwnd, WM_NCCREATE, 0, cs);
        uint32_t r = win_send(hwnd, WM_CREATE, 0, cs);
        guest_free(cs);
        if (r == 0xffffffffu) { wn->used = 0; return 0; }
        if (!parent) {
            win_send(hwnd, WM_SIZE, 0, (w & 0xffff) | (h << 16));
            win_send(hwnd, WM_MOVE, 0, 0);
        }
        if (style & 0x10000000u) {   /* WS_VISIBLE */
            win_send(hwnd, WM_SHOWWINDOW, 1, 0);
            win_send(hwnd, WM_ACTIVATEAPP, 1, 0);
            win_send(hwnd, WM_ACTIVATE, 1, 0);
            win_send(hwnd, WM_SETFOCUS, 0, 0);
        }
    }
    return hwnd;
}

SHIM(USER32, DefWindowProcA, 4) {
    uint32_t hwnd = ARG(0), msg = ARG(1);
    switch (msg) {
    case WM_NCCREATE: return 1;
    case WM_CLOSE: win_send(hwnd, WM_DESTROY, 0, 0); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR: return 1;
    }
    return 0;
}
SHIM(USER32, DestroyWindow, 1) {
    uint32_t h = ARG(0);
    wnd_t *w = W(h);
    if (w) { win_send(h, WM_DESTROY, 0, 0); w->used = 0; }
    return 1;
}
SHIM(USER32, ShowWindow, 2) {
    uint32_t h = ARG(0);
    if (W(h)) {
        win_send(h, WM_SHOWWINDOW, 1, 0);
        win_send(h, WM_ACTIVATEAPP, 1, 0);
        win_send(h, WM_ACTIVATE, 1, 0);
        win_send(h, WM_SETFOCUS, 0, 0);
    }
    return 0;
}
SHIM(USER32, UpdateWindow, 1) { return 1; }
SHIM(USER32, SetWindowPos, 7) { wnd_t *w = W(ARG(0)); if (w && !(ARG(6) & 2)) { w->x = ARG(2); w->y = ARG(3); } if (w && !(ARG(6) & 1)) { w->w = ARG(4); w->h = ARG(5); } return 1; }
SHIM(USER32, GetWindowLongA, 2) {
    wnd_t *w = W(ARG(0)); int32_t i = ARG(1);
    if (!w) return 0;
    if (i == -21) return w->userdata;
    if (i == -16) return w->style;
    if (i == -20) return w->exstyle;
    if (i == -4) return w->proc;
    if (i >= 0 && i < 32) return w->wndextra[i / 4];
    if (i == -6) return H_MODULE;
    return 0;
}
SHIM(USER32, SetWindowLongA, 3) {
    wnd_t *w = W(ARG(0)); int32_t i = ARG(1); uint32_t v = ARG(2), old = 0;
    if (!w) return 0;
    if (i == -21) { old = w->userdata; w->userdata = v; }
    else if (i == -16) { old = w->style; w->style = v; }
    else if (i == -20) { old = w->exstyle; w->exstyle = v; }
    else if (i == -4) { old = w->proc; w->proc = v; }
    else if (i >= 0 && i < 32) { old = w->wndextra[i / 4]; w->wndextra[i / 4] = v; }
    return old;
}
static void set_rect(uint32_t r, int l, int t, int rr, int b) { wr32(r, l); wr32(r + 4, t); wr32(r + 8, rr); wr32(r + 12, b); }
SHIM(USER32, GetClientRect, 2) {
    wnd_t *w = W(ARG(0));
    set_rect(ARG(1), 0, 0, screen_w ? screen_w : (w ? w->w : 640), screen_h ? screen_h : (w ? w->h : 480));
    return 1;
}
SHIM(USER32, GetWindowRect, 2) {
    wnd_t *w = W(ARG(0));
    int ww = w ? w->w : 640, hh = w ? w->h : 480;
    if (ww <= 0 || ww > 4096) ww = 640;
    if (hh <= 0 || hh > 4096) hh = 480;
    set_rect(ARG(1), w ? w->x : 0, w ? w->y : 0, (w ? w->x : 0) + ww, (w ? w->y : 0) + hh);
    return 1;
}
SHIM(USER32, AdjustWindowRect, 3) { return 1; }
SHIM(USER32, AdjustWindowRectEx, 4) { return 1; }
SHIM(USER32, SetRect, 5) { set_rect(ARG(0), ARG(1), ARG(2), ARG(3), ARG(4)); return 1; }
SHIM(USER32, GetSystemMetrics, 1) {
    switch (ARG(0)) {
    case 0: return 640; case 1: return 480;   /* SM_CXSCREEN / SM_CYSCREEN */
    case 4: return 19;                        /* caption */
    case 5: case 6: return 1;
    case 7: case 8: return 3;
    case 15: return 19;                       /* menu */
    case 16: return 640; case 17: return 460;
    case 32: case 33: return 4;
    }
    return 0;
}
SHIM(USER32, GetActiveWindow, 0) { return main_hwnd; }
SHIM(USER32, SetActiveWindow, 1) { return main_hwnd; }
SHIM(USER32, SetForegroundWindow, 1) { return 1; }
SHIM(USER32, SetFocus, 1) { return main_hwnd; }
SHIM(USER32, IsIconic, 1) { return 0; }
SHIM(USER32, EnableWindow, 2) { return 0; }
SHIM(USER32, InvalidateRect, 3) {
    uint32_t h = ARG(0) ? ARG(0) : main_hwnd;
    /* avoid flooding the queue with paints */
    for (int k = qh; k != qt; k = (k + 1) & 255) if (q[k].msg == WM_PAINT && q[k].hwnd == h) return 1;
    win_post(h, WM_PAINT, 0, 0);
    return 1;
}
SHIM(USER32, BeginPaint, 2) { uint32_t ps = ARG(1); memset(MEM + ps, 0, 64); wr32(ps, H_DC + 1); set_rect(ps + 8, 0, 0, 640, 480); return H_DC + 1; }
SHIM(USER32, EndPaint, 2) { return 1; }
SHIM(USER32, GetDC, 1) { return H_DC + 1; }
SHIM(USER32, ReleaseDC, 2) { return 1; }

/* ---------- cursor / misc ---------- */
SHIM(USER32, LoadIconA, 2) { return H_ICON + 1; }
SHIM(USER32, LoadCursorA, 2) { return H_ICON + 2; }
SHIM(USER32, ShowCursor, 1) { static int c; c += ARG(0) ? 1 : -1; return c; }
SHIM(USER32, ClipCursor, 1) { return 1; }
SHIM(USER32, GetCursorPos, 1) { wr32(ARG(0), 320); wr32(ARG(0) + 4, 240); return 1; }
SHIM(USER32, SetCursorPos, 2) { return 1; }
SHIM(USER32, MessageBeep, 1) { return 1; }
SHIM(USER32, MessageBoxA, 4) {
    rt_log("MessageBoxA('%s', '%s')\n", ARG(2) ? gstr(ARG(2)) : "", ARG(1) ? gstr(ARG(1)) : "");
    uint32_t t = ARG(3) & 0xf;
    if (t == 4 || t == 3) return 6; /* IDYES */
    return 1;                        /* IDOK */
}
SHIM(USER32, WinHelpA, 4) { return 1; }
SHIM(USER32, SetTimer, 4) {
    extern int user_set_timer(uint32_t, uint32_t, uint32_t, uint32_t);
    return user_set_timer(ARG(0), ARG(1), ARG(2), ARG(3));
}
SHIM(USER32, KillTimer, 2) {
    extern void user_kill_timer(uint32_t, uint32_t);
    user_kill_timer(ARG(0), ARG(1)); return 1;
}
SHIM(USER32, SetScrollPos, 4) { return 0; }
SHIM(USER32, SetScrollRange, 5) { return 1; }

/* ---------- menus (invisible) ---------- */
SHIM(USER32, LoadMenuA, 2) { return H_MENU + (ARG(1) & 0xffff); }
SHIM(USER32, SetMenu, 2) { wnd_t *w = W(ARG(0)); if (w) w->menu = ARG(1); return 1; }
SHIM(USER32, GetMenu, 1) { wnd_t *w = W(ARG(0)); return w ? w->menu : 0; }
SHIM(USER32, DestroyMenu, 1) { return 1; }
SHIM(USER32, EnableMenuItem, 3) { return 0; }

/* ---------- keyboard ---------- */
SHIM(USER32, GetAsyncKeyState, 1) {
    uint32_t vk = ARG(0) & 0xff;
    winmm_service();
    return vk_state[vk] ? 0x8000 : 0;
}
SHIM(USER32, MapVirtualKeyA, 2) {
    static const uint8_t sc[256] = {
        [0x1b] = 1, [0x0d] = 0x1c, [0x20] = 0x39, [0x11] = 0x1d, [0x10] = 0x2a, [0x12] = 0x38,
        [0x25] = 0x4b, [0x26] = 0x48, [0x27] = 0x4d, [0x28] = 0x50,
        [0x70] = 0x3b, [0x71] = 0x3c, [0x72] = 0x3d, [0x73] = 0x3e, [0x74] = 0x3f, [0x75] = 0x40,
        ['A'] = 0x1e, ['B'] = 0x30, ['C'] = 0x2e, ['D'] = 0x20, ['E'] = 0x12, ['F'] = 0x21, ['G'] = 0x22,
        ['H'] = 0x23, ['I'] = 0x17, ['J'] = 0x24, ['K'] = 0x25, ['L'] = 0x26, ['M'] = 0x32, ['N'] = 0x31,
        ['O'] = 0x18, ['P'] = 0x19, ['Q'] = 0x10, ['R'] = 0x13, ['S'] = 0x1f, ['T'] = 0x14, ['U'] = 0x16,
        ['V'] = 0x2f, ['W'] = 0x11, ['X'] = 0x2d, ['Y'] = 0x15, ['Z'] = 0x2c,
    };
    uint32_t c = ARG(0), t = ARG(1);
    if (t == 0) return sc[c & 0xff];
    if (t == 1) { for (int k = 0; k < 256; k++) if (sc[k] == c) return k; return 0; }
    return 0;
}
SHIM(USER32, GetKeyNameTextA, 3) {
    uint32_t lp = ARG(0), buf = ARG(1), n = ARG(2);
    char tmp[32]; snprintf(tmp, sizeof tmp, "Key %u", (lp >> 16) & 0xff);
    strncpy((char *)MEM + buf, tmp, n);
    return strlen(tmp);
}

/* ---------- accelerators ---------- */
SHIM(USER32, LoadAcceleratorsA, 2) { return 0x00090000u + (ARG(1) & 0xffff); }
SHIM(USER32, TranslateAcceleratorA, 3) {
    uint32_t hwnd = ARG(0), acc = ARG(1), msg = ARG(2);
    uint32_t m = rd32(msg + 4), vk = rd32(msg + 8);
    if (m != WM_KEYDOWN && m != WM_SYSKEYDOWN) return 0;
    uint32_t a, sz;
    if (!res_find(9, acc & 0xffff, &a, &sz)) return 0;
    for (uint32_t k = 0; k + 8 <= sz; k += 8) {
        uint16_t fv = rd16(a + k), key = rd16(a + k + 2), cmd = rd16(a + k + 4);
        if ((fv & 1) && key == vk) {
            int sh = !!vk_state[0x10], ct = !!vk_state[0x11], al = !!vk_state[0x12];
            if (!!(fv & 4) == sh && !!(fv & 8) == ct && !!(fv & 0x10) == al) {
                rt_log("accelerator -> WM_COMMAND %u\n", cmd);
                win_send(hwnd, WM_COMMAND, cmd | 0x10000, 0);
                return 1;
            }
        }
        if (fv & 0x80) break;
    }
    return 0;
}

/* ---------- message loop ---------- */
static int quit_posted; static uint32_t quit_code;
SHIM(USER32, PostQuitMessage, 1) { quit_posted = 1; quit_code = ARG(0); return 0; }
SHIM(USER32, PostMessageA, 4) { win_post(ARG(0), ARG(1), ARG(2), ARG(3)); return 1; }
SHIM(USER32, SendMessageA, 4) {
    uint32_t h = ARG(0);
    if (W(h)) return win_send(h, ARG(1), ARG(2), ARG(3));
    return 0;
}
static void write_msg(uint32_t p, qmsg_t *m) {
    wr32(p, m->hwnd); wr32(p + 4, m->msg); wr32(p + 8, m->wp); wr32(p + 12, m->lp); wr32(p + 16, m->time);
    wr32(p + 20, 320); wr32(p + 24, 240);
}
static int take_msg(uint32_t p, int remove) {
    if (qh != qt) {
        write_msg(p, &q[qh]);
        if (remove) qh = (qh + 1) & 255;
        return 1;
    }
    if (quit_posted) {
        qmsg_t m = {0, WM_QUIT, quit_code, 0, host_ticks_ms()};
        write_msg(p, &m);
        if (remove) quit_posted = 0;
        return 1;
    }
    return 0;
}
SHIM(USER32, PeekMessageA, 5) {
    uint32_t p = ARG(0), rem = ARG(4) & 1;
    win_pump_host();
    return take_msg(p, rem);
}
SHIM(USER32, GetMessageA, 4) {
    uint32_t p = ARG(0);
    for (;;) {
        win_pump_host();
        if (take_msg(p, 1)) return rd32(p + 4) == WM_QUIT ? 0 : 1;
        host_sleep_ms(1);
    }
}
SHIM(USER32, TranslateMessage, 1) { return 1; }
SHIM(USER32, DispatchMessageA, 1) {
    uint32_t p = ARG(0);
    uint32_t h = rd32(p), m = rd32(p + 4), wp = rd32(p + 8), lp = rd32(p + 12);
    if (m == WM_TIMER && lp) { uint32_t a[4] = {h, m, wp, host_ticks_ms()}; guest_call(lp, 4, a); return 0; }
    if (!h) return 0;
    return win_send(h, m, wp, lp);
}
SHIM(USER32, IsDialogMessageA, 2) { return 0; }

/* ---------- dialogs ---------- */
/* Dialog boxes are not displayed on the target.  Modal dialogs run their dialog
   procedure through WM_INITDIALOG and are then closed with IDCANCEL so the game keeps
   its current settings.  Control states set by the dialog proc are remembered so
   reads (IsDlgButtonChecked etc.) behave sensibly. */
typedef struct { uint32_t dlg, id; uint32_t check; char text[128]; } ctl_t;
static ctl_t ctls[256]; static int nctls;
static ctl_t *ctl(uint32_t dlg, uint32_t id) {
    for (int k = 0; k < nctls; k++) if (ctls[k].dlg == dlg && ctls[k].id == id) return &ctls[k];
    if (nctls >= 256) return &ctls[0];
    ctl_t *c = &ctls[nctls++]; memset(c, 0, sizeof *c); c->dlg = dlg; c->id = id; return c;
}
static uint32_t make_dialog(uint32_t proc, uint32_t parent) {
    for (int k = 0; k < MAXWND; k++) if (!wnds[k].used) {
        wnd_t *w = &wnds[k]; memset(w, 0, sizeof *w);
        w->used = 1; w->proc = proc; w->dialog = 1; w->parent = parent;
        return H_WINDOW + 4 * k;
    }
    return 0;
}
SHIM(USER32, DialogBoxParamA, 5) {
    uint32_t tmpl = ARG(1), parent = ARG(2), proc = ARG(3), param = ARG(4);
    rt_log("DialogBoxParamA(template %u) -> auto-cancel\n", tmpl);
    uint32_t h = make_dialog(proc, parent);
    wnd_t *w = W(h);
    if (!w) return 0xffffffffu;
    win_send(h, WM_INITDIALOG, 0, param);
    if (!w->ended) win_send(h, WM_COMMAND, 2, 0);   /* IDCANCEL */
    uint32_t r = w->ended ? w->dlgresult : 2;
    w->used = 0;
    return r;
}
SHIM(USER32, CreateDialogParamA, 5) {
    uint32_t tmpl = ARG(1), parent = ARG(2), proc = ARG(3), param = ARG(4);
    rt_log("CreateDialogParamA(template %u)\n", tmpl);
    uint32_t h = make_dialog(proc, parent);
    if (h) win_send(h, WM_INITDIALOG, 0, param);
    return h;
}
SHIM(USER32, EndDialog, 2) { wnd_t *w = W(ARG(0)); if (w) { w->ended = 1; w->dlgresult = ARG(1); } return 1; }
SHIM(USER32, GetDlgItem, 2) { return H_DLGITEM + (ARG(1) & 0xffff); }
SHIM(USER32, CheckDlgButton, 3) { ctl(ARG(0), ARG(1))->check = ARG(2); return 1; }
SHIM(USER32, CheckRadioButton, 4) {
    for (uint32_t id = ARG(1); id <= ARG(2); id++) ctl(ARG(0), id)->check = (id == ARG(3));
    return 1;
}
SHIM(USER32, IsDlgButtonChecked, 2) { return ctl(ARG(0), ARG(1))->check; }
SHIM(USER32, SetDlgItemTextA, 3) { strncpy(ctl(ARG(0), ARG(1))->text, ARG(2) ? gstr(ARG(2)) : "", 127); return 1; }
SHIM(USER32, GetDlgItemTextA, 4) {
    ctl_t *c = ctl(ARG(0), ARG(1));
    uint32_t n = ARG(3);
    if (!n) return 0;
    strncpy((char *)MEM + ARG(2), c->text, n - 1); wr8(ARG(2) + n - 1, 0);
    return strlen(gstr(ARG(2)));
}
SHIM(USER32, SendDlgItemMessageA, 5) { return 0; }
SHIM(USER32, DlgDirListA, 5) { return 1; }

/* ---------- timers ---------- */
typedef struct { uint32_t hwnd, id, ms, proc, next; int used; } utimer_t;
static utimer_t utim[16];
int user_set_timer(uint32_t hwnd, uint32_t id, uint32_t ms, uint32_t proc) {
    for (int k = 0; k < 16; k++) if (utim[k].used && utim[k].hwnd == hwnd && utim[k].id == id) { utim[k].ms = ms; utim[k].proc = proc; return id; }
    for (int k = 0; k < 16; k++) if (!utim[k].used) {
        utim[k] = (utimer_t){hwnd, id ? id : (uint32_t)(k + 1), ms ? ms : 1, proc, host_ticks_ms() + ms, 1};
        return utim[k].id;
    }
    return 0;
}
void user_kill_timer(uint32_t hwnd, uint32_t id) {
    for (int k = 0; k < 16; k++) if (utim[k].used && utim[k].hwnd == hwnd && utim[k].id == id) utim[k].used = 0;
}
static void user_timers(void) {
    uint32_t now = host_ticks_ms();
    for (int k = 0; k < 16; k++) if (utim[k].used && (int32_t)(now - utim[k].next) >= 0) {
        utim[k].next = now + utim[k].ms;
        int dup = 0;
        for (int j = qh; j != qt; j = (j + 1) & 255) if (q[j].msg == WM_TIMER && q[j].wp == utim[k].id) dup = 1;
        if (!dup) win_post(utim[k].hwnd, WM_TIMER, utim[k].id, utim[k].proc);
    }
}

/* ---------- host input -> keyboard ---------- */
typedef struct { uint32_t bit; uint8_t vk; } bmap_t;
static const bmap_t bmap[] = {
    {HB_UP, 0x26}, {HB_DOWN, 0x28}, {HB_LEFT, 0x25}, {HB_RIGHT, 0x27},
    {HB_FIRE, 0x11}, {HB_BOMB, 0x20}, {HB_START, 0x71}, {HB_SELECT, 0x1b},
    {HB_TRIANGLE, 0x0d}, {HB_SQUARE, 0x72},
};
static uint32_t prev_buttons;
int kbd_active, kbd_index; char kbd_char = 'A';
extern void dinput_key_event(uint8_t sc, int down);
static void type_char(int c) {
    static const uint8_t lsc[26] = {0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
    static const uint8_t dsc[10] = {0x0b,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a};
    uint32_t vk = c, sc = 0;
    if (c >= 'A' && c <= 'Z') sc = lsc[c - 'A'];
    else if (c >= '0' && c <= '9') sc = dsc[c - '0'];
    else if (c == 8) { vk = 8; sc = 0x0e; }
    else if (c == 13) { vk = 13; sc = 0x1c; }
    else if (c == ' ') sc = 0x39;
    else if (c == '.') { vk = 0xbe; sc = 0x34; }
    else if (c == '-') { vk = 0xbd; sc = 0x0c; }
    else if (c == '!') { vk = '1'; sc = 0x02; }
    if (main_hwnd) {
        win_post(main_hwnd, WM_KEYDOWN, vk, 1 | (sc << 16));
        win_post(main_hwnd, WM_CHAR, c, 1 | (sc << 16));
        win_post(main_hwnd, WM_KEYUP, vk, 1 | (sc << 16) | 0xc0000000u);
    }
    if (sc) { dinput_key_event(sc, 1); dinput_key_event(sc, 0); }
}
extern void dinput_update(const host_input_t *in);
uint32_t g_pad_buttons;
host_input_t g_last_input;

void win_pump_host(void) {
    host_input_t in;
    memset(&in, 0, sizeof in);
    host_poll(&in);
    static uint32_t quit_time;
    if (in.quit && !app_quit_requested) {
        app_quit_requested = 1;
        quit_time = host_ticks_ms();
        rt_log("quit requested: closing main window\n");
        if (main_hwnd) win_post(main_hwnd, WM_CLOSE, 0, 0); else { reg_save(); exit(0); }
    }
    if (app_quit_requested && host_ticks_ms() - quit_time > 4000) { rt_log("quit timeout\n"); reg_save(); exit(0); }
    /* analog stick acts as d-pad */
    if (in.ay < -64) in.buttons |= HB_UP;
    if (in.ay > 64) in.buttons |= HB_DOWN;
    if (in.ax < -64) in.buttons |= HB_LEFT;
    if (in.ax > 64) in.buttons |= HB_RIGHT;
    /* L held: on-screen letter keyboard (for high-score names).  Up/Down pick a letter,
       Cross/Right types it, Left = backspace; R alone = Enter */
    {
        static const char cs[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-!";
        static uint32_t kprev;
        uint32_t pr = in.buttons & ~kprev;
        kprev = in.buttons;
        kbd_active = (in.buttons & HB_L) != 0;
        if (kbd_active) {
            int n = (int)strlen(cs);
            if (pr & HB_UP) kbd_index = (kbd_index + 1) % n;
            if (pr & HB_DOWN) kbd_index = (kbd_index + n - 1) % n;
            if (pr & (HB_FIRE | HB_RIGHT)) type_char(cs[kbd_index]);
            if (pr & HB_LEFT) type_char(8);
            in.buttons = 0;
        } else if (pr & HB_R) type_char(13);
        kbd_char = cs[kbd_index];
    }
    g_pad_buttons = in.buttons;
    uint32_t ch = in.buttons ^ prev_buttons;
    for (unsigned k = 0; k < sizeof bmap / sizeof bmap[0]; k++) {
        if (!(ch & bmap[k].bit)) continue;
        int down = (in.buttons & bmap[k].bit) != 0;
        uint8_t vk = bmap[k].vk;
        vk_state[vk] = down;
        if (vk == 0x11) vk_state[0xa2] = down;
        uint32_t sc = 0;
        switch (vk) { case 0x26: sc = 0x148; break; case 0x28: sc = 0x150; break; case 0x25: sc = 0x14b; break; case 0x27: sc = 0x14d; break;
                      case 0x11: sc = 0x1d; break; case 0x20: sc = 0x39; break; case 0x71: sc = 0x3c; break; case 0x72: sc = 0x3d; break;
                      case 0x1b: sc = 0x01; break; case 0x0d: sc = 0x1c; break; }
        if (main_hwnd) {
            if (down) win_post(main_hwnd, WM_KEYDOWN, vk, 1 | (sc << 16));
            else win_post(main_hwnd, WM_KEYUP, vk, 1 | (sc << 16) | 0xc0000000u);
            if (down && (vk == 0x0d || vk == 0x1b || vk == 0x20)) win_post(main_hwnd, WM_CHAR, vk, 1 | (sc << 16));
        }
    }
    prev_buttons = in.buttons;
    g_last_input = in;
    dinput_update(&in);
    user_timers();
    winmm_service();
    dsound_service();
}
