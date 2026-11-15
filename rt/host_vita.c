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
static uint32_t quit_hold;
void host_poll(host_input_t *in) {
    SceCtrlData pad;
    memset(&pad, 0, sizeof pad);
    sceCtrlPeekBufferPositive(0, &pad, 1);
    uint32_t b = 0, p = pad.buttons;
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
    in->ax = (int)pad.lx - 128;
    in->ay = (int)pad.ly - 128;
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
    int a, b, c;
    if (fscanf(f, "%d %d %d", &a, &b, &c) == 3) { opts.aspect = (a >= 0 && a <= 2) ? a : 2; opts.ds1_difficulty = (b >= -1 && b <= 3) ? b : -1; opts.smooth = c; }
    fclose(f);
}
static void save_cfg(void) {
    sceIoMkdir(DATA_ROOT, 0777);
    sceIoMkdir(DATA_ROOT "/save", 0777);
    FILE *f = fopen(DATA_ROOT "/save/launcher.cfg", "w");
    if (!f) return;
    fprintf(f, "%d %d %d\n", opts.aspect, opts.ds1_difficulty, opts.smooth);
    fclose(f);
}

static int launcher(void) {
    static const char *diff[] = {"Keep game setting", "Easy", "Normal", "Hard", "Insane"};
    int sel = 0, nitems = 5;
    uint32_t prev = 0;
    int inst[NGAMES];
    for (int g = 0; g < NGAMES; g++) inst[g] = game_installed(g);
    if (!inst[0] && inst[1]) sel = 1;
    for (;;) {
        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        uint32_t p = pad.buttons, pr = p & ~prev;
        prev = p;
        if (pr & SCE_CTRL_UP) sel = (sel + nitems - 1) % nitems;
        if (pr & SCE_CTRL_DOWN) sel = (sel + 1) % nitems;
        int lr = (pr & SCE_CTRL_RIGHT) ? 1 : (pr & SCE_CTRL_LEFT) ? -1 : 0;
        if (sel == 2 && (lr || (pr & SCE_CTRL_CROSS))) opts.aspect = (opts.aspect + (lr < 0 ? 2 : 1)) % 3;
        if (sel == 3 && lr) opts.ds1_difficulty = (opts.ds1_difficulty + 1 + lr + 5) % 5 - 1;
        if (sel == 3 && (pr & SCE_CTRL_CROSS)) opts.ds1_difficulty = (opts.ds1_difficulty + 2) % 5 - 1;
        if (pr & SCE_CTRL_CROSS) {
            if (sel < NGAMES && inst[sel]) { save_cfg(); return sel; }
            if (sel == 4) { save_cfg(); return -1; }
        }
        uint32_t *d = fb[fbi];
        fill(d, 0, 0, SW, SH, 0xff100404);
        fill(d, 0, 0, SW, 70, 0xff201010);
        text(d, 40, 20, "DEMONSTAR  -  PS Vita", 0xff3050ff, 0);
        text(d, 600, 28, "static recompilation", 0xff8080a0, 1);
        const char *items[5];
        char buf[3][96];
        for (int g = 0; g < NGAMES; g++) {
            snprintf(buf[g], sizeof buf[g], "%s%s", games[g].title, inst[g] ? "" : "   (data missing)");
            items[g] = buf[g];
        }
        static const char *am[] = {"4:3", "Full screen (stretched)", "Original (PC window, 1:1 pixels)"};
        snprintf(buf[2], sizeof buf[2], "Screen: %s", am[opts.aspect]);
        items[2] = buf[2];
        static char dbuf[96];
        snprintf(dbuf, sizeof dbuf, "DemonStar difficulty: %s", diff[opts.ds1_difficulty + 1]);
        items[3] = dbuf;
        items[4] = "Exit";
        for (int k = 0; k < nitems; k++) {
            int y = 100 + k * 46 + (k >= 2 ? 16 : 0);
            if (k == sel) fill(d, 30, y - 4, 900, 40, 0xff402020);
            uint32_t c = (k < NGAMES && !inst[k]) ? 0xff707070 : 0xffffffff;
            text(d, 50, y, items[k], c, 0);
        }
        int y = 370;
        text(d, 50, y, "Controls:  D-pad / left stick = move   Cross / Square = fire   Circle = mega bomb (also selects in menus)", 0xffb0b0b0, 1); y += 20;
        text(d, 50, y, "           START = start player 1 (F2)   SELECT = Esc (menu in Secret Missions)", 0xffb0b0b0, 1); y += 20;
        text(d, 50, y, "           Triangle = Enter / pause   L + Up/Down/Cross = type letters   R = Enter   L+R+START+SELECT = quit", 0xffb0b0b0, 1); y += 34;
        text(d, 50, y, "Game data: " DATA_ROOT "/ds1/  and  " DATA_ROOT "/sm1/", 0xff80a0a0, 1); y += 20;
        text(d, 50, y, "(copy your own ds.exe and *.glb files there)", 0xff80a0a0, 1);
        flip();
        sceDisplayWaitVblankStart();
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    sceKernelCreateLwMutex(&amtx, "audio", 2, 0, NULL);
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
    int g = launcher();
    if (g < 0) { sceKernelExitProcess(0); return 0; }
    for (int k = 0; k < NFB; k++) memset(fb[k], 0, SW * SH * 4);
    game_main(g, &opts);
    sceKernelExitProcess(0);
    return 0;
}

/* process exit from the game (ExitProcess) */
void host_exit(void) { sceKernelExitProcess(0); }
