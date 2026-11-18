/* PS Vita host: display, audio, input, launcher */
#include "host.h"
#include "games.h"
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/audioout.h>
#include <psp2/power.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <psp2/apputil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int _newlib_heap_size_user = 210 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize = 8 * 1024 * 1024;

#define DATA_ROOT "ux0:data/DemonStar"
extern const char *g_game_id;
extern const uint16_t font16x32[96][32];

static FILE *logf_;
static SceKernelLwMutexWork amtx;
static launch_opts opts = {2, -1, 0};
static int play_mode;   /* 0 one Vita, 1 ad-hoc host, 2 ad-hoc join, 3 Wi-Fi host, 4 Wi-Fi join */
static int net_session(int mode, int g, char *err, int errlen);
static char net_msg[128];
static uint32_t net_msg_until;
static void banner(uint32_t *d);
void message_screen(const char *title, const char *msg);
extern void rt_log(const char *fmt, ...);

/* ---------------- logging / misc ---------------- */
void host_vlog(const char *fmt, va_list ap) {
    if (!logf_) logf_ = fopen(DATA_ROOT "/log.txt", "w");
    if (logf_) { vfprintf(logf_, fmt, ap); fflush(logf_); }
}
uint64_t host_ticks_us(void) { return sceKernelGetProcessTimeWide(); }
uint32_t host_ticks_ms(void) { return (uint32_t)(sceKernelGetProcessTimeWide() / 1000); }
void host_sleep_ms(uint32_t ms) { sceKernelDelayThread(ms ? ms * 1000 : 100); }
const char *host_game_dir(void) { static char b[128]; snprintf(b, sizeof b, DATA_ROOT "/%s", g_game_id); return b; }
const char *host_save_dir(void) { static char b[128]; snprintf(b, sizeof b, DATA_ROOT "/save/%s", g_game_id); return b; }
const char *host_soundfont_path(void) { return "app0:GeneralUser-GS.sf2"; }
void *host_alloc_guest(uint32_t size) { return malloc(size); }

/* ---------------- display ---------------- */
static void text(uint32_t *d, int x, int y, const char *s, uint32_t c, int scale_half);
#define SW 960
#define SH 544
#define NFB 3
static uint32_t *fb[NFB];
static int fbi;
static int xtab[SW], ytab[SH], dst_x0, dst_w, last_sw, last_sh, last_aspect = -1;

static void display_init(void) {
    static int done;
    if (done) return;
    done = 1;
    uint32_t sz = (SW * SH * 4 + 0x3ffff) & ~0x3ffff;
    for (int k = 0; k < NFB; k++) {
        SceUID id = sceKernelAllocMemBlock("fb", SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW, sz, NULL);
        sceKernelGetMemBlockBase(id, (void **)&fb[k]);
        memset(fb[k], 0, SW * SH * 4);
    }
    SceDisplayFrameBuf f = {sizeof f, fb[0], SW, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, SW, SH};
    sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
}
static void flip(void) {
    SceDisplayFrameBuf f = {sizeof f, fb[fbi], SW, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, SW, SH};
    sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
    fbi = (fbi + 1) % NFB;
}
void host_video_init(int w, int h) { (void)w; (void)h; display_init(); }

void host_present(const uint8_t *pix, int w, int h, int pitch, const uint32_t *pal) {
    display_init();
    if (w != last_sw || h != last_sh || opts.aspect != last_aspect) {
        last_sw = w; last_sh = h; last_aspect = opts.aspect;
        /* 0 = 4:3, 1 = stretched, 2 = original (square pixels, as the PC window showed it: 320x400 -> 435x544) */
        dst_w = opts.aspect == 1 ? SW : opts.aspect == 2 ? (w * SH + h / 2) / h : (SH * 4 / 3);
        if (dst_w > SW) dst_w = SW;
        dst_x0 = (SW - dst_w) / 2;
        for (int x = 0; x < dst_w; x++) xtab[x] = x * w / dst_w;
        for (int y = 0; y < SH; y++) ytab[y] = y * h / SH;
        for (int k = 0; k < NFB; k++) memset(fb[k], 0, SW * SH * 4);
    }
    uint32_t p32[256];
    for (int k = 0; k < 256; k++) {
        uint32_t c = pal[k];
        p32[k] = 0xff000000u | ((c >> 16) & 0xff) | (c & 0xff00) | ((c & 0xff) << 16);
    }
    uint32_t *d = fb[fbi];
    int prev = -1;
    for (int y = 0; y < SH; y++) {
        uint32_t *row = d + y * SW + dst_x0;
        int sy = ytab[y];
        if (sy == prev) { memcpy(row, row - SW, dst_w * 4); continue; }
        prev = sy;
        const uint8_t *s = pix + (intptr_t)sy * pitch;
        for (int x = 0; x < dst_w; x++) row[x] = p32[s[xtab[x]]];
    }
    {
        extern int kbd_active; extern char kbd_char;
        if (kbd_active) {
            char s[4] = {'<', kbd_char, '>', 0};
            for (int j = 8; j < 52; j++) for (int i = 8; i < 72; i++) d[j * SW + i] = 0xff402010;
            text(d, 18, 14, s, 0xffffffff, 0);
        }
    }
    if (net_msg[0]) {
        if (net_msg_until && (int32_t)(host_ticks_ms() - net_msg_until) > 0) net_msg[0] = 0;
        else banner(d);
    }
    flip();
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
}

/* ---------------- audio ---------------- */
void host_audio_lock(void) { sceKernelLockLwMutex(&amtx, 1, NULL); }
void host_audio_unlock(void) { sceKernelUnlockLwMutex(&amtx, 1); }
static int audio_thread(SceSize args, void *argp) {
    (void)args; (void)argp;
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, 1024, HOST_AUDIO_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0) return 0;
    int vol[2] = {SCE_AUDIO_VOLUME_0DB, SCE_AUDIO_VOLUME_0DB};
    sceAudioOutSetVolume(port, SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH, vol);
    static int16_t buf[2][1024 * 2];
    int k = 0;
    for (;;) {
        audio_mix(buf[k], 1024);
        sceAudioOutOutput(port, buf[k]);
        k ^= 1;
    }
    return 0;
}
void host_audio_init(void) {
    static int started;
    if (started) return;
    started = 1;
    SceUID t = sceKernelCreateThread("audio", audio_thread, 0x10000100 - 10, 0x10000, 0, SCE_KERNEL_CPU_MASK_USER_1, NULL);
    sceKernelStartThread(t, 0, NULL);
}

/* ---------------- input ---------------- */
/* The Vita's own controls only (player 2 in two-Vita play is the other Vita). */
static uint32_t quit_hold;

static uint32_t map_buttons(uint32_t p) {
    uint32_t b = 0;
    if (p & SCE_CTRL_UP) b |= HB_UP;
    if (p & SCE_CTRL_DOWN) b |= HB_DOWN;
    if (p & SCE_CTRL_LEFT) b |= HB_LEFT;
    if (p & SCE_CTRL_RIGHT) b |= HB_RIGHT;
    if (p & SCE_CTRL_CROSS) b |= HB_FIRE;
    if (p & SCE_CTRL_SQUARE) b |= HB_FIRE;
    if (p & SCE_CTRL_CIRCLE) b |= HB_BOMB;
    if (p & SCE_CTRL_TRIANGLE) b |= HB_TRIANGLE;
    if (p & SCE_CTRL_START) b |= HB_START;
    if (p & SCE_CTRL_SELECT) b |= HB_SELECT;
    if (p & SCE_CTRL_LTRIGGER) b |= HB_L;
    if (p & SCE_CTRL_RTRIGGER) b |= HB_R;
    return b;
}

void host_bt_release(void) {}

void host_poll(host_input_t *in) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t p = pad.buttons;
    in->ax = (int)pad.lx - 128;
    in->ay = (int)pad.ly - 128;
    uint32_t b = map_buttons(p);
    /* L + R + START + SELECT held for 1 second: quit */
    if ((p & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START | SCE_CTRL_SELECT)) ==
        (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER | SCE_CTRL_START | SCE_CTRL_SELECT)) {
        if (!quit_hold) quit_hold = host_ticks_ms();
        else if (host_ticks_ms() - quit_hold > 1000) in->quit = 1;
        b &= ~(HB_START | HB_SELECT);
    } else quit_hold = 0;
    in->buttons = b;
}

/* ---------------- text drawing for launcher / errors ---------------- */
static void fill(uint32_t *d, int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h && j < SH; j++) for (int i = x; i < x + w && i < SW; i++) d[j * SW + i] = c;
}
static void text(uint32_t *d, int x, int y, const char *s, uint32_t c, int scale_half) {
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 127) ch = '?';
        const uint16_t *g = font16x32[ch - 32];
        if (scale_half) {
            for (int r = 0; r < 16; r++) for (int col = 0; col < 8; col++)
                if ((g[r * 2] | g[r * 2 + 1]) & (3u << (col * 2))) { int px = x + col, py = y + r; if (px < SW && py < SH) d[py * SW + px] = c; }
            x += 8;
        } else {
            for (int r = 0; r < 32; r++) for (int col = 0; col < 16; col++)
                if (g[r] & (1u << col)) { int px = x + col, py = y + r; if (px < SW && py < SH) d[py * SW + px] = c; }
            x += 15;
        }
    }
}

void host_fatal(const char *msg) {
    extern void rt_log(const char *fmt, ...);
    rt_log("FATAL: %s\n", msg);
    display_init();
    for (int k = 0; k < NFB; k++) {
        uint32_t *d = fb[k];
        fill(d, 0, 0, SW, SH, 0xff200000);
        text(d, 40, 60, "DemonStar - fatal error", 0xff4040ff, 0);
        text(d, 40, 120, msg, 0xffffffff, 0);
        text(d, 40, 200, "See " DATA_ROOT "/log.txt", 0xffc0c0c0, 0);
        text(d, 40, 260, "Press START to exit.", 0xffc0c0c0, 0);
    }
    flip();
    SceCtrlData pad;
    sceKernelDelayThread(500000);
    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START) break;
        sceKernelDelayThread(16000);
    }
    host_bt_release();
    sceKernelExitProcess(0);
    for (;;) {}
}

/* ---------------- launcher ---------------- */
static int file_exists(const char *p) { SceIoStat st; return sceIoGetstat(p, &st) >= 0; }
static int game_installed(int g) {
    char p[160];
    snprintf(p, sizeof p, DATA_ROOT "/%s/ds.exe", games[g].id);
    if (!file_exists(p)) { snprintf(p, sizeof p, DATA_ROOT "/%s/DS.EXE", games[g].id); if (!file_exists(p)) return 0; }
    return 1;
}
static void load_cfg(void) {
    FILE *f = fopen(DATA_ROOT "/save/launcher.cfg", "r");
    if (!f) return;
    int a, b, c, m = 0;
    if (fscanf(f, "%d %d %d %d", &a, &b, &c, &m) >= 3) {
        opts.aspect = (a >= 0 && a <= 2) ? a : 2; opts.ds1_difficulty = (b >= -1 && b <= 3) ? b : -1; opts.smooth = c;
        play_mode = (m >= 0 && m <= 4) ? m : 0;
    }
    fclose(f);
}
static void save_cfg(void) {
    sceIoMkdir(DATA_ROOT, 0777);
    sceIoMkdir(DATA_ROOT "/save", 0777);
    FILE *f = fopen(DATA_ROOT "/save/launcher.cfg", "w");
    if (!f) return;
    fprintf(f, "%d %d %d %d\n", opts.aspect, opts.ds1_difficulty, opts.smooth, play_mode);
    fclose(f);
}

/* items: 0/1 games, 2 play mode, 3 screen, 4 difficulty, 5 exit */
static int launcher(void) {
    static const char *diff[] = {"Keep game setting", "Easy", "Normal", "Hard", "Insane"};
    static const char *pm[] = {"One Vita", "Two Vitas, ad-hoc: HOST (player 1)",
                               "Two Vitas, ad-hoc: JOIN (player 2)", "Two Vitas, same Wi-Fi: HOST (player 1)",
                               "Two Vitas, same Wi-Fi: JOIN (player 2)"};
    int sel = 0, nitems = 6;
    uint32_t prev = ~0u;
    int inst[NGAMES];
    for (int g = 0; g < NGAMES; g++) inst[g] = game_installed(g);
    if (!inst[0] && inst[1]) sel = 1;
    for (;;) {
        int joining = play_mode == 2 || play_mode == 4;
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t p = pad.buttons, pr = p & ~prev;
        prev = p;
        if (pr & SCE_CTRL_UP) { sel = (sel + nitems - 1) % nitems; if (joining && sel == 1) sel = 0; }
        if (pr & SCE_CTRL_DOWN) { sel = (sel + 1) % nitems; if (joining && sel == 1) sel = 2; }
        int lr = (pr & SCE_CTRL_RIGHT) ? 1 : (pr & SCE_CTRL_LEFT) ? -1 : 0;
        if (sel == 2 && (lr || (pr & SCE_CTRL_CROSS))) play_mode = (play_mode + (lr < 0 ? 4 : 1)) % 5;
        if (sel == 3 && (lr || (pr & SCE_CTRL_CROSS))) opts.aspect = (opts.aspect + (lr < 0 ? 2 : 1)) % 3;
        if (sel == 4 && lr) opts.ds1_difficulty = (opts.ds1_difficulty + 1 + lr + 5) % 5 - 1;
        if (sel == 4 && (pr & SCE_CTRL_CROSS)) opts.ds1_difficulty = (opts.ds1_difficulty + 2) % 5 - 1;
        joining = play_mode == 2 || play_mode == 4;
        if (joining && sel == 1) sel = 0;
        if (pr & SCE_CTRL_CROSS) {
            if (joining && sel == 0) { save_cfg(); return 0; }
            if (!joining && sel < NGAMES && inst[sel]) { save_cfg(); return sel; }
            if (sel == 5) { save_cfg(); return -1; }
        }
        uint32_t *d = fb[fbi];
        fill(d, 0, 0, SW, SH, 0xff100404);
        fill(d, 0, 0, SW, 70, 0xff201010);
        text(d, 40, 20, "DEMONSTAR  -  PS Vita", 0xff3050ff, 0);
        text(d, 600, 28, "static recompilation", 0xff8080a0, 1);
        char buf[6][100];
        for (int g = 0; g < NGAMES; g++)
            snprintf(buf[g], sizeof buf[g], "%s%s", games[g].title, inst[g] ? "" : "   (data missing)");
        if (joining) {
            snprintf(buf[0], sizeof buf[0], "JOIN player 1's game");
            snprintf(buf[1], sizeof buf[1], "   (player 1 picks DemonStar or Secret Missions)");
        }
        snprintf(buf[2], sizeof buf[2], "Play: %s", pm[play_mode]);
        static const char *am[] = {"4:3", "Full screen (stretched)", "Original (PC window, 1:1 pixels)"};
        snprintf(buf[3], sizeof buf[3], "Screen: %s", am[opts.aspect]);
        snprintf(buf[4], sizeof buf[4], "DemonStar difficulty: %s%s", diff[opts.ds1_difficulty + 1], joining ? "  (player 1's is used)" : "");
        snprintf(buf[5], sizeof buf[5], "Exit");
        for (int k = 0; k < nitems; k++) {
            int y = 96 + k * 40 + (k >= 2 ? 12 : 0);
            if (k == sel) fill(d, 30, y - 4, 900, 38, 0xff402020);
            uint32_t c = joining ? (k == 1 ? 0xff909090 : 0xffffffff) : (k < NGAMES && !inst[k]) ? 0xff707070 : 0xffffffff;
            if (k == 2 && play_mode) c = 0xff60e0ff;
            text(d, 50, y, buf[k], c, 0);
        }
        int y = 360;
        text(d, 50, y, "Controls:  D-pad / left stick = move   Cross / Square = fire   Circle = mega bomb (also selects in menus)", 0xffb0b0b0, 1); y += 20;
        text(d, 50, y, "           START = start player 1 (F2)   SELECT = Esc (menu in Secret Missions)", 0xffb0b0b0, 1); y += 20;
        text(d, 50, y, "           Triangle = Enter / pause   L + Up/Down/Cross = type letters   R = Enter   L+R+START+SELECT = quit", 0xffb0b0b0, 1); y += 30;
        if (!play_mode) {
            text(d, 50, y, "One Vita: single player (for 2 players, use two Vitas)", 0xff8080a0, 1);
        } else if (play_mode <= 2) {
            text(d, 50, y, "Ad-hoc: Vitas side by side, Wi-Fi on (no flight mode). Player 2 presses START in the game to join in.", 0xff60e0ff, 1);
        } else {
            text(d, 50, y, "Same Wi-Fi: both Vitas connected to one router (Settings > Network). Player 2 presses START to join in.", 0xff60e0ff, 1);
        }
        y += 26;
        text(d, 50, y, "Game data: " DATA_ROOT "/ds1/  and  " DATA_ROOT "/sm1/", 0xff80a0a0, 1); y += 20;
        text(d, 50, y, "(copy your own ds.exe and *.glb files there)", 0xff80a0a0, 1);
        flip();
        sceDisplayWaitVblankStart();
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    sceKernelCreateLwMutex(&amtx, "audio", 2, 0, NULL);
    atexit(host_bt_release);
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    sceIoMkdir(DATA_ROOT, 0777);
    sceIoMkdir(DATA_ROOT "/save", 0777);
    sceIoMkdir(DATA_ROOT "/save/ds1", 0777);
    sceIoMkdir(DATA_ROOT "/save/sm1", 0777);
    display_init();
    load_cfg();
    int g;
    for (;;) {
        g = launcher();
        if (g < 0) { sceKernelExitProcess(0); return 0; }
        if (!play_mode) break;
        char err[200] = "";
        g = net_session(play_mode, g, err, sizeof err);
        if (g >= 0) break;
        if (strcmp(err, "Cancelled.")) message_screen("DEMONSTAR  -  two-Vita play", err);
    }
    for (int k = 0; k < NFB; k++) memset(fb[k], 0, SW * SH * 4);
    game_main(g, &opts);
    host_bt_release();
    sceKernelExitProcess(0);
    return 0;
}

/* process exit from the game (ExitProcess) */
void host_exit(void) { host_bt_release(); sceKernelExitProcess(0); }

/* ======================================================================
 * Two-Vita play: transports for rt/netplay.c
 *   ad-hoc     - the system's ad-hoc dialog (group "DEMONSTR"), PDP datagrams
 *   same Wi-Fi - UDP broadcast / unicast on port 7777 through an access point
 * ====================================================================== */
#include <psp2/sysmodule.h>
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/pspnet_adhoc.h>
#include <psp2/pspnet_adhocctl.h>
#include <psp2/netcheck_dialog.h>
#include <psp2/common_dialog.h>
#include <psp2/gxm.h>
#include "netplay.h"

#define NET_PORT 7777
#define ADHOC_PORT 0x4453
enum { NET_OFF, NET_ADHOC, NET_INET };
static int net_kind;
static int pdp = -1, sock = -1, have_peer;
static SceNetEtherAddr my_mac, peer_mac;
static uint32_t my_ip, my_bcast, peer_ip;
static uint16_t peer_port;
static int in_lobby, lobby_hosting;
static uint32_t lobby_prev, lobby_drawn;

static void banner(uint32_t *d) {
    fill(d, 0, SH - 52, SW, 52, 0xe0201008);
    text(d, 40, SH - 44, net_msg, 0xff40e0ff, 0);
}

static void draw_lobby(void) {
    uint32_t *d = fb[fbi];
    fill(d, 0, 0, SW, SH, 0xff100404);
    fill(d, 0, 0, SW, 70, 0xff201010);
    text(d, 40, 20, "DEMONSTAR  -  two-Vita play", 0xff3050ff, 0);
    text(d, 40, 110, net_kind == NET_ADHOC ? "Ad-hoc (no router needed)" : "Same Wi-Fi network", 0xffb0b0b0, 0);
    text(d, 40, 150, lobby_hosting ? "This Vita is PLAYER 1 (host)" : "This Vita is PLAYER 2", 0xffffffff, 0);
    text(d, 40, 240, net_msg, 0xff40e0ff, 0);
    text(d, 40, 330, lobby_hosting ? "On the other Vita: start DemonStar, Play mode = join, then press Cross."
                                   : "On the other Vita: start DemonStar, Play mode = host, pick the game.", 0xffb0b0b0, 1);
    text(d, 40, 352, "Both Vitas need the same game files in ux0:data/DemonStar.", 0xffb0b0b0, 1);
    text(d, 40, 440, "Circle = cancel", 0xffc0c0c0, 0);
}

void message_screen(const char *title, const char *msg) {
    uint32_t *d = fb[fbi];
    fill(d, 0, 0, SW, SH, 0xff100404);
    fill(d, 0, 0, SW, 70, 0xff201010);
    text(d, 40, 20, title, 0xff3050ff, 0);
    /* (long messages wrap at ~60 characters) */
    char line[64]; const char *s = msg; int y = 140;
    while (*s && y < 400) {
        int n = (int)strlen(s);
        if (n > 60) { n = 60; while (n > 30 && s[n] != ' ') n--; }
        memcpy(line, s, n); line[n] = 0;
        text(d, 40, y, line, 0xffffffff, 0);
        s += n; while (*s == ' ') s++;
        y += 40;
    }
    text(d, 40, 460, "Press Cross or START", 0xffc0c0c0, 0);
    flip();
    SceCtrlData pad; uint32_t prev = ~0u;
    sceKernelDelayThread(300000);
    for (;;) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t pr = pad.buttons & ~prev; prev = pad.buttons;
        if (pr & (SCE_CTRL_CROSS | SCE_CTRL_START | SCE_CTRL_CIRCLE)) break;
        sceKernelDelayThread(16000);
    }
}

/* ---- minimal GXM, only so the system's ad-hoc dialog can be drawn ---- */
static int gxm_state;
static SceGxmSyncObject *gsync[NFB];
typedef struct { void *addr; } dq_data;
static void dq_callback(const void *data) {
    SceDisplayFrameBuf f = {sizeof f, ((const dq_data *)data)->addr, SW, SCE_DISPLAY_PIXELFORMAT_A8B8G8R8, SW, SH};
    sceDisplaySetFrameBuf(&f, SCE_DISPLAY_SETBUF_NEXTFRAME);
    sceDisplayWaitVblankStart();
}
static void *dlg_depth;
static int gxm_up(void) {
    if (gxm_state) return gxm_state > 0;
    gxm_state = -1;
    SceGxmInitializeParams p;
    memset(&p, 0, sizeof p);
    p.displayQueueMaxPendingCount = 2;
    p.displayQueueCallback = dq_callback;
    p.displayQueueCallbackDataSize = sizeof(dq_data);
    int r = -1;
    for (uint32_t size = 4 * 1024 * 1024; size >= 0x40000 && r < 0; size /= 2) {
        p.parameterBufferSize = size;
        r = sceGxmInitialize(&p);
        rt_log("net: sceGxmInitialize(%u KB) 0x%08x\n", size / 1024, r);
    }
    if (r < 0) return 0;
    uint32_t sz = (SW * SH * 4 + 0x3ffff) & ~0x3ffff;
    for (int k = 0; k < NFB; k++) {
        r = sceGxmMapMemory(fb[k], sz, SCE_GXM_MEMORY_ATTRIB_RW);
        if (r < 0) { rt_log("net: sceGxmMapMemory 0x%08x\n", r); return 0; }
        sceGxmSyncObjectCreate(&gsync[k]);
    }
    uint32_t dsz = (SW * SH * 4 + 0x3ffff) & ~0x3ffff;
    SceUID id = sceKernelAllocMemBlock("dlgdepth", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW_UNCACHE, dsz, NULL);
    if (id < 0) return 0;
    sceKernelGetMemBlockBase(id, &dlg_depth);
    if (sceGxmMapMemory(dlg_depth, dsz, SCE_GXM_MEMORY_ATTRIB_RW) < 0) return 0;
    gxm_state = 1;
    return 1;
}

/* runs the ad-hoc connection dialog; returns 1 when this Vita is in a group */
static int adhoc_dialog(char *err, int errlen) {
    if (!gxm_up()) { snprintf(err, errlen, "Could not start the graphics for the system dialog."); return 0; }
    SceNetAdhocctlGroupName group;
    memset(&group, 0, sizeof group);
    memcpy(group.data, "DEMONSTR", 8);
    SceNetCheckDialogParam p;
    sceNetCheckDialogParamInit(&p);
    p.mode = SCE_NETCHECK_DIALOG_MODE_PSP_ADHOC_CONN;
    p.groupName = &group;
    memcpy(p.npCommunicationId.data, "DMST00001", 9);
    p.npCommunicationId.term = 0;
    p.npCommunicationId.num = 0;
    p.timeoutUs = 0;
    int r = sceNetCheckDialogInit(&p);
    rt_log("net: sceNetCheckDialogInit 0x%08x\n", r);
    if (r < 0) { snprintf(err, errlen, "The system's ad-hoc dialog did not open (0x%08x).", r); return 0; }
    int prev = (fbi + NFB - 1) % NFB, status;
    snprintf(net_msg, sizeof net_msg, "Connecting...");
    for (;;) {
        status = sceNetCheckDialogGetStatus();
        if (status == SCE_COMMON_DIALOG_STATUS_FINISHED || status < 0) break;
        draw_lobby();
        SceCommonDialogUpdateParam u;
        memset(&u, 0, sizeof u);
        u.renderTarget.colorFormat = SCE_GXM_COLOR_FORMAT_A8B8G8R8;
        u.renderTarget.surfaceType = SCE_GXM_COLOR_SURFACE_LINEAR;
        u.renderTarget.width = SW;
        u.renderTarget.height = SH;
        u.renderTarget.strideInPixels = SW;
        u.renderTarget.colorSurfaceData = fb[fbi];
        u.renderTarget.depthSurfaceData = dlg_depth;
        u.displaySyncObject = gsync[fbi];
        sceCommonDialogUpdate(&u);
        dq_data dd = {fb[fbi]};
        sceGxmDisplayQueueAddEntry(gsync[prev], gsync[fbi], &dd);
        prev = fbi;
        fbi = (fbi + 1) % NFB;
    }
    sceGxmDisplayQueueFinish();
    SceNetCheckDialogResult res;
    memset(&res, 0, sizeof res);
    r = sceNetCheckDialogGetResult(&res);
    sceNetCheckDialogTerm();
    rt_log("net: dialog status %d, GetResult 0x%08x, result %d\n", status, r, res.result);
    if (r < 0 || res.result != SCE_COMMON_DIALOG_RESULT_OK) {
        snprintf(err, errlen, res.result == SCE_COMMON_DIALOG_RESULT_USER_CANCELED ? "Cancelled." :
                 "The Vita did not join an ad-hoc group. Is Wi-Fi switched on (flight mode off)?");
        return 0;
    }
    return 1;
}

static int net_lib_up(char *err, int errlen) {
    static int done;
    if (done) return 1;
    sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
    int r = sceNetShowNetstat();
    if (r == (int)SCE_NET_ERROR_ENOTINIT) {
        static SceNetInitParam p;
        p.size = 1024 * 1024;
        p.memory = malloc(p.size);
        p.flags = 0;
        r = sceNetInit(&p);
        rt_log("net: sceNetInit 0x%08x\n", r);
        if (r < 0) { snprintf(err, errlen, "The network library did not start (0x%08x).", r); return 0; }
    }
    r = sceNetCtlInit();
    rt_log("net: sceNetCtlInit 0x%08x\n", r);
    done = 1;
    return 1;
}

static void net_close(void) {
    if (pdp >= 0) { sceNetAdhocPdpDelete(pdp, 0); pdp = -1; }
    if (net_kind == NET_ADHOC) {
        sceNetCtlAdhocDisconnect();
        sceNetAdhocctlTerm();
        sceNetAdhocTerm();
    }
    if (sock >= 0) { sceNetSocketClose(sock); sock = -1; }
    net_kind = NET_OFF;
    have_peer = 0;
}

static int net_open(int adhoc, char *err, int errlen) {
    have_peer = 0;
    if (!net_lib_up(err, errlen)) return 0;
    if (adhoc) {
        net_kind = NET_ADHOC;
        sceSysmoduleLoadModule(SCE_SYSMODULE_PSPNET_ADHOC);
        int r = sceNetAdhocInit();
        rt_log("net: sceNetAdhocInit 0x%08x\n", r);
        SceNetAdhocctlAdhocId id;
        memset(&id, 0, sizeof id);
        id.type = SCE_NET_ADHOCCTL_ADHOCTYPE_RESERVED;
        memcpy(id.data, "DMST00001", SCE_NET_ADHOCCTL_ADHOCID_LEN);
        r = sceNetAdhocctlInit(&id);
        rt_log("net: sceNetAdhocctlInit 0x%08x\n", r);
        if (!adhoc_dialog(err, errlen)) { net_close(); return 0; }
        memset(&my_mac, 0, sizeof my_mac);
        sceNetAdhocctlGetEtherAddr(&my_mac);
        pdp = sceNetAdhocPdpCreate(&my_mac, ADHOC_PORT, 0x4000, 0);
        rt_log("net: PDP 0x%08x, MAC %02x:%02x:%02x:%02x:%02x:%02x\n", pdp, my_mac.data[0], my_mac.data[1], my_mac.data[2],
               my_mac.data[3], my_mac.data[4], my_mac.data[5]);
        if (pdp < 0) { snprintf(err, errlen, "Could not open the ad-hoc socket (0x%08x).", pdp); net_close(); return 0; }
        return 1;
    }
    net_kind = NET_INET;
    int state = 0;
    sceNetCtlInetGetState(&state);
    if (state != SCE_NETCTL_STATE_CONNECTED) {
        snprintf(err, errlen, "This Vita is not connected to Wi-Fi. Connect both Vitas to the same network in Settings, or use ad-hoc.");
        net_kind = NET_OFF;
        return 0;
    }
    SceNetCtlInfo info;
    memset(&info, 0, sizeof info);
    sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS, &info);
    sceNetInetPton(SCE_NET_AF_INET, info.ip_address, &my_ip);
    {
        SceNetCtlInfo nm; uint32_t mask = 0;
        memset(&nm, 0, sizeof nm);
        if (sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK, &nm) >= 0 && sceNetInetPton(SCE_NET_AF_INET, nm.netmask, &mask) > 0)
            my_bcast = my_ip | ~mask;
    }
    sock = sceNetSocket("demonstar", SCE_NET_AF_INET, SCE_NET_SOCK_DGRAM, 0);
    if (sock < 0) { snprintf(err, errlen, "Could not open a network socket (0x%08x).", sock); net_kind = NET_OFF; return 0; }
    int one = 1;
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_BROADCAST, &one, sizeof one);
    sceNetSetsockopt(sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO, &one, sizeof one);
    SceNetSockaddrIn a;
    memset(&a, 0, sizeof a);
    a.sin_len = sizeof a;
    a.sin_family = SCE_NET_AF_INET;
    a.sin_port = sceNetHtons(NET_PORT);
    a.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    int r = sceNetBind(sock, (SceNetSockaddr *)&a, sizeof a);
    rt_log("net: UDP socket %d bind 0x%08x, my IP %s\n", sock, r, info.ip_address);
    if (r < 0) { snprintf(err, errlen, "Could not open port %d (0x%08x).", NET_PORT, r); net_close(); return 0; }
    return 1;
}

/* the packet that fixes the peer: a HELLO for the host, INFO / REJECT for the joiner */
static int peer_packet(const void *buf, int len) {
    const uint8_t *b = buf;
    if (len < 8 || *(const uint32_t *)b != 0x504e5344u) return 0;
    return lobby_hosting ? b[4] == 1 : (b[4] == 2 || b[4] == 3);
}

int host_net_send(const void *buf, int len) {
    if (net_kind == NET_ADHOC && pdp >= 0) {
        if (have_peer) return sceNetAdhocPdpSend(pdp, &peer_mac, ADHOC_PORT, buf, len, 0, SCE_NET_ADHOC_F_NONBLOCK);
        /* no peer yet: broadcast, and also send to everyone in the group */
        SceNetEtherAddr bc;
        memset(bc.data, 0xff, 6);
        sceNetAdhocPdpSend(pdp, &bc, ADHOC_PORT, buf, len, 0, SCE_NET_ADHOC_F_NONBLOCK);
        static SceNetAdhocctlPeerInfo peers[8];
        int blen = sizeof peers;
        memset(peers, 0, sizeof peers);
        if (sceNetAdhocctlGetPeerList(&blen, peers) >= 0)
            for (int k = 0; k < blen / (int)sizeof peers[0] && k < 8; k++)
                if (memcmp(peers[k].macAddr.data, my_mac.data, 6))
                    sceNetAdhocPdpSend(pdp, &peers[k].macAddr, ADHOC_PORT, buf, len, 0, SCE_NET_ADHOC_F_NONBLOCK);
        return len;
    }
    if (net_kind == NET_INET && sock >= 0) {
        SceNetSockaddrIn a;
        memset(&a, 0, sizeof a);
        a.sin_len = sizeof a;
        a.sin_family = SCE_NET_AF_INET;
        a.sin_port = have_peer ? peer_port : sceNetHtons(NET_PORT);
        a.sin_addr.s_addr = have_peer ? peer_ip : SCE_NET_INADDR_BROADCAST;
        int r = sceNetSendto(sock, buf, len, 0, (SceNetSockaddr *)&a, sizeof a);
        if (!have_peer && my_bcast && my_bcast != SCE_NET_INADDR_BROADCAST) {
            a.sin_addr.s_addr = my_bcast;
            sceNetSendto(sock, buf, len, 0, (SceNetSockaddr *)&a, sizeof a);
        }
        return r;
    }
    return -1;
}

/* non-blocking; the first packet of this port from another console fixes the peer */
int host_net_recv(void *buf, int max) {
    for (;;) {
        if (net_kind == NET_ADHOC && pdp >= 0) {
            SceNetEtherAddr from;
            SceUShort16 port = 0;
            int len = max;
            int r = sceNetAdhocPdpRecv(pdp, &from, &port, buf, &len, 0, SCE_NET_ADHOC_F_NONBLOCK);
            if (r < 0 || len <= 0) return 0;
            if (!memcmp(from.data, my_mac.data, 6)) continue;
            if (!have_peer) { if (!peer_packet(buf, len)) continue; peer_mac = from; have_peer = 1; }
            else if (memcmp(from.data, peer_mac.data, 6)) continue;
            return len;
        }
        if (net_kind == NET_INET && sock >= 0) {
            SceNetSockaddrIn from;
            unsigned int fl = sizeof from;
            int r = sceNetRecvfrom(sock, buf, max, SCE_NET_MSG_DONTWAIT, (SceNetSockaddr *)&from, &fl);
            if (r <= 0) return 0;
            if (from.sin_addr.s_addr == my_ip) continue;
            if (!have_peer) {
                if (!peer_packet(buf, r)) continue;
                peer_ip = from.sin_addr.s_addr; peer_port = from.sin_port; have_peer = 1;
            } else if (from.sin_addr.s_addr != peer_ip) continue;
            return r;
        }
        return 0;
    }
}

int host_net_cancelled(void) {
    if (!in_lobby) return 0;
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t pr = pad.buttons & ~lobby_prev;
    lobby_prev = pad.buttons;
    uint32_t now = host_ticks_ms();
    if (now - lobby_drawn >= 33) { lobby_drawn = now; draw_lobby(); flip(); }
    sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DISABLE_AUTO_SUSPEND);
    return (pr & SCE_CTRL_CIRCLE) != 0;
}

void host_net_status(const char *msg, int ms) {
    if (!msg) { net_msg[0] = 0; return; }
    snprintf(net_msg, sizeof net_msg, "%s", msg);
    net_msg_until = ms ? host_ticks_ms() + ms : 0;
    if (in_lobby) { lobby_drawn = 0; host_net_cancelled(); return; }
    /* in game: put it on the frame being shown (nothing new is drawn while waiting) */
    banner(fb[(fbi + NFB - 1) % NFB]);
}

void host_net_end(const char *why) {
    in_lobby = 0;
    char m[160];
    snprintf(m, sizeof m, "The two-Vita game has ended: %s.", why);
    message_screen("DEMONSTAR  -  two-Vita play", m);
    net_close();
}

/* hosting: g is the game picked; joining: g is ignored.  Returns the game to run or -1. */
static int net_session(int mode, int g, char *err, int errlen) {
    int adhoc = mode == 1 || mode == 2, hosting = mode == 1 || mode == 3;
    lobby_hosting = hosting;
    net_kind = adhoc ? NET_ADHOC : NET_INET;
    if (!net_open(adhoc, err, errlen)) return -1;
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    lobby_prev = pad.buttons;
    in_lobby = 1;
    int r = hosting ? (np_host(g, &opts, err, errlen) < 0 ? -1 : g) : np_join(&opts, err, errlen);
    in_lobby = 0;
    net_msg[0] = 0;
    if (r < 0) net_close();
    return r;
}
