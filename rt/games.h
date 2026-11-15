#pragma once
#include "rt.h"

#define NGAMES 2
typedef struct {
    const char *id, *title;
    const guest_func_entry *funcs;
    const guest_import_entry *imports;
    const uint32_t *entry;
} game_desc;
extern const game_desc games[NGAMES];

typedef struct {
    int aspect;            /* 0 = 4:3, 1 = full screen stretch */
    int ds1_difficulty;    /* -1 keep, 0..3 */
    int smooth;            /* 0 nearest, 1 smooth (bilinear-ish) */
} launch_opts;

int game_main(int idx, const launch_opts *opt);
extern int g_game_idx;
