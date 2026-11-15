/* core runtime: memory, loader, dispatch, fpu, string ops */
#include "rt.h"
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

regs_t R;
int rt_trace;
fpu_t F;
uint8_t *MEM;

static void (**disp_tab)(void);
static uint32_t disp_lo, disp_hi;

#define MAX_THUNKS 4096
static void (*thunks[MAX_THUNKS])(void);
static int nthunks;

const guest_func_entry *g_funcs;
const guest_import_entry *g_imports;

void rt_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    host_vlog(fmt, ap);
    va_end(ap);
}

void guest_fault(uint32_t addr, const char *msg) {
    rt_log("GUEST FAULT at %08x: %s  (eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x)\n",
           addr, msg, R.eax, R.ecx, R.edx, R.ebx, R.esp, R.ebp, R.esi, R.edi);
    /* stack dump */
    for (int k = 0; k < 16; k++) rt_log("  [esp+%02x] %08x\n", k * 4, ld32(MEM, R.esp + 4 * k));
    host_fatal(msg);
}

uint32_t make_thunk(void (*fn)(void)) {
    if (nthunks >= MAX_THUNKS) host_fatal("out of thunks");
    thunks[nthunks] = fn;
    return THUNK_BASE + 4 * (nthunks++);
}

/* allocate guest memory for a vtable of thunks */
uint32_t guest_alloc(uint32_t size);
uint32_t make_vtable(void (**fns)(void), int n) {
    uint32_t vt = guest_alloc(4 * n);
    for (int k = 0; k < n; k++) wr32(vt + 4 * k, fns[k] ? make_thunk(fns[k]) : 0);
    return vt;
}

void call_ind(uint32_t a) {
    if (a >= disp_lo && a < disp_hi) {
        void (*f)(void) = disp_tab[a - disp_lo];
        if (f) { f(); return; }
    } else if (a >= THUNK_BASE) {
        uint32_t k = (a - THUNK_BASE) / 4;
        if (k < (uint32_t)nthunks) { thunks[k](); return; }
    }
    char buf[64];
    snprintf(buf, sizeof buf, "indirect call to unknown %08x", a);
    guest_fault(a, buf);
}

#define RET_MAGIC 0xffffeff0u
uint32_t guest_call(uint32_t addr, int nargs, const uint32_t *args) {
    uint32_t saved = R.esp;
    for (int k = nargs - 1; k >= 0; k--) { R.esp -= 4; wr32(R.esp, args[k]); }
    R.esp -= 4; wr32(R.esp, RET_MAGIC);
    call_ind(addr);
    R.esp = saved;
    return R.eax;
}

/* ------------- simple guest heap (first-fit free list) ------------- */
static uint32_t heap_lo, heap_hi, heap_brk;
typedef struct { uint32_t size, free; } hblk;   /* header stored in guest memory, 16 bytes */
#define HDR 16u

void heap_init(uint32_t lo, uint32_t hi) { heap_lo = heap_brk = (lo + 15) & ~15u; heap_hi = hi; }

uint32_t guest_alloc(uint32_t size) {
    size = (size + 15) & ~15u;
    if (size == 0) size = 16;
    /* first fit over existing blocks */
    uint32_t p = heap_lo;
    while (p < heap_brk) {
        uint32_t bs = rd32(p), fr = rd32(p + 4);
        if (fr && bs >= size) {
            if (bs >= size + HDR + 32) {
                uint32_t q = p + HDR + size;
                wr32(q, bs - size - HDR); wr32(q + 4, 1);
                wr32(p, size);
            }
            wr32(p + 4, 0);
            memset(MEM + p + HDR, 0, rd32(p));
            return p + HDR;
        }
        p += HDR + bs;
    }
    if (heap_brk + HDR + size > heap_hi) {
        rt_log("guest heap exhausted (want %u)\n", size);
        return 0;
    }
    p = heap_brk;
    wr32(p, size); wr32(p + 4, 0);
    heap_brk += HDR + size;
    { static uint32_t pk; if (heap_brk > pk + 0x100000) { pk = heap_brk; rt_log("heap brk %08x\n", heap_brk); } }
    memset(MEM + p + HDR, 0, size);
    return p + HDR;
}

uint32_t heap_peak(void) { return heap_brk; }
uint32_t guest_size(uint32_t a) { return a ? rd32(a - HDR) : 0; }

void guest_free(uint32_t a) {
    if (!a) return;
    uint32_t p = a - HDR;
    wr32(p + 4, 1);
    /* coalesce forward */
    for (;;) {
        uint32_t q = p + HDR + rd32(p);
        if (q >= heap_brk || !rd32(q + 4)) break;
        wr32(p, rd32(p) + HDR + rd32(q));
    }
    if (p + HDR + rd32(p) == heap_brk) heap_brk = p;
}

uint32_t guest_realloc(uint32_t a, uint32_t size) {
    if (!a) return guest_alloc(size);
    uint32_t old = guest_size(a);
    if (old >= size) return a;
    uint32_t n = guest_alloc(size);
    if (!n) return 0;
    memcpy(MEM + n, MEM + a, old);
    guest_free(a);
    return n;
}

/* ------------- loader ------------- */
static uint32_t img_base, img_size;

int load_pe(const char *path, uint32_t *entry) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *f = malloc(len);
    if (fread(f, 1, len, fp) != (size_t)len) { fclose(fp); free(f); return -2; }
    fclose(fp);
    uint32_t pe = *(uint32_t *)(f + 0x3c);
    uint16_t nsec = *(uint16_t *)(f + pe + 6);
    uint16_t optsz = *(uint16_t *)(f + pe + 20);
    uint8_t *opt = f + pe + 24;
    img_base = *(uint32_t *)(opt + 28);
    img_size = *(uint32_t *)(opt + 56);
    uint32_t hdrsz = *(uint32_t *)(opt + 60);
    *entry = img_base + *(uint32_t *)(opt + 16);
    if (img_base + img_size > GUEST_MEM_SIZE) return -3;
    memcpy(MEM + img_base, f, hdrsz < (uint32_t)len ? hdrsz : (uint32_t)len);
    uint8_t *sec = opt + optsz;
    for (int k = 0; k < nsec; k++, sec += 40) {
        uint32_t vsz = *(uint32_t *)(sec + 8), va = *(uint32_t *)(sec + 12);
        uint32_t rsz = *(uint32_t *)(sec + 16), roff = *(uint32_t *)(sec + 20);
        uint32_t n = rsz < vsz || vsz == 0 ? rsz : vsz;
        if (roff + n > (uint32_t)len) n = len - roff;
        memcpy(MEM + img_base + va, f + roff, n);
    }
    free(f);
    return 0;
}

uint32_t image_end(void) { return img_base + img_size; }

void rt_init(const guest_func_entry *funcs, const guest_import_entry *imps) {
    g_funcs = funcs; g_imports = imps;
    disp_lo = 0xffffffff; disp_hi = 0;
    for (const guest_func_entry *e = funcs; e->addr; e++) {
        if (e->addr < disp_lo) disp_lo = e->addr;
        if (e->addr + 1 > disp_hi) disp_hi = e->addr + 1;
    }
    disp_tab = calloc(disp_hi - disp_lo, sizeof(*disp_tab));
    for (const guest_func_entry *e = funcs; e->addr; e++) disp_tab[e->addr - disp_lo] = e->fn;
    for (const guest_import_entry *e = imps; e->iat; e++) wr32(e->iat, make_thunk(e->fn));
    /* TEB */
    wr32(TEB_ADDR + 0, 0xffffffffu);
    wr32(TEB_ADDR + 4, STACK_TOP);
    wr32(TEB_ADDR + 8, STACK_LO);
    wr32(TEB_ADDR + 0x18, TEB_ADDR);
    wr32(TEB_ADDR + 0x20, 0x100); /* pid */
    wr32(TEB_ADDR + 0x24, 0x104); /* tid */
    R.esp = STACK_TOP;
    fpu_init();
}

/* lookup an import shim by name (used for GetProcAddress) */
uint32_t import_thunk_by_name(const char *name) {
    for (const guest_import_entry *e = g_imports; e->iat; e++)
        if (!strcmp(e->name, name)) return rd32(e->iat);
    return 0;
}

/* ------------- FPU ------------- */
void fpu_init(void) { F.top = 0; F.cw = 0x27f; F.sw = 0; }
uint16_t fpu_sw(void) { return (F.sw & ~0x3800) | ((F.top & 7) << 11); }
void fpu_com(double a, double b) {
    F.sw &= ~0x4500;
    if (isnan(a) || isnan(b)) F.sw |= 0x4500;
    else if (a < b) F.sw |= 0x0100;
    else if (a == b) F.sw |= 0x4000;
}
void fpu_xam(void) {
    double v = FST(0);
    F.sw &= ~0x4700;
    if (signbit(v)) F.sw |= 0x200;
    if (isnan(v)) F.sw |= 0x0100;
    else if (isinf(v)) F.sw |= 0x0500;
    else if (v == 0) F.sw |= 0x4000;
    else if (fpclassify(v) == FP_SUBNORMAL) F.sw |= 0x4400;
    else F.sw |= 0x0400;
}
double fpu_round(double v) {
    switch ((F.cw >> 10) & 3) {
    case 0: return nearbyint(v);   /* host default: round to nearest even */
    case 1: return floor(v);
    case 2: return ceil(v);
    default: return trunc(v);
    }
}
int32_t fpu_toint32(double v) {
    double r = fpu_round(v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return (int32_t)0x80000000;
    return (int32_t)r;
}
int32_t fpu_trunc32(double v) {
    double r = trunc(v);
    if (!(r >= -2147483648.0 && r <= 2147483647.0)) return (int32_t)0x80000000;
    return (int32_t)r;
}
int16_t fpu_toint16(double v) {
    double r = fpu_round(v);
    if (!(r >= -32768.0 && r <= 32767.0)) return (int16_t)0x8000;
    return (int16_t)r;
}
int64_t fpu_toint64(double v) {
    double r = fpu_round(v);
    if (!(r >= -9223372036854775808.0 && r < 9223372036854775808.0)) return (int64_t)0x8000000000000000ULL;
    return (int64_t)r;
}
void fpu_ldenv(uint32_t a) { F.cw = rd16(a); F.sw = rd16(a + 4); F.top = (F.sw >> 11) & 7; }
void fpu_stenv(uint32_t a) { memset(MEM + a, 0, 28); wr16(a, F.cw); wr16(a + 4, fpu_sw()); wr16(a + 8, 0xffff); }
void fpu_fsave(uint32_t a) {
    fpu_stenv(a);
    for (int k = 0; k < 8; k++) double_to_f80(FST(k), MEM + a + 28 + 10 * k);
    fpu_init();
}
void fpu_frstor(uint32_t a) {
    fpu_ldenv(a);
    for (int k = 0; k < 8; k++) FST(k) = f80_to_double(MEM + a + 28 + 10 * k);
}

double f80_to_double(const uint8_t *p) {
    uint64_t m; memcpy(&m, p, 8);
    uint16_t se = p[8] | (p[9] << 8);
    int s = se >> 15, e = se & 0x7fff;
    double r;
    if (e == 0 && m == 0) r = 0;
    else if (e == 0x7fff) r = (m << 1) ? NAN : INFINITY;
    else r = ldexp((double)m, e - 16383 - 63);
    return s ? -r : r;
}
void double_to_f80(double d, uint8_t *p) {
    uint64_t m = 0; int e = 0, s = signbit(d) ? 1 : 0;
    if (d == 0) { e = 0; m = 0; }
    else if (isinf(d)) { e = 0x7fff; m = 0x8000000000000000ULL; }
    else if (isnan(d)) { e = 0x7fff; m = 0xc000000000000000ULL; }
    else {
        int ex; double fr = frexp(fabs(d), &ex);   /* fr in [0.5,1) */
        m = (uint64_t)ldexp(fr, 64);
        e = ex - 1 + 16383;
    }
    memcpy(p, &m, 8);
    uint16_t se = (uint16_t)((s << 15) | e);
    p[8] = se & 0xff; p[9] = se >> 8;
}

/* ------------- string ops ------------- */
#define DSTEP(n) (R.df ? -(n) : (n))
#define STR_MOV(N, T, LD, ST) \
void str_movs##N(int rep) { \
    if (rep) { \
        uint32_t c = R.ecx; \
        if (!R.df && c) { \
            uint32_t s = R.esi, d = R.edi, bytes = c * N; \
            if (d <= s || d >= s + bytes) memmove(MEM + d, MEM + s, bytes); \
            else { for (uint32_t k = 0; k < c; k++) ST(MEM, d + k * N, LD(MEM, s + k * N)); } \
            R.esi += bytes; R.edi += bytes; R.ecx = 0; return; } \
        while (R.ecx) { ST(MEM, R.edi, LD(MEM, R.esi)); R.esi += DSTEP(N); R.edi += DSTEP(N); R.ecx--; } \
    } else { ST(MEM, R.edi, LD(MEM, R.esi)); R.esi += DSTEP(N); R.edi += DSTEP(N); } } \
void str_stos##N(int rep) { \
    T v = (T)R.eax; \
    if (rep) { \
        if (!R.df && N == 1) { memset(MEM + R.edi, (uint8_t)v, R.ecx); R.edi += R.ecx; R.ecx = 0; return; } \
        while (R.ecx) { ST(MEM, R.edi, v); R.edi += DSTEP(N); R.ecx--; } \
    } else { ST(MEM, R.edi, v); R.edi += DSTEP(N); } } \
void str_lods##N(int rep) { \
    do { T v = LD(MEM, R.esi); R.esi += DSTEP(N); \
         if (N == 4) R.eax = v; else if (N == 2) R.eax = (R.eax & 0xffff0000u) | v; else R.eax = (R.eax & 0xffffff00u) | v; \
         if (rep) R.ecx--; } while (rep && R.ecx); } \
int str_scas##N(int rep, uint32_t *a, uint32_t *b) { \
    T av = (T)R.eax; \
    if (!rep) { *a = av; *b = LD(MEM, R.edi); R.edi += DSTEP(N); return 1; } \
    if (!R.ecx) return 0; \
    for (;;) { T bv = LD(MEM, R.edi); R.edi += DSTEP(N); R.ecx--; *a = av; *b = bv; \
        if (rep == 1 && av != bv) break; if (rep == 2 && av == bv) break; if (!R.ecx) break; } \
    return 1; } \
int str_cmps##N(int rep, uint32_t *a, uint32_t *b) { \
    if (!rep) { *a = LD(MEM, R.esi); *b = LD(MEM, R.edi); R.esi += DSTEP(N); R.edi += DSTEP(N); return 1; } \
    if (!R.ecx) return 0; \
    for (;;) { T av = LD(MEM, R.esi), bv = LD(MEM, R.edi); R.esi += DSTEP(N); R.edi += DSTEP(N); R.ecx--; *a = av; *b = bv; \
        if (rep == 1 && av != bv) break; if (rep == 2 && av == bv) break; if (!R.ecx) break; } \
    return 1; }

STR_MOV(1, uint8_t, ld8, st8)
STR_MOV(2, uint16_t, ld16, st16)
STR_MOV(4, uint32_t, ld32, st32)

/* called on backward branches of loops that neither call nor store: lets
   timer callbacks (which on Windows run on their own thread) make progress */
void winmm_service(void);
void win_pump_host(void);
void rt_spin_poll(void) {
    static uint32_t n, last;
    if ((++n & 255) == 0) {
        uint32_t now = host_ticks_ms();
        if (now != last) { last = now; win_pump_host(); }
        else host_sleep_ms(1);
    }
}
