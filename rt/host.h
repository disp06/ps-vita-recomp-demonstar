/* platform abstraction (Linux/SDL test host, PS Vita host) */
#pragma once
#include <stdint.h>
#include <stdarg.h>

void host_vlog(const char *fmt, va_list ap);
void host_fatal(const char *msg) __attribute__((noreturn));
uint32_t host_ticks_ms(void);
uint64_t host_ticks_us(void);
void host_sleep_ms(uint32_t ms);

/* game data directory, e.g. "ux0:data/DemonStar/sm1" */
const char *host_game_dir(void);
const char *host_save_dir(void);

/* video: present a 8bpp frame with a 256-entry RGB palette (0x00RRGGBB) */
void host_video_init(int w, int h);
void host_present(const uint8_t *pix, int w, int h, int pitch, const uint32_t *pal);

/* audio: host pulls stereo s16 samples at HOST_AUDIO_RATE via audio_mix() */
#define HOST_AUDIO_RATE 44100
void host_audio_init(void);
void audio_mix(int16_t *out, int frames);   /* implemented by runtime */
void host_audio_lock(void);
void host_audio_unlock(void);

/* input */
enum {
    HB_UP = 1 << 0, HB_DOWN = 1 << 1, HB_LEFT = 1 << 2, HB_RIGHT = 1 << 3,
    HB_FIRE = 1 << 4, HB_BOMB = 1 << 5, HB_START = 1 << 6, HB_SELECT = 1 << 7,
    HB_L = 1 << 8, HB_R = 1 << 9, HB_TRIANGLE = 1 << 10, HB_SQUARE = 1 << 11,
};
typedef struct {
    uint32_t buttons;      /* HB_* */
    int ax, ay;            /* left analog -128..127 */
    int quit;
    int touch, tx, ty;     /* touch in game coordinates (unused) */
    int p2_present;        /* second controller (player 2) connected */
    uint32_t buttons2;     /* player 2 HB_* */
    int ax2, ay2;          /* player 2 left analog -128..127 */
} host_input_t;
void host_poll(host_input_t *in);
