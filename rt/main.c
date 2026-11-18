/* game entry: loads the original ds.exe image and runs the recompiled code */
#include "win32.h"
#include "games.h"

extern void heap_init(uint32_t lo, uint32_t hi);
extern int load_pe(const char *path, uint32_t *entry);
extern void rt_init(const guest_func_entry *funcs, const guest_import_entry *imps);
void *host_alloc_guest(uint32_t size);

extern const guest_func_entry ds1_funcs[], sm1_funcs[];
extern const guest_import_entry ds1_imports[], sm1_imports[];
extern const uint32_t ds1_entry, sm1_entry;

const game_desc games[NGAMES] = {
    {"ds1", "DemonStar", ds1_funcs, ds1_imports, &ds1_entry},
    {"sm1", "DemonStar: Secret Missions 1", sm1_funcs, sm1_imports, &sm1_entry},
};
const char *g_game_id = "ds1";
int g_game_idx;

/* registry pre-seeding from launcher options (DemonStar 1 has no in-game options) */
extern void reg_seed_dword(const char *key, const char *name, uint32_t v);

int game_main(int idx, const launch_opts *opt) {
    g_game_idx = idx;
    g_game_id = games[idx].id;
    MEM = host_alloc_guest(GUEST_MEM_SIZE);
    if (!MEM) host_fatal("cannot allocate guest memory");
    memset(MEM, 0, GUEST_MEM_SIZE);
    char path[600];
    if (!map_path("ds.exe", path, sizeof path)) host_fatal("ds.exe not found in the game data folder");
    uint32_t entry;
    int r = load_pe(path, &entry);
    if (r) { rt_log("load_pe failed %d\n", r); host_fatal("cannot load ds.exe"); }
    if (entry != *games[idx].entry) host_fatal("ds.exe does not match this build (wrong game version?)");
    heap_init(image_end() + 0x10000, GUEST_MEM_SIZE - 0x1000000);
    rt_init(games[idx].funcs, games[idx].imports);
    reg_load();
    if (opt && idx == 0 && opt->ds1_difficulty >= 0)
        reg_seed_dword("HKCU\\Software\\Mountain King Studios\\DemonStar", "OptDifficulty", opt->ds1_difficulty);
    {
        extern uint32_t reg_get_dword(const char *key, const char *name, uint32_t def);
        host_input_t in; memset(&in, 0, sizeof in); host_poll(&in);
        rt_log("player 2 controller: %s\n", in.p2_present ? "connected" : "not connected");
        if (idx == 1) {
            const char *k = "HKCU\\Software\\Mountain King Studios\\DemonStarSM1";
            if (in.p2_present) {
                /* SM1 input types: 0 keyboard, 1 mouse, 2 joystick 1, 3 joystick 2.
                   The Bluetooth pad is its joystick 1: give it to player 2 and keep
                   player 1 on the Vita's own controls (keyboard mapping). */
                reg_seed_dword(k, "OptPlayer2DefInput", 2);
                if (reg_get_dword(k, "OptPlayer1DefInput", 0) >= 2) reg_seed_dword(k, "OptPlayer1DefInput", 0);
            } else {
                if (reg_get_dword(k, "OptPlayer2DefInput", 0) >= 2) reg_seed_dword(k, "OptPlayer2DefInput", 0);
                if (reg_get_dword(k, "OptPlayer1DefInput", 0) >= 2) reg_seed_dword(k, "OptPlayer1DefInput", 0);
            }
        }
    }
    rt_log("starting %s at %08x\n", g_game_id, entry);
    R.esp -= 4; wr32(R.esp, 0xffffeff0u);
    call_ind(entry);
    reg_save();
    rt_log("entry returned\n");
    return 0;
}
