#include "win32.h"
#include <time.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

uint32_t last_error;

uint32_t gstrdup(const char *s) {
    uint32_t n = strlen(s) + 1;
    uint32_t a = guest_alloc(n);
    memcpy(MEM + a, s, n);
    return a;
}

/* --------- path mapping --------- */
static int ci_find(const char *dir, const char *name, char *out, int outlen) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    struct dirent *e;
    int ok = 0;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, outlen, "%s/%s", dir, e->d_name);
            ok = 1; break;
        }
    }
    closedir(d);
    return ok;
}

const char *map_path(const char *wp, char *out, int outlen) {
    /* basename of the windows path */
    const char *b = wp;
    for (const char *p = wp; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
    if (ci_find(rt_save_dir(), b, out, outlen)) return out;
    if (ci_find(host_game_dir(), b, out, outlen)) return out;
    /* subfolder newstuff\ */
    if (strstr(wp, "newstuff") || strstr(wp, "NEWSTUFF")) {
        char sub[512]; snprintf(sub, sizeof sub, "%s/newstuff", host_game_dir());
        if (ci_find(sub, b, out, outlen)) return out;
    }
    return NULL;
}

/* --------- files --------- */
typedef struct { FILE *fp; int used; char path[512]; } hfile_t;
static hfile_t files[64];
#define FH_BASE 0x1000u
#define INVALID_HANDLE 0xffffffffu

static hfile_t *fh(uint32_t h) {
    if (h < FH_BASE || h >= FH_BASE + 64 * 4) return NULL;
    hfile_t *f = &files[(h - FH_BASE) / 4];
    return f->used ? f : NULL;
}

SHIM(KERNEL32, CreateFileA, 7) {
    const char *name = gstr(ARG(0));
    uint32_t access = ARG(1), disp = ARG(4);
    char path[600];
    const char *mode;
    int write = (access & 0x40000000u) != 0;
    const char *found = map_path(name, path, sizeof path);
    if (!write) {
        if (!found) { rt_log("CreateFileA(%s) read: not found\n", name); last_error = 2; return INVALID_HANDLE; }
        mode = "rb";
    } else {
        /* writes always go to the save dir */
        const char *b = name;
        for (const char *p = name; *p; p++) if (*p == '\\' || *p == '/' || *p == ':') b = p + 1;
        char sp[600]; snprintf(sp, sizeof sp, "%s/%s", rt_save_dir(), b);
        if (found && strncmp(found, rt_save_dir(), strlen(rt_save_dir())) != 0 && (disp == 3 || disp == 4)) {
            /* copy-on-write from game dir to save dir */
            FILE *a = fopen(found, "rb"), *o = fopen(sp, "wb");
            if (a && o) { char buf[4096]; size_t n; while ((n = fread(buf, 1, sizeof buf, a)) > 0) fwrite(buf, 1, n, o); }
            if (a) fclose(a);
            if (o) fclose(o);
        }
        strcpy(path, sp);
        struct stat st;
        int exists = stat(path, &st) == 0;
        switch (disp) {
        case 1: if (exists) { last_error = 80; return INVALID_HANDLE; } mode = "w+b"; break; /* CREATE_NEW */
        case 2: mode = "w+b"; break;                    /* CREATE_ALWAYS */
        case 3: if (!exists) { last_error = 2; return INVALID_HANDLE; } mode = "r+b"; break;  /* OPEN_EXISTING */
        case 4: mode = exists ? "r+b" : "w+b"; break;   /* OPEN_ALWAYS */
        case 5: if (!exists) { last_error = 2; return INVALID_HANDLE; } mode = "w+b"; break;  /* TRUNCATE */
        default: mode = "r+b";
        }
    }
    FILE *fp = fopen(path, mode);
    if (!fp) { rt_log("CreateFileA(%s) -> fopen(%s,%s) failed\n", name, path, mode); last_error = 2; return INVALID_HANDLE; }
    for (int k = 0; k < 64; k++) if (!files[k].used) {
        files[k].used = 1; files[k].fp = fp; strncpy(files[k].path, path, 511);
        last_error = 0;
        return FH_BASE + 4 * k;
    }
    fclose(fp);
    return INVALID_HANDLE;
}

SHIM(KERNEL32, ReadFile, 5) {
    hfile_t *f = fh(ARG(0));
    uint32_t buf = ARG(1), n = ARG(2), pread = ARG(3);
    if (!f) { if (pread) wr32(pread, 0); return 0; }
    size_t r = fread(MEM + buf, 1, n, f->fp);
    if (pread) wr32(pread, (uint32_t)r);
    return 1;
}

SHIM(KERNEL32, WriteFile, 5) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    hfile_t *f = fh(h);
    if (!f) {
        /* std handles */
        if (h == 0xfff4 || h == 0xfff5) { fwrite(MEM + buf, 1, n, stderr); if (pw) wr32(pw, n); return 1; }
        if (pw) wr32(pw, 0); return 0;
    }
    size_t r = fwrite(MEM + buf, 1, n, f->fp);
    if (pw) wr32(pw, (uint32_t)r);
    return 1;
}

SHIM(KERNEL32, SetFilePointer, 4) {
    hfile_t *f = fh(ARG(0));
    if (!f) return 0xffffffffu;
    int32_t dist = (int32_t)ARG(1);
    uint32_t hi = ARG(2), method = ARG(3);
    if (hi) wr32(hi, 0);
    fseek(f->fp, dist, method == 0 ? SEEK_SET : method == 1 ? SEEK_CUR : SEEK_END);
    return (uint32_t)ftell(f->fp);
}

SHIM(KERNEL32, CloseHandle, 1) {
    hfile_t *f = fh(ARG(0));
    if (f) { fclose(f->fp); f->used = 0; }
    return 1;
}
SHIM(KERNEL32, SetEndOfFile, 1) { return 1; }
SHIM(KERNEL32, FlushFileBuffers, 1) { hfile_t *f = fh(ARG(0)); if (f) fflush(f->fp); return 1; }
SHIM(KERNEL32, GetFileType, 1) { uint32_t h = ARG(0); if (fh(h)) return 1; if (h >= 0xfff0) return 2; return 0; }
SHIM(KERNEL32, GetStdHandle, 1) { return 0xfff0 + (ARG(0) & 0xf); }
SHIM(KERNEL32, SetStdHandle, 2) { return 1; }
SHIM(KERNEL32, SetHandleCount, 1) { return ARG(0); }
SHIM(KERNEL32, GetFullPathNameA, 4) {
    const char *n = gstr(ARG(0)); uint32_t len = ARG(1), buf = ARG(2), fp = ARG(3);
    char tmp[512];
    if (n[0] && n[1] == ':') snprintf(tmp, sizeof tmp, "%s", n); else snprintf(tmp, sizeof tmp, "C:\\DEMONSTAR\\%s", n);
    uint32_t l = strlen(tmp);
    if (l + 1 > len) return l + 1;
    memcpy(MEM + buf, tmp, l + 1);
    if (fp) { const char *b = strrchr(tmp, '\\'); wr32(fp, buf + (b ? (uint32_t)(b - tmp + 1) : 0)); }
    return l;
}

/* --------- memory --------- */
SHIM(KERNEL32, HeapCreate, 3) { return H_HEAP; }
SHIM(KERNEL32, HeapDestroy, 1) { return 1; }
SHIM(KERNEL32, HeapAlloc, 3) { uint32_t a = guest_alloc(ARG(2)); return a; }
SHIM(KERNEL32, HeapFree, 3) { guest_free(ARG(2)); return 1; }
SHIM(KERNEL32, HeapReAlloc, 4) {
    uint32_t flags = ARG(1), p = ARG(2), sz = ARG(3);
    if (flags & 0x10) { /* HEAP_REALLOC_IN_PLACE_ONLY */ return guest_size(p) >= sz ? p : 0; }
    return guest_realloc(p, sz);
}
SHIM(KERNEL32, GlobalAlloc, 2) { return guest_alloc(ARG(1)); }
SHIM(KERNEL32, GlobalFree, 1) { guest_free(ARG(0)); return 0; }
SHIM(KERNEL32, GlobalLock, 1) { return ARG(0); }
SHIM(KERNEL32, GlobalUnlock, 1) { return 1; }
SHIM(KERNEL32, GlobalHandle, 1) { return ARG(0); }
SHIM(KERNEL32, LocalAlloc, 2) { return guest_alloc(ARG(1)); }
SHIM(KERNEL32, LocalFree, 1) { guest_free(ARG(0)); return 0; }

/* VirtualAlloc: regions carved downward from top of guest memory */
static uint32_t va_top = GUEST_MEM_SIZE;
typedef struct { uint32_t base, size; } vres_t;
static vres_t vres[256]; static int nvres;
SHIM(KERNEL32, VirtualAlloc, 4) {
    uint32_t addr = ARG(0), size = ARG(1), type = ARG(2);
    if (addr) {
        for (int k = 0; k < nvres; k++)
            if (addr >= vres[k].base && addr < vres[k].base + vres[k].size) return addr & ~0xfffu;
        if (type & 0x2000) return 0;
    }
    size = (size + 0xffff) & ~0xffffu;
    uint32_t lim = GUEST_MEM_SIZE - 0x1000000u;  /* top 16MB reserved for VirtualAlloc */
    if (va_top - size < lim) { rt_log("VirtualAlloc out of space\n"); return 0; }
    va_top -= size;
    memset(MEM + va_top, 0, size);
    if (nvres < 256) { vres[nvres].base = va_top; vres[nvres].size = size; nvres++; }
    return va_top;
}
SHIM(KERNEL32, VirtualFree, 3) { return 1; }

/* --------- process / env --------- */
static uint32_t cmdline_a, modname_a, env_a;
SHIM(KERNEL32, GetCommandLineA, 0) { if (!cmdline_a) cmdline_a = gstrdup("\"C:\\DEMONSTAR\\DS.EXE\""); return cmdline_a; }
SHIM(KERNEL32, GetModuleFileNameA, 3) {
    uint32_t buf = ARG(1), n = ARG(2);
    const char *s = "C:\\DEMONSTAR\\DS.EXE";
    uint32_t l = strlen(s);
    if (l + 1 > n) l = n - 1;
    memcpy(MEM + buf, s, l); wr8(buf + l, 0);
    (void)modname_a;
    return l;
}
SHIM(KERNEL32, GetModuleHandleA, 1) { return ARG(0) ? 0 : H_MODULE; }
SHIM(KERNEL32, GetEnvironmentStrings, 0) {
    if (!env_a) { env_a = guest_alloc(16); }
    return env_a;
}
SHIM(KERNEL32, GetEnvironmentStringsW, 0) { return 0; }
SHIM(KERNEL32, FreeEnvironmentStringsA, 1) { return 1; }
SHIM(KERNEL32, FreeEnvironmentStringsW, 1) { return 1; }
SHIM(KERNEL32, GetEnvironmentVariableA, 3) { return 0; }
SHIM(KERNEL32, SetEnvironmentVariableA, 2) { return 1; }
SHIM(KERNEL32, GetStartupInfoA, 1) { uint32_t p = ARG(0); memset(MEM + p, 0, 68); wr32(p, 68); return 0; }
SHIM(KERNEL32, GetVersion, 0) { return 0xC0000A04u; }   /* Windows 98 */
SHIM(KERNEL32, GetVersionExA, 1) {
    uint32_t p = ARG(0);
    wr32(p + 4, 4); wr32(p + 8, 10); wr32(p + 12, 0x040A0898); wr32(p + 16, 1); /* VER_PLATFORM_WIN32_WINDOWS */
    memset(MEM + p + 20, 0, 128);
    return 1;
}
SHIM(KERNEL32, GetSystemInfo, 1) {
    uint32_t p = ARG(0); memset(MEM + p, 0, 36);
    wr32(p + 4, 0x1000); wr32(p + 8, 0x10000); wr32(p + 12, 0x7ffeffff); wr32(p + 16, 1); wr32(p + 20, 1);
    wr32(p + 24, 586); wr32(p + 28, 0x10000);
    return 0;
}
SHIM(KERNEL32, GetCurrentProcess, 0) { return 0xffffffffu; }
SHIM(KERNEL32, GetCurrentThread, 0) { return 0xfffffffeu; }
SHIM(KERNEL32, SetPriorityClass, 2) { return 1; }
SHIM(KERNEL32, SetThreadPriority, 2) { return 1; }
SHIM(KERNEL32, GetLastError, 0) { return last_error; }
SHIM(KERNEL32, ExitProcess, 1) { reg_save(); rt_log("ExitProcess(%u)\n", ARG(0)); exit(0); return 0; }
SHIM(KERNEL32, TerminateProcess, 2) { reg_save(); exit(0); return 0; }
SHIM(KERNEL32, UnhandledExceptionFilter, 1) { return 0; }
SHIM(KERNEL32, RtlUnwind, 4) { return 0; }
SHIM(KERNEL32, WinExec, 2) { return 33; }
SHIM(KERNEL32, CreateEventA, 4) { return H_EVENT; }

SHIM(KERNEL32, LoadLibraryA, 1) {
    const char *n = gstr(ARG(0));
    rt_log("LoadLibraryA(%s)\n", n);
    if (n && (!strcasecmp(n, "user32.dll") || !strcasecmp(n, "kernel32.dll") || !strcasecmp(n, "kernel32"))) return 0x77000000u;
    return 0;
}
SHIM(KERNEL32, GetProcAddress, 2) {
    uint32_t n = ARG(1);
    if (n < 0x10000) return 0;
    const char *s = gstr(n);
    uint32_t t = import_thunk_by_name(s);
    rt_log("GetProcAddress(%s) -> %08x\n", s, t);
    return t;
}

/* --------- time --------- */
SHIM(KERNEL32, Sleep, 1) {
    uint32_t ms = ARG(0);
    if (g_netplay) {
        uint32_t k = 0;
        do { rt_advance_us(ms ? 1000 : 100); winmm_service(); dsound_service(); } while (++k < ms);
        return 0;
    }
    uint32_t end = host_ticks_ms() + ms;
    do {
        winmm_service();
        dsound_service();
        uint32_t now = host_ticks_ms();
        if ((int32_t)(end - now) <= 0) break;
        host_sleep_ms((end - now) > 2 ? 2 : (end - now));
    } while (1);
    return 0;
}
SHIM(KERNEL32, GetLocalTime, 1) {
    uint32_t p = ARG(0);
    extern int64_t g_fixed_time;
    time_t t = g_netplay ? (time_t)g_fixed_time : time(NULL); struct tm *tm = g_netplay ? gmtime(&t) : localtime(&t);
    wr16(p, tm->tm_year + 1900); wr16(p + 2, tm->tm_mon + 1); wr16(p + 4, tm->tm_wday); wr16(p + 6, tm->tm_mday);
    wr16(p + 8, tm->tm_hour); wr16(p + 10, tm->tm_min); wr16(p + 12, tm->tm_sec); wr16(p + 14, 0);
    return 0;
}
SHIM(KERNEL32, QueryPerformanceFrequency, 1) { wr64(ARG(0), 1000000ull); return 1; }
SHIM(KERNEL32, QueryPerformanceCounter, 1) {
    /* games busy-wait on this for frame pacing: yield the CPU now and then */
    static uint32_t n;
    if (g_netplay) rt_advance_us(100);
    else if ((++n & 31) == 0) host_sleep_ms(0);
    winmm_service();
    wr64(ARG(0), rt_us());
    return 1;
}

/* --------- strings / locale --------- */
SHIM(KERNEL32, lstrcpyA, 2) { strcpy((char *)MEM + ARG(0), gstr(ARG(1))); return ARG(0); }
SHIM(KERNEL32, lstrlenA, 1) { return ARG(0) ? strlen(gstr(ARG(0))) : 0; }
SHIM(KERNEL32, GetACP, 0) { return 1252; }
SHIM(KERNEL32, GetOEMCP, 0) { return 437; }
SHIM(KERNEL32, GetCPInfo, 2) {
    uint32_t p = ARG(1);
    memset(MEM + p, 0, 20); wr32(p, 1); wr8(p + 4, '?');
    return 1;
}
SHIM(KERNEL32, MultiByteToWideChar, 6) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); uint32_t dn = ARG(5);
    if (n < 0) n = strlen(gstr(src)) + 1;
    if (!dn) return n;
    int k;
    for (k = 0; k < n && (uint32_t)k < dn; k++) wr16(dst + 2 * k, rd8(src + k));
    return k;
}
SHIM(KERNEL32, WideCharToMultiByte, 8) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); uint32_t dn = ARG(5);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (!dn) return n;
    int k;
    for (k = 0; k < n && (uint32_t)k < dn; k++) { uint16_t c = rd16(src + 2 * k); wr8(dst + k, c < 256 ? c : '?'); }
    return k;
}
SHIM(KERNEL32, GetStringTypeA, 5) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t out = ARG(4);
    if (n < 0) n = strlen(gstr(src)) + 1;
    for (int k = 0; k < n; k++) {
        int c = rd8(src + k); uint16_t t = 0;
        if (isupper(c)) t |= 1; if (islower(c)) t |= 2; if (isdigit(c)) t |= 4; if (isspace(c)) t |= 8;
        if (ispunct(c)) t |= 0x10; if (iscntrl(c)) t |= 0x20; if (c == ' ' || c == '\t') t |= 0x40; if (isxdigit(c)) t |= 0x80;
        if (isalpha(c)) t |= 0x100;
        wr16(out + 2 * k, t);
    }
    return 1;
}
SHIM(KERNEL32, GetStringTypeW, 4) {
    uint32_t src = ARG(1); int32_t n = (int32_t)ARG(2); uint32_t out = ARG(3);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    for (int k = 0; k < n; k++) {
        int c = rd16(src + 2 * k); if (c > 255) c = 0; uint16_t t = 0;
        if (isupper(c)) t |= 1; if (islower(c)) t |= 2; if (isdigit(c)) t |= 4; if (isspace(c)) t |= 8;
        if (ispunct(c)) t |= 0x10; if (iscntrl(c)) t |= 0x20; if (c == ' ' || c == '\t') t |= 0x40; if (isxdigit(c)) t |= 0x80;
        if (isalpha(c)) t |= 0x100;
        wr16(out + 2 * k, t);
    }
    return 1;
}
SHIM(KERNEL32, LCMapStringA, 6) {
    uint32_t flags = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), dn = ARG(5);
    if (n < 0) n = strlen(gstr(src)) + 1;
    if (!dn) return n;
    for (int k = 0; k < n && (uint32_t)k < dn; k++) {
        int c = rd8(src + k);
        if (flags & 0x100) c = tolower(c); else if (flags & 0x200) c = toupper(c);
        wr8(dst + k, c);
    }
    return n;
}
SHIM(KERNEL32, LCMapStringW, 6) {
    uint32_t flags = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4), dn = ARG(5);
    if (n < 0) { n = 0; while (rd16(src + 2 * n)) n++; n++; }
    if (!dn) return n;
    for (int k = 0; k < n && (uint32_t)k < dn; k++) {
        int c = rd16(src + 2 * k);
        if (c < 256) { if (flags & 0x100) c = tolower(c); else if (flags & 0x200) c = toupper(c); }
        wr16(dst + 2 * k, c);
    }
    return n;
}
static int cmp_ci(int a, int b, int ci) { if (ci) { a = tolower(a); b = tolower(b); } return a - b; }
SHIM(KERNEL32, CompareStringA, 6) {
    uint32_t fl = ARG(1), s1 = ARG(2); int32_t n1 = ARG(3); uint32_t s2 = ARG(4); int32_t n2 = ARG(5);
    if (n1 < 0) n1 = strlen(gstr(s1));
    if (n2 < 0) n2 = strlen(gstr(s2));
    int k = 0;
    for (; k < n1 && k < n2; k++) { int d = cmp_ci(rd8(s1 + k), rd8(s2 + k), fl & 1); if (d) return d < 0 ? 1 : 3; }
    return n1 < n2 ? 1 : n1 > n2 ? 3 : 2;
}
SHIM(KERNEL32, CompareStringW, 6) {
    uint32_t fl = ARG(1), s1 = ARG(2); int32_t n1 = ARG(3); uint32_t s2 = ARG(4); int32_t n2 = ARG(5);
    if (n1 < 0) { n1 = 0; while (rd16(s1 + 2 * n1)) n1++; }
    if (n2 < 0) { n2 = 0; while (rd16(s2 + 2 * n2)) n2++; }
    int k = 0;
    for (; k < n1 && k < n2; k++) { int d = cmp_ci(rd16(s1 + 2 * k), rd16(s2 + 2 * k), fl & 1); if (d) return d < 0 ? 1 : 3; }
    return n1 < n2 ? 1 : n1 > n2 ? 3 : 2;
}
