/* headless Linux host used for testing the recompiled game */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "games.h"
extern const char *g_game_id;
static struct timespec t0;
static FILE *logf_;
static int frame_no, dump_every = 0;
static const char *dump_dir = "/tmp/frames";
static uint32_t max_time;
static pthread_mutex_t amtx = PTHREAD_MUTEX_INITIALIZER;
static FILE *wavf;
static int wav_frames;

void host_vlog(const char *fmt, va_list ap) {
    va_list a2; va_copy(a2, ap);
    vfprintf(stderr, fmt, ap);
    if (logf_) { vfprintf(logf_, fmt, a2); fflush(logf_); }
    va_end(a2);
}
static void dump_frame(const char *name);
void host_fatal(const char *msg) {
    fprintf(stderr, "FATAL: %s\n", msg);
    dump_frame("fatal");
    exit(1);
}
static double speed = 1.0;
uint64_t host_ticks_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)(((double)(t.tv_sec - t0.tv_sec) * 1000000.0 + (t.tv_nsec - t0.tv_nsec) / 1000) * speed);
}
uint32_t host_ticks_ms(void) { return (uint32_t)(host_ticks_us() / 1000); }
void host_sleep_ms(uint32_t ms) { usleep(ms ? (useconds_t)(ms * 1000 / speed) : 100); }
const char *host_game_dir(void) { static char b[256]; const char *s = getenv("DS_GAMEDIR"); if (s) return s; snprintf(b, sizeof b, "games/%s", g_game_id); return b; }
const char *host_save_dir(void) { static char b[256]; const char *s = getenv("DS_SAVEDIR"); if (s) return s; snprintf(b, sizeof b, "/tmp/dssave_%s", g_game_id); mkdir(b, 0755); return b; }
void *host_alloc_guest(uint32_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static uint8_t last[1024 * 1024]; static int lw, lh; static uint32_t lpal[256];
void host_video_init(int w, int h) { fprintf(stderr, "video init %dx%d\n", w, h); }
static void dump_frame(const char *name) {
    if (!lw) return;
    char p[512]; snprintf(p, sizeof p, "%s/%s.ppm", dump_dir, name);
    FILE *f = fopen(p, "wb");
    if (!f) return;
    fprintf(f, "P6 %d %d 255\n", lw, lh);
    for (int i = 0; i < lw * lh; i++) { uint32_t c = lpal[last[i]]; fputc(c >> 16, f); fputc(c >> 8, f); fputc(c, f); }
    fclose(f);
}
void host_present(const uint8_t *pix, int w, int h, int pitch, const uint32_t *pal) {
    lw = w; lh = h;
    for (int y = 0; y < h; y++) memcpy(last + y * w, pix + y * pitch, w);
    memcpy(lpal, pal, sizeof lpal);
    frame_no++;
    if (dump_every && frame_no % dump_every == 0) { char n[64]; snprintf(n, sizeof n, "f%06d", frame_no); dump_frame(n); }

}

/* input script: lines "<ms> <hexbuttons>" */
static struct { uint32_t t, b; } script[4096], script2[4096]; static int nscript, nscript2;
void host_poll(host_input_t *in) {
    uint32_t now = host_ticks_ms();
    uint32_t b = 0;
    for (int k = 0; k < nscript && script[k].t <= now; k++) b = script[k].b;
    if (getenv("DS_BOT") && (!getenv("DS_BOT_AFTER") || now > (uint32_t)atoi(getenv("DS_BOT_AFTER")))) {
        /* simple autoplay: fire constantly, sweep left/right, bomb now and then, press start/enter periodically */
        uint32_t t = now;
        b = getenv("DS_BOT_NOFIRE") ? 0 : HB_FIRE;
        uint32_t ph = (t / 1700) % 4;
        if (ph == 0) b |= HB_LEFT; else if (ph == 2) b |= HB_RIGHT;
        if ((t / 600) % 3 == 0) b |= HB_UP; else if ((t / 600) % 3 == 1) b |= HB_DOWN;
        if (t % 9000 < 150) b |= HB_BOMB;
        if (t % 7000 < 120 && (!getenv("DS_BOT_NOSTART") || t < 10000)) b |= HB_START;
        if (getenv("DS_BOT_KBD") && t > (uint32_t)atoi(getenv("DS_BOT_KBD"))) {
            /* exercise the L-trigger keyboard: type 'B', then R = Enter */
            uint32_t u = (t - atoi(getenv("DS_BOT_KBD"))) % 4000;
            b = 0;
            if (u < 2000) b |= HB_L;
            if (u > 300 && u < 400) b |= HB_UP;
            if (u > 600 && u < 700) b |= HB_UP;
            if (u > 1000 && u < 1100) b |= HB_FIRE;
            if (u > 2500 && u < 2600) b |= HB_R;
        }
        if (t % 5000 < 100 && (t < 30000 || getenv("DS_BOT_ENTER")) && !getenv("DS_BOT_NOSTART")) b = HB_TRIANGLE;
        if (t > 12000 && t < 12150 && !getenv("DS_BOT_AFTER")) b = HB_SELECT;
    }
    in->buttons = b;
    if (getenv("DS_P2")) {
        uint32_t b2 = 0;
        for (int k = 0; k < nscript2 && script2[k].t <= now; k++) b2 = script2[k].b;
        in->p2_present = 1; in->buttons2 = b2;
    }
    if (max_time && now > max_time) { static int d; if (!d) { d = 1; dump_frame("last"); fprintf(stderr, "max time reached (poll), frames=%d\n", frame_no); } in->quit = 1; }
    static uint32_t lastdump;
    if (getenv("DS_DUMP_MS") && now - lastdump >= (uint32_t)atoi(getenv("DS_DUMP_MS"))) {
        lastdump = now; char n[64]; snprintf(n, sizeof n, "t%06u", now); dump_frame(n);
    }
}

void host_audio_lock(void) { pthread_mutex_lock(&amtx); }
void host_audio_unlock(void) { pthread_mutex_unlock(&amtx); }
static void *audio_thread(void *arg) {
    (void)arg;
    int16_t buf[1024 * 2];
    uint64_t next = host_ticks_us();
    for (;;) {
        audio_mix(buf, 1024);
        if (wavf) { fwrite(buf, 4, 1024, wavf); wav_frames += 1024; }
        next += 1024ull * 1000000 / HOST_AUDIO_RATE;
        int64_t d = (int64_t)next - (int64_t)host_ticks_us();
        if (d > 0) usleep(d);
    }
    return NULL;
}
void host_audio_init(void) {
    static int started;
    if (started) return;
    started = 1;
    if (getenv("DS_WAV")) {
        wavf = fopen(getenv("DS_WAV"), "wb");
        /* raw s16 stereo 44100 */
    }
    pthread_t t; pthread_create(&t, NULL, audio_thread, NULL);
}

int main(int argc, char **argv) {
    clock_gettime(CLOCK_MONOTONIC, &t0);
    mkdir(dump_dir, 0755);
    logf_ = fopen("/tmp/ds.log", "w");
    if (getenv("DS_SPEED")) speed = atof(getenv("DS_SPEED"));
    if (getenv("DS_DUMP_EVERY")) dump_every = atoi(getenv("DS_DUMP_EVERY"));
    if (getenv("DS_MAXTIME")) max_time = atoi(getenv("DS_MAXTIME"));
    if (getenv("DS_INPUT2")) {
        FILE *f = fopen(getenv("DS_INPUT2"), "r");
        if (f) { while (nscript2 < 4096 && fscanf(f, "%u %x", &script2[nscript2].t, &script2[nscript2].b) == 2) nscript2++; fclose(f); }
    }
    if (getenv("DS_INPUT")) {
        FILE *f = fopen(getenv("DS_INPUT"), "r");
        if (f) { while (nscript < 4096 && fscanf(f, "%u %x", &script[nscript].t, &script[nscript].b) == 2) nscript++; fclose(f); }
    }
    (void)argc; (void)argv;
    { extern int rt_trace; rt_trace = getenv("DS_TRACE") != NULL; }
    int idx = (getenv("DS_GAME") && !strcmp(getenv("DS_GAME"), "sm1")) ? 1 : 0;
    launch_opts o = {0, -1, 0};
    if (getenv("DS_DIFF")) o.ds1_difficulty = atoi(getenv("DS_DIFF"));
    return game_main(idx, &o);
}
const char *host_soundfont_path(void) { const char *s = getenv("DS_SF2"); return s ? s : "GeneralUser-GS.sf2"; }
