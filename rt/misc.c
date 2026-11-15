/* advapi32 registry, ole32, version, wsock32, shell32, comdlg32, dplay */
#include "win32.h"
#include <ctype.h>

/* ---------------- registry ---------------- */
typedef struct { char key[160]; char name[64]; uint32_t type; uint32_t len; uint8_t data[256]; } regval_t;
static regval_t regv[256]; static int nregv;
static char keys[64][160]; static int nkeys;
static int reg_dirty;

static void reg_path(char *dst) { snprintf(dst, 512, "%s/registry.txt", host_save_dir()); }
void reg_load(void) {
    char p[512]; reg_path(p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f) && nregv < 256) {
        regval_t *v = &regv[nregv];
        char *a = strtok(line, "\t"), *b = strtok(NULL, "\t"), *c = strtok(NULL, "\t"), *d = strtok(NULL, "\t\n");
        if (!a || !b || !c) continue;
        strncpy(v->key, a, 159); strncpy(v->name, strcmp(b, "@") ? b : "", 63); v->type = strtoul(c, 0, 10);
        v->len = 0;
        if (d) for (char *s = d; s[0] && s[1] && v->len < 256; s += 2) { unsigned x; sscanf(s, "%2x", &x); v->data[v->len++] = x; }
        nregv++;
    }
    fclose(f);
}
void reg_save(void) {
    if (!reg_dirty) return;
    char p[512]; reg_path(p);
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int k = 0; k < nregv; k++) {
        fprintf(f, "%s\t%s\t%u\t", regv[k].key, regv[k].name[0] ? regv[k].name : "@", regv[k].type);
        for (uint32_t j = 0; j < regv[k].len; j++) fprintf(f, "%02x", regv[k].data[j]);
        fprintf(f, "\n");
    }
    fclose(f);
    reg_dirty = 0;
}
static uint32_t key_handle(const char *path) {
    for (int k = 0; k < nkeys; k++) if (!strcasecmp(keys[k], path)) return 0x7000 + k;
    if (nkeys >= 64) return 0;
    strncpy(keys[nkeys], path, 159);
    return 0x7000 + nkeys++;
}
static const char *key_path(uint32_t h) {
    switch (h) {
    case 0x80000000u: return "HKCR";
    case 0x80000001u: return "HKCU";
    case 0x80000002u: return "HKLM";
    case 0x80000003u: return "HKU";
    }
    if (h >= 0x7000 && h < 0x7000u + (uint32_t)nkeys) return keys[h - 0x7000];
    return "?";
}
static int key_exists(const char *path) {
    size_t n = strlen(path);
    for (int k = 0; k < nregv; k++) if (!strncasecmp(regv[k].key, path, n) && (regv[k].key[n] == 0 || regv[k].key[n] == '\\')) return 1;
    for (int k = 0; k < nkeys; k++) if (!strcasecmp(keys[k], path)) return 1;
    return 0;
}
static uint32_t open_key(uint32_t parent, const char *sub, uint32_t out, int create) {
    char p[400];
    if (sub && *sub) snprintf(p, sizeof p, "%s\\%s", key_path(parent), sub); else snprintf(p, sizeof p, "%s", key_path(parent));
    if (!create && !key_exists(p)) { return 2; }
    wr32(out, key_handle(p));
    return 0;
}
SHIM(ADVAPI32, RegOpenKeyExA, 5) { return open_key(ARG(0), gstr(ARG(1)), ARG(4), 0); }
SHIM(ADVAPI32, RegCreateKeyExA, 9) {
    const char *sub = gstr(ARG(1));
    char p[400]; snprintf(p, sizeof p, "%s\\%s", key_path(ARG(0)), sub ? sub : "");
    int existed = key_exists(p);
    uint32_t r = open_key(ARG(0), sub, ARG(7), 1);
    if (ARG(8)) wr32(ARG(8), existed ? 2 : 1);
    return r;
}
SHIM(ADVAPI32, RegCloseKey, 1) { reg_save(); return 0; }
static regval_t *find_val(const char *key, const char *name) {
    for (int k = 0; k < nregv; k++) if (!strcasecmp(regv[k].key, key) && !strcasecmp(regv[k].name, name)) return &regv[k];
    return NULL;
}
SHIM(ADVAPI32, RegQueryValueExA, 6) {
    const char *key = key_path(ARG(0));
    const char *name = ARG(1) ? gstr(ARG(1)) : "";
    uint32_t tp = ARG(3), data = ARG(4), cb = ARG(5);
    regval_t *v = find_val(key, name);
    if (!v) return 2;
    if (tp) wr32(tp, v->type);
    if (cb) {
        uint32_t have = rd32(cb);
        wr32(cb, v->len);
        if (data) { if (have < v->len) return 234; memcpy(MEM + data, v->data, v->len); }
    }
    return 0;
}
SHIM(ADVAPI32, RegSetValueExA, 6) {
    const char *key = key_path(ARG(0));
    const char *name = ARG(1) ? gstr(ARG(1)) : "";
    regval_t *v = find_val(key, name);
    if (!v) { if (nregv >= 256) return 1; v = &regv[nregv++]; strncpy(v->key, key, 159); strncpy(v->name, name, 63); }
    v->type = ARG(3);
    v->len = ARG(5) > 256 ? 256 : ARG(5);
    memcpy(v->data, MEM + ARG(4), v->len);
    reg_dirty = 1;
    return 0;
}
SHIM(ADVAPI32, RegEnumKeyExA, 8) { return 259; }   /* ERROR_NO_MORE_ITEMS */

/* ---------------- ole32 ---------------- */
SHIM(OLE32, CoInitialize, 1) { return 0; }
SHIM(OLE32, CoUninitialize, 0) { return 0; }
SHIM(OLE32, CoCreateInstance, 5) { if (ARG(4)) wr32(ARG(4), 0); return 0x80040154u; }   /* REGDB_E_CLASSNOTREG */

/* ---------------- version ---------------- */
SHIM(VERSION, GetFileVersionInfoSizeA, 2) { if (ARG(1)) wr32(ARG(1), 0); return 0; }
SHIM(VERSION, GetFileVersionInfoA, 4) { return 0; }
SHIM(VERSION, VerQueryValueA, 4) { return 0; }

/* ---------------- wsock32 ---------------- */
SHIM(WSOCK32, WSAStartup, 2) { return 10091; }   /* WSASYSNOTREADY */
SHIM(WSOCK32, WSACleanup, 0) { return 0; }
SHIM(WSOCK32, gethostname, 2) { strncpy((char *)MEM + ARG(0), "vita", ARG(1)); return 0; }
SHIM(WSOCK32, gethostbyname, 1) { return 0; }
SHIM(WSOCK32, inet_ntoa, 1) { static uint32_t s; if (!s) s = gstrdup("127.0.0.1"); return s; }

/* ---------------- shell32 / comdlg32 / dplay ---------------- */
SHIM(SHELL32, ShellAboutA, 4) { return 1; }
SHIM(SHELL32, ShellExecuteA, 6) { return 2; }
SHIM(COMDLG32, GetOpenFileNameA, 1) { return 0; }
SHIM(COMDLG32, GetSaveFileNameA, 1) { return 0; }
SHIM(DPLAYX, ord4, 5) { return 0x80004005u; }   /* DirectPlayLobbyCreate fails: no multiplayer */

void reg_seed_dword(const char *key, const char *name, uint32_t v) {
    regval_t *r = find_val(key, name);
    if (!r) { if (nregv >= 256) return; r = &regv[nregv++]; strncpy(r->key, key, 159); strncpy(r->name, name, 63); }
    r->type = 4; r->len = 4; memcpy(r->data, &v, 4);
    reg_dirty = 1;
}
