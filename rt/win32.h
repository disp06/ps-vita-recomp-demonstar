#pragma once
#include "rt.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>

uint32_t guest_alloc(uint32_t size);
void guest_free(uint32_t a);
uint32_t guest_size(uint32_t a);
uint32_t guest_realloc(uint32_t a, uint32_t size);
uint32_t import_thunk_by_name(const char *name);
uint32_t image_end(void);

/* guest string helpers */
static inline const char *gstr(uint32_t a) { return a ? (const char *)(MEM + a) : NULL; }
uint32_t gstrdup(const char *s);

/* handles */
#define H_WINDOW    0x00010000u
#define H_MENU      0x00020000u
#define H_DC        0x00030000u
#define H_ICON      0x00040000u
#define H_GDI       0x00050000u
#define H_MODULE    0x00400000u
#define H_HEAP      0x00060000u
#define H_EVENT     0x00070000u
#define H_DLGITEM   0x00080000u

/* window messages */
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SHOWWINDOW 0x0018
#define WM_ACTIVATEAPP 0x001C
#define WM_SETCURSOR 0x0020
#define WM_GETMINMAXINFO 0x0024
#define WM_NCCREATE 0x0081
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_INITMENU 0x0116
#define WM_MOUSEMOVE 0x0200
#define WM_PALETTECHANGED 0x0311
#define WM_QUERYNEWPALETTE 0x030F
#define WM_USER 0x0400

void win_post(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
uint32_t win_send(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void win_pump_host(void);     /* poll host input and generate messages, run timers */
void win_present_screen(void);

/* keyboard state shared by GetAsyncKeyState / DirectInput */
extern uint8_t vk_state[256];
extern int app_quit_requested;

/* video state */
extern uint8_t *screen_pix; extern int screen_w, screen_h, screen_pitch;
extern uint32_t screen_pal[256];
void video_present(const uint8_t *pix, int w, int h, int pitch);

/* timers (winmm) */
void winmm_service(void);
void dsound_service(void);

/* registry */
void reg_load(void);
void reg_save(void);

/* misc */
extern uint32_t last_error;
const char *map_path(const char *winpath, char *out, int outlen);
extern uint32_t main_hwnd;
extern const char *g_game_id;
