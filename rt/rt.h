/* Runtime for statically recompiled Win32 x86 code */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

typedef struct {
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t df;
} regs_t;
extern regs_t R;

typedef struct {
    double st[8];
    int top;
    uint16_t cw, sw;
} fpu_t;
extern fpu_t F;

extern uint8_t *MEM;

#define GUEST_MEM_SIZE   0x4000000u     /* 64 MB guest address space */
#define TEB_ADDR         0x00010000u
#define FS_BASE          TEB_ADDR
#define STACK_LO         0x00100000u
#define STACK_TOP        0x003ff000u
#define THUNK_BASE       0xfff00000u

typedef struct { uint32_t addr; void (*fn)(void); } guest_func_entry;
typedef struct { uint32_t iat; const char *dll; const char *name; void (*fn)(void); } guest_import_entry;

static inline uint8_t  ld8 (const uint8_t *m, uint32_t a) { return m[a]; }
static inline uint16_t ld16(const uint8_t *m, uint32_t a) { uint16_t v; memcpy(&v, m + a, 2); return v; }
static inline uint32_t ld32(const uint8_t *m, uint32_t a) { uint32_t v; memcpy(&v, m + a, 4); return v; }
static inline uint64_t ld64(const uint8_t *m, uint32_t a) { uint64_t v; memcpy(&v, m + a, 8); return v; }
static inline void st8 (uint8_t *m, uint32_t a, uint8_t v)  { m[a] = v; }
static inline void st16(uint8_t *m, uint32_t a, uint16_t v) { memcpy(m + a, &v, 2); }
static inline void st32(uint8_t *m, uint32_t a, uint32_t v) { memcpy(m + a, &v, 4); }
static inline void st64(uint8_t *m, uint32_t a, uint64_t v) { memcpy(m + a, &v, 8); }

double f80_to_double(const uint8_t *p);
void double_to_f80(double d, uint8_t *p);

#ifdef GENCODE
#define rd8(a)  ld8(M, (a))
#define rd16(a) ld16(M, (a))
#define rd32(a) ld32(M, (a))
#define rd64(a) ld64(M, (a))
#define wr8(a, v)  st8(M, (a), (uint8_t)(v))
#define wr16(a, v) st16(M, (a), (uint16_t)(v))
#define wr32(a, v) st32(M, (a), (uint32_t)(v))
#define wr64(a, v) st64(M, (a), (uint64_t)(v))
#else
#define rd8(a)  ld8(MEM, (a))
#define rd16(a) ld16(MEM, (a))
#define rd32(a) ld32(MEM, (a))
#define rd64(a) ld64(MEM, (a))
#define wr8(a, v)  st8(MEM, (a), (uint8_t)(v))
#define wr16(a, v) st16(MEM, (a), (uint16_t)(v))
#define wr32(a, v) st32(MEM, (a), (uint32_t)(v))
#define wr64(a, v) st64(MEM, (a), (uint64_t)(v))
#endif

static inline float  rdf32_(const uint8_t *m, uint32_t a) { float f; memcpy(&f, m + a, 4); return f; }
static inline double rdf64_(const uint8_t *m, uint32_t a) { double f; memcpy(&f, m + a, 8); return f; }
static inline void wrf32_(uint8_t *m, uint32_t a, double d) { float f = (float)d; memcpy(m + a, &f, 4); }
static inline void wrf64_(uint8_t *m, uint32_t a, double d) { memcpy(m + a, &d, 8); }
#ifdef GENCODE
#define rdf32(a) ((double)rdf32_(M, (a)))
#define rdf64(a) rdf64_(M, (a))
#define rdf80(a) f80_to_double(M + (a))
#define wrf32(a, v) wrf32_(M, (a), (v))
#define wrf64(a, v) wrf64_(M, (a), (v))
#define wrf80(a, v) double_to_f80((v), M + (a))
#endif

#define PUSH32(v) do { uint32_t _pv = (uint32_t)(v); esp -= 4; wr32(esp, _pv); } while (0)
#define SYNC_OUT do { R.eax = eax; R.ecx = ecx; R.edx = edx; R.ebx = ebx; R.esp = esp; R.ebp = ebp; R.esi = esi; R.edi = edi; } while (0)
#define SYNC_IN  do { eax = R.eax; ecx = R.ecx; edx = R.edx; ebx = R.ebx; esp = R.esp; ebp = R.ebp; esi = R.esi; edi = R.edi; } while (0)

static inline bool parity8(uint8_t v) { return !__builtin_parity(v); }

/* FPU */
#define FST(i) F.st[(F.top + (i)) & 7]
#define FPUSH(v) do { double _fv = (v); F.top = (F.top - 1) & 7; F.st[F.top] = _fv; } while (0)
#define FPOP() (F.top = (F.top + 1) & 7)
void fpu_com(double a, double b);
void fpu_xam(void);
void fpu_init(void);
uint16_t fpu_sw(void);
double fpu_round(double v);
int32_t fpu_toint32(double v);
int16_t fpu_toint16(double v);
int64_t fpu_toint64(double v);
int32_t fpu_trunc32(double v);
void fpu_ldenv(uint32_t a);
void fpu_stenv(uint32_t a);
void fpu_fsave(uint32_t a);
void fpu_frstor(uint32_t a);

/* string ops: operate on R */
void str_movs1(int rep); void str_movs2(int rep); void str_movs4(int rep);
void str_stos1(int rep); void str_stos2(int rep); void str_stos4(int rep);
void str_lods1(int rep); void str_lods2(int rep); void str_lods4(int rep);
int str_scas1(int rep, uint32_t *a, uint32_t *b); int str_scas2(int rep, uint32_t *a, uint32_t *b); int str_scas4(int rep, uint32_t *a, uint32_t *b);
int str_cmps1(int rep, uint32_t *a, uint32_t *b); int str_cmps2(int rep, uint32_t *a, uint32_t *b); int str_cmps4(int rep, uint32_t *a, uint32_t *b);

/* control */
void call_ind(uint32_t addr);
void guest_fault(uint32_t addr, const char *msg);
uint32_t guest_call(uint32_t addr, int nargs, const uint32_t *args);

/* shim helpers */
#define ARG(k) rd32(R.esp + 4 + 4 * (k))
#define SHIM(dll, name, nargs) \
    static uint32_t sh_##dll##_##name(void); \
    void imp_##dll##_##name(void) { if (rt_trace) rt_log("[%08x] " #name "(%x,%x,%x,%x)\n", rd32(R.esp), ARG(0), ARG(1), ARG(2), ARG(3)); uint32_t _r = sh_##dll##_##name(); R.eax = _r; R.esp += 4 + 4 * (nargs); } \
    static uint32_t sh_##dll##_##name(void)

/* COM / thunks */
typedef uint32_t (*host_method)(void);   /* reads args from guest stack, returns eax; nargs incl. this */
uint32_t make_thunk(void (*fn)(void));
uint32_t make_vtable(void (**fns)(void), int n);

void rt_log(const char *fmt, ...);
void rt_spin_poll(void);
extern int rt_trace;
