/* Interface between recompiled guest code and the runtime. Must stay C. */
#pragma once
#include "compat.h"
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(MP_WATCH) || defined(MP_TRACE_CALLS)
#define MP_CALL_TRACE 1
#endif

typedef union { double d; uint64_t u; } FPR;

typedef struct CPU {
    uint32_t r[32];
    FPR f[32];         /* ps0 */
    double ps1[32];
    uint8_t cr[8];     /* each field: LT=8 GT=4 EQ=2 SO=1 */
    uint8_t xer_ca, xer_so, xer_ov, xer_bc;
    uint32_t lr, ctr, msr, fpscr;
    uint32_t gqr[8];
    uint32_t spr[1024];
    void* host;        /* owning HostThread */
#ifdef MP_CALL_TRACE
    uint32_t stack[256];  /* guest call stack, innermost at depth-1 */
    uint32_t depth;
#endif
} CPU;

/* Guest call stack. Every recompiled function opens with ENTER(its own address) and
   leaves through RET(), which pops.

   Nothing in the running game reads it: the only consumer is debug_dump_threads(), from
   the fault and interrupt handlers, and whatever is asking "which guest function did
   that?" at the time. So a release build leaves it out and both macros vanish, which also
   takes 1 KB off the CPU snapshot that every context switch copies.

   Not, as it turns out, because it is slow. Benchmarked on a Quest 3 at MP_TIMESCALE=3,
   OFF/ON/OFF, the steady state was 40.60, 41.34 and 40.84 fps: the instrumented build
   measured *faster*, consistently, so a push and a pop per guest call sit below the noise
   floor and the couple of per cent between builds is code layout. It is out of release
   because it is diagnostic machinery that the running game has no use for, not to buy back
   frames -- do not go looking for them here.

   This replaces a ring of the last 256 functions *entered*. A ring cannot answer which
   function a piece of work came from: between a callee and its caller sit all the
   callee's siblings and any interrupt handler that ran, and two draws a few microseconds
   apart share almost the whole ring. A stack answers it exactly.

   The fields live in CPU, which OSSaveContext/OSLoadContext snapshot and restore beside
   the jmp_buf, so longjmping back into a parked guest thread brings the stack back with
   the C stack it belongs to. */
#ifdef MP_CALL_TRACE
static inline void wr_push(CPU* c, uint32_t fn) {
    if (c->depth < 256) c->stack[c->depth] = fn;
    c->depth++;  /* counts past the end, so deep recursion still unwinds correctly */
}
static inline void wr_pop(CPU* c) {
    if (c->depth) c->depth--;
}
#endif

#ifdef MP_WATCH
void debug_watch_check(CPU* c, uint32_t fn);
#define ENTER(addr) (wr_push(c, (addr)), debug_watch_check(c, (addr)))
#define WATCH_STORE(pc) debug_watch_check(c, (pc) | 1u)
#define EXIT() wr_pop(c)
#elif defined(MP_TRACE_CALLS)
#define ENTER(addr) wr_push(c, (addr))
#define WATCH_STORE(pc) ((void)0)
#define EXIT() wr_pop(c)
#else
#define ENTER(addr) ((void)0)
#define WATCH_STORE(pc) ((void)0)
#define EXIT() ((void)0)
#endif

/* How a recompiled function returns. */
#define RET() do { EXIT(); return; } while (0)

#define MSR_EE 0x8000u

/* ---- memory ---- */
extern uint8_t* g_mem;             /* 1GB host reservation; guest addr & 0x3FFFFFFF */
#define HOST(a) (g_mem + ((uint32_t)(a) & 0x3FFFFFFFu))
#define IS_MMIO(a) ((uint32_t)((a) - 0xC8000000u) < 0x08000000u)

uint32_t mmio_read32(uint32_t a);
uint16_t mmio_read16(uint32_t a);
uint8_t mmio_read8(uint32_t a);
void mmio_write32(uint32_t a, uint32_t v);
void mmio_write16(uint32_t a, uint16_t v);
void mmio_write8(uint32_t a, uint8_t v);

#define LIKELY(x) MP_LIKELY(x)
#define UNLIKELY(x) MP_UNLIKELY(x)

static inline uint32_t LD32(uint32_t a) {
    if (UNLIKELY(IS_MMIO(a))) return mmio_read32(a);
    uint32_t v; memcpy(&v, HOST(a), 4); return MP_BSWAP32(v);
}
static inline uint32_t LD16(uint32_t a) {
    if (UNLIKELY(IS_MMIO(a))) return mmio_read16(a);
    uint16_t v; memcpy(&v, HOST(a), 2); return MP_BSWAP16(v);
}
static inline uint32_t LDS16(uint32_t a) { return (uint32_t)(int32_t)(int16_t)LD16(a); }
static inline uint32_t LD8(uint32_t a) {
    if (UNLIKELY(IS_MMIO(a))) return mmio_read8(a);
    return *HOST(a);
}
static inline uint32_t LD32BR(uint32_t a) { return MP_BSWAP32(LD32(a)); }
static inline uint32_t LD16BR(uint32_t a) { return MP_BSWAP16((uint16_t)LD16(a)); }
static inline uint64_t LD64(uint32_t a) {
    if (UNLIKELY(IS_MMIO(a))) return ((uint64_t)mmio_read32(a) << 32) | mmio_read32(a + 4);
    uint64_t v; memcpy(&v, HOST(a), 8); return MP_BSWAP64(v);
}
static inline void ST32(uint32_t a, uint32_t v) {
    if (UNLIKELY(IS_MMIO(a))) { mmio_write32(a, v); return; }
    v = MP_BSWAP32(v); memcpy(HOST(a), &v, 4);
}
static inline void ST16(uint32_t a, uint32_t v) {
    if (UNLIKELY(IS_MMIO(a))) { mmio_write16(a, (uint16_t)v); return; }
    uint16_t h = MP_BSWAP16((uint16_t)v); memcpy(HOST(a), &h, 2);
}
static inline void ST8(uint32_t a, uint32_t v) {
    if (UNLIKELY(IS_MMIO(a))) { mmio_write8(a, (uint8_t)v); return; }
    *HOST(a) = (uint8_t)v;
}
static inline void ST32BR(uint32_t a, uint32_t v) { ST32(a, MP_BSWAP32(v)); }
static inline void ST16BR(uint32_t a, uint32_t v) { ST16(a, MP_BSWAP16((uint16_t)v)); }
static inline void ST64(uint32_t a, uint64_t v) {
    if (UNLIKELY(IS_MMIO(a))) { mmio_write32(a, (uint32_t)(v >> 32)); mmio_write32(a + 4, (uint32_t)v); return; }
    v = MP_BSWAP64(v); memcpy(HOST(a), &v, 8);
}
static inline double LDF32(uint32_t a) {
    uint32_t v = LD32(a); float f; memcpy(&f, &v, 4); return (double)f;
}
static inline void STF32(uint32_t a, double d) {
    float f = (float)d; uint32_t v; memcpy(&v, &f, 4); ST32(a, v);
}

/* ---- condition register / xer ---- */
#define CRB(n) ((c->cr[(n) >> 2] >> (3 - ((n) & 3))) & 1)
#define SET_CRB(n, v) do { int _s = 3 - ((n) & 3); \
    c->cr[(n) >> 2] = (uint8_t)((c->cr[(n) >> 2] & ~(1 << _s)) | ((!!(v)) << _s)); } while (0)
#define CMPS(a, b) ((uint8_t)(((a) < (b) ? 8 : (a) > (b) ? 4 : 2) | c->xer_so))
#define CMPU(a, b) ((uint8_t)(((uint32_t)(a) < (uint32_t)(b) ? 8 : (uint32_t)(a) > (uint32_t)(b) ? 4 : 2) | c->xer_so))
#define SET_CR0(v) (c->cr[0] = CMPS((int32_t)(v), 0))
#define SET_OV(x) do { c->xer_ov = (uint8_t)(x); c->xer_so |= c->xer_ov; } while (0)
#define ROTL(x, n) (((uint32_t)(x) << (n)) | ((uint32_t)(x) >> ((32 - (n)) & 31)))

static inline uint32_t cpu_get_cr(const CPU* c) {
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint32_t)c->cr[i] << (28 - 4 * i);
    return v;
}
static inline void cpu_set_cr(CPU* c, uint32_t v) {
    for (int i = 0; i < 8; i++) c->cr[i] = (v >> (28 - 4 * i)) & 0xF;
}
#define GET_CR() cpu_get_cr(c)
#define SET_CR(v) cpu_set_cr(c, (v))
#define GET_XER() (((uint32_t)c->xer_so << 31) | ((uint32_t)c->xer_ov << 30) | ((uint32_t)c->xer_ca << 29) | c->xer_bc)
#define SET_XER(v) do { uint32_t _v = (v); c->xer_so = _v >> 31; c->xer_ov = (_v >> 30) & 1; \
    c->xer_ca = (_v >> 29) & 1; c->xer_bc = _v & 0x7F; } while (0)

static inline uint8_t fcmp(double a, double b) {
    if (a < b) return 8;
    if (a > b) return 4;
    if (a == b) return 2;
    return 1;
}
#define FCMP(a, b) fcmp((a), (b))

static inline int32_t FCTIWZ(double d) {
    if (d != d) return INT32_MIN;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)d;
}
static inline int32_t FCTIW(double d) {
    if (d != d) return INT32_MIN;
    if (d >= 2147483647.0) return INT32_MAX;
    if (d <= -2147483648.0) return INT32_MIN;
    return (int32_t)nearbyint(d);
}

/* ---- runtime services called from generated code ---- */
typedef void (*RecompFn)(CPU*);
typedef struct { uint32_t addr; RecompFn fn; const char* name; } RecompFunc;
extern const RecompFunc g_recomp_funcs[];
extern const uint32_t g_recomp_func_count;
/* The DOL's code sections: every function the table can hold lies in [base, end). */
extern const uint32_t g_recomp_code_base, g_recomp_code_end;

void call_indirect(CPU* c, uint32_t addr);
void unimpl(CPU* c, uint32_t pc, uint32_t inst);
void hle_sc(CPU* c, uint32_t pc);
void hle_trap(CPU* c, uint32_t pc, uint32_t inst);
void hle_rfi(CPU* c, uint32_t pc);
void hle_mtmsr(CPU* c, uint32_t v);
uint32_t hle_mfspr(CPU* c, uint32_t spr);
void hle_mtspr(CPU* c, uint32_t spr, uint32_t v);
void hle_set_fpscr(CPU* c, uint32_t v);
void hle_dcbz(CPU* c, uint32_t ea);
void hle_dcbz_l(CPU* c, uint32_t ea);
void hle_dcbi(CPU* c, uint32_t ea);
void hle_lswi(CPU* c, uint32_t ea, int rd, int n);
void hle_stswi(CPU* c, uint32_t ea, int rs, int n);
void psq_load(CPU* c, uint32_t ea, int frd, int w, int gqr);
void psq_store(CPU* c, uint32_t ea, int frs, int w, int gqr);
jmp_buf* hle_context_jmpbuf(CPU* c);

extern volatile int g_irq_pending;
void irq_poll(CPU* c);
#define IRQ_CHECK() do { if (UNLIKELY(g_irq_pending)) irq_poll(c); } while (0)

#ifdef __cplusplus
}
#endif
