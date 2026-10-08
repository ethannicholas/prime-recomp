// Memory, CPU helper routines, timing and event scheduling.
#include "runtime.h"
#include "platform.h"
#include <chrono>
#include <mutex>
#include <queue>
#include <vector>
#include <cstdlib>
#include <cmath>

uint8_t* g_mem;
uint8_t* g_aram;
bool g_log_enabled[LOG_COUNT] = {true, true, false, true, false, false, false, false, false, false, false};

static std::mutex g_log_mutex;
static const char* kLogNames[LOG_COUNT] = {"CPU", "OS", "HW", "DVD", "GX", "VI", "SI", "EXI", "DSP", "AI", "THR"};

void log_msg(LogCat cat, const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    fprintf(stderr, "[%s] ", kLogNames[cat]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void fatal(const char* fmt, ...) {
    // Formatted once rather than printed piecewise, because the text has to reach two
    // places: stderr, and the platform's own crash record. On Android stderr is a pipe
    // into a logcat ring that the app's tracing can lap in under a minute, so a message
    // that went only there can be gone before anyone comes to read it.
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    plat_record_fatal(msg);
    fprintf(stderr, "FATAL: %s\n", msg);
    CPU* c = cpu_current();
    if (c) {
        fprintf(stderr, "  lr=%08X ctr=%08X msr=%08X\n", c->lr, c->ctr, c->msr);
        for (int i = 0; i < 32; i += 4)
            fprintf(stderr, "  r%-2d %08X %08X %08X %08X\n", i, c->r[i], c->r[i + 1], c->r[i + 2], c->r[i + 3]);
    }
    plat_backtrace_print();
    fflush(stderr);
    abort();
}

// ---------------------------------------------------------------------------
// Memory: reserve 1GB, commit RAM at 0 and locked cache at 0x20000000.
// ---------------------------------------------------------------------------
void mem_init() {
    const size_t reserve = 0x40000000;
    void* p = plat_reserve(reserve);
    if (!p) fatal("mem reserve failed");
    g_mem = (uint8_t*)p;
    if (!plat_commit(g_mem, RAM_SIZE)) fatal("mem commit failed");
    if (!plat_commit(g_mem + LC_BASE, 0x4000)) fatal("lc commit failed");
    g_aram = (uint8_t*)calloc(1, ARAM_SIZE);
}

// ---------------------------------------------------------------------------
// Indirect calls
// ---------------------------------------------------------------------------
// One slot per instruction word of the DOL's code sections, filled from the generated
// table at startup; the bounds come from the recompiler, not from here.
static std::vector<RecompFn> g_fn_table;

const char* func_name(uint32_t addr) {
    for (uint32_t i = 0; i < g_recomp_func_count; i++)
        if (g_recomp_funcs[i].addr == addr) return g_recomp_funcs[i].name;
    return "?";
}

// The function `addr` falls inside, rather than the one it starts. A backtrace is made of
// return addresses, which point into the middle of their function and so match nothing in
// func_name(); the nearest start at or below is what names them. `start` takes that start,
// so a caller can print the offset. Linear because this runs on failure paths only.
const char* func_containing(uint32_t addr, uint32_t* start) {
    uint32_t best = 0;
    const char* name = nullptr;
    for (uint32_t i = 0; i < g_recomp_func_count; i++) {
        uint32_t a = g_recomp_funcs[i].addr;
        if (a <= addr && a >= best) { best = a; name = g_recomp_funcs[i].name; }
    }
    if (start) *start = best;
    return name ? name : "?";
}

static void build_fn_table() {
    g_fn_table.assign((g_recomp_code_end - g_recomp_code_base) / 4, nullptr);
    for (uint32_t i = 0; i < g_recomp_func_count; i++) {
        uint32_t a = g_recomp_funcs[i].addr;
        if (a >= g_recomp_code_base && a < g_recomp_code_end) g_fn_table[(a - g_recomp_code_base) / 4] = g_recomp_funcs[i].fn;
    }
}

extern "C" void call_indirect(CPU* c, uint32_t addr) {
    if (addr >= g_recomp_code_base && addr < g_recomp_code_end && !(addr & 3)) {
        RecompFn fn = g_fn_table[(addr - g_recomp_code_base) / 4];
        if (fn) { fn(c); return; }
    }
    fatal("call_indirect: no function at %08X (lr=%08X)", addr, c->lr);
}

extern "C" void unimpl(CPU* c, uint32_t pc, uint32_t inst) {
    fatal("unimplemented instruction %08X at %08X", inst, pc);
}
extern "C" void hle_sc(CPU* c, uint32_t pc) {}  // PPCSync etc.
extern "C" void hle_trap(CPU* c, uint32_t pc, uint32_t inst) {
    // tw/twi: evaluate TO field
    uint32_t to = (inst >> 21) & 31, a = c->r[(inst >> 16) & 31];
    uint32_t b = (inst >> 26) == 3 ? (uint32_t)(int32_t)(int16_t)inst : c->r[(inst >> 11) & 31];
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    bool t = ((to & 16) && sa < sb) || ((to & 8) && sa > sb) || ((to & 4) && a == b) ||
             ((to & 2) && a < b) || ((to & 1) && a > b);
    if (t) fatal("trap at %08X", pc);
}
// rfi: continue at SRR0 with MSR = SRR1. The SDK uses it for two things, neither an
// exception return (exceptions are delivered and returned by the runtime, see threads.cpp):
//   * "return with a new MSR": mtsrr0 lr; mtsrr1 ...; rfi. SRR0 is the link register, so
//     this is a blr. The generated code RETs right after this call.
//   * "jump with a new MSR": __OSInitMemoryProtection's RealMode puts a *physical* code
//     address in SRR0 and rfi's to it, to run the BAT setup with translation off. Run as a
//     call: the target itself ends in the first idiom, which brings control back here, and
//     the RET after this call then returns to RealMode's caller -- which is where the
//     target's own rfi was aiming.
extern "C" void hle_rfi(CPU* c, uint32_t pc) {
    const uint32_t target = c->spr[26], msr = c->spr[27];
    c->msr = msr;
    if (target == c->lr) return;
    call_indirect(c, target | 0x80000000u);
}

extern "C" void hle_mtmsr(CPU* c, uint32_t v) { c->msr = v; }
extern "C" void hle_set_fpscr(CPU* c, uint32_t v) {
    c->fpscr = v;
    // rounding mode bits 30-31 (RN); only nearest is honoured for now
}

enum { SPR_DEC = 22, SPR_SRR0 = 26, SPR_SRR1 = 27, SPR_TBL_R = 268, SPR_TBU_R = 269,
       SPR_TBL_W = 284, SPR_TBU_W = 285, SPR_HID2 = 920, SPR_WPAR = 921, SPR_DMAU = 922, SPR_DMAL = 923 };

static int64_t g_tb_offset;
void dec_write(CPU* c, uint32_t v);
uint32_t dec_read(CPU* c);

extern "C" uint32_t hle_mfspr(CPU* c, uint32_t spr) {
    switch (spr) {
    case SPR_TBL_R: return (uint32_t)(now_ticks() + g_tb_offset);
    case SPR_TBU_R: return (uint32_t)((now_ticks() + g_tb_offset) >> 32);
    case SPR_DEC: return dec_read(c);
    // HID2 reads back as written, except DMAQL (bits 24..27): the locked-cache DMA queue
    // is always empty here, since hle_mtspr runs each transfer at once. The top four bits
    // are LSQE, WPE, PSE and LCE, and the game reads them: the THP video decoder checks
    // LCE (0x10000000) before it will decode a frame, and returns an error if it is clear,
    // which is what blanked the title and menu videos.
    case SPR_HID2: return c->spr[spr] & ~0x0F000000u;
    default: return c->spr[spr & 1023];
    }
}

extern "C" void hle_mtspr(CPU* c, uint32_t spr, uint32_t v) {
    switch (spr) {
    case SPR_TBL_W: {
        uint64_t cur = now_ticks() + g_tb_offset;
        uint64_t nv = (cur & 0xFFFFFFFF00000000ull) | v;
        g_tb_offset += (int64_t)(nv - cur);
        return;
    }
    case SPR_TBU_W: {
        uint64_t cur = now_ticks() + g_tb_offset;
        uint64_t nv = (cur & 0xFFFFFFFFull) | ((uint64_t)v << 32);
        g_tb_offset += (int64_t)(nv - cur);
        return;
    }
    case SPR_DEC: dec_write(c, v); return;
    case SPR_DMAL: {
        c->spr[spr] = v;
        if (v & 2) {  // trigger locked-cache DMA
            uint32_t dmau = c->spr[SPR_DMAU];
            uint32_t len = ((dmau & 0x1F) << 2) | ((v >> 2) & 3);
            if (len == 0) len = 128;
            uint32_t mem = dmau & ~0x1Fu, lc = v & ~0x1Fu;
            if (v & 0x10) memcpy(HOST(lc), HOST(mem), len * 32);   // load: mem -> LC
            else memcpy(HOST(mem), HOST(lc), len * 32);            // store: LC -> mem
            c->spr[spr] &= ~2u;
        }
        return;
    }
    default: c->spr[spr & 1023] = v; return;
    }
}

extern "C" void hle_dcbz(CPU* c, uint32_t ea) { memset(HOST(ea & ~31u), 0, 32); }
extern "C" void hle_dcbz_l(CPU* c, uint32_t ea) { memset(HOST(ea & ~31u), 0, 32); }
extern "C" void hle_dcbi(CPU* c, uint32_t ea) {}

extern "C" void hle_lswi(CPU* c, uint32_t ea, int rd, int n) {
    int r = (rd - 1) & 31;
    for (int i = 0; i < n; i++) {
        if ((i & 3) == 0) { r = (r + 1) & 31; c->r[r] = 0; }
        c->r[r] |= LD8(ea + i) << (24 - 8 * (i & 3));
    }
}
extern "C" void hle_stswi(CPU* c, uint32_t ea, int rs, int n) {
    int r = rs, shift = 24;
    for (int i = 0; i < n; i++) {
        ST8(ea + i, (c->r[r] >> shift) & 0xFF);
        if (shift == 0) { r = (r + 1) & 31; shift = 24; } else shift -= 8;
    }
}

// ---------------------------------------------------------------------------
// Paired-single quantized load/store
// ---------------------------------------------------------------------------
static inline double dequant(uint32_t type, int scale, uint32_t ea, int idx) {
    float s = ldexpf(1.0f, -scale);
    switch (type) {
    case 4: return (double)((float)(uint8_t)LD8(ea + idx) * s);
    case 5: return (double)((float)(uint16_t)LD16(ea + idx * 2) * s);
    case 6: return (double)((float)(int8_t)LD8(ea + idx) * s);
    case 7: return (double)((float)(int16_t)LD16(ea + idx * 2) * s);
    default: return LDF32(ea + idx * 4);
    }
}

extern "C" void psq_load(CPU* c, uint32_t ea, int frd, int w, int gqr) {
    uint32_t g = c->gqr[gqr];
    uint32_t type = (g >> 16) & 7;
    int scale = (int)((g >> 24) & 0x3F);
    if (scale & 0x20) scale -= 64;
    double p0 = dequant(type, scale, ea, 0);
    double p1 = w ? 1.0 : dequant(type, scale, ea, 1);
    c->f[frd].d = p0;
    c->ps1[frd] = p1;
}

static inline void quant_store(uint32_t type, int scale, uint32_t ea, int idx, double v) {
    float f = (float)v * ldexpf(1.0f, scale);
    auto clampi = [](float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); };
    switch (type) {
    case 4: ST8(ea + idx, (uint8_t)(int)clampi(f, 0, 255)); break;
    case 5: ST16(ea + idx * 2, (uint16_t)(int)clampi(f, 0, 65535)); break;
    case 6: ST8(ea + idx, (uint8_t)(int8_t)(int)clampi(f, -128, 127)); break;
    case 7: ST16(ea + idx * 2, (uint16_t)(int16_t)(int)clampi(f, -32768, 32767)); break;
    default: STF32(ea + idx * 4, v); break;
    }
}

extern "C" void psq_store(CPU* c, uint32_t ea, int frs, int w, int gqr) {
    uint32_t g = c->gqr[gqr];
    uint32_t type = g & 7;
    int scale = (int)((g >> 8) & 0x3F);
    if (scale & 0x20) scale -= 64;
    quant_store(type, scale, ea, 0, c->f[frs].d);
    if (!w) quant_store(type, scale, ea, 1, c->ps1[frs]);
}

// ---------------------------------------------------------------------------
// Timing and events
// ---------------------------------------------------------------------------
static std::chrono::steady_clock::time_point g_t0;

// Diagnostic only: MP_TIMESCALE=N makes the emulated timebase advance N times faster,
// so the game tries to run at N x real time. Useful for finding out how much CPU
// headroom a machine actually has -- a host that is merely keeping up at 1.0 and one
// with room to spare both report the game's capped 30 fps otherwise. It skews every
// other emulated timing (DVD, audio, retrace), so it is not a correctness path.
static double g_time_scale = 1.0;

void timing_init() {
    g_t0 = std::chrono::steady_clock::now();
    if (const char* e = getenv("MP_TIMESCALE")) {
        double s = atof(e);
        if (s > 0) g_time_scale = s;
    }
    build_fn_table();
}

uint64_t now_ticks() {
    auto d = std::chrono::steady_clock::now() - g_t0;
    return (uint64_t)((double)std::chrono::duration_cast<std::chrono::nanoseconds>(d).count() *
                      (TB_FREQ / 1e9) * g_time_scale);
}

struct Event {
    uint64_t at;
    uint64_t seq;
    EventFn fn;
    bool operator>(const Event& o) const { return at != o.at ? at > o.at : seq > o.seq; }
};
static std::mutex g_ev_mutex;
static std::priority_queue<Event, std::vector<Event>, std::greater<Event>> g_events;
static uint64_t g_ev_seq;
std::atomic<uint64_t> g_next_event_at{UINT64_MAX};

void event_schedule(uint64_t at, EventFn fn) {
    std::lock_guard<std::mutex> lk(g_ev_mutex);
    g_events.push(Event{at, g_ev_seq++, std::move(fn)});
    g_next_event_at = g_events.top().at;
}
void event_schedule_in(uint64_t delta, EventFn fn) { event_schedule(now_ticks() + delta, std::move(fn)); }

void events_run_due() {
    uint64_t now = now_ticks();
    for (;;) {
        EventFn fn;
        {
            std::lock_guard<std::mutex> lk(g_ev_mutex);
            if (g_events.empty() || g_events.top().at > now) {
                g_next_event_at = g_events.empty() ? UINT64_MAX : g_events.top().at;
                return;
            }
            fn = std::move(const_cast<Event&>(g_events.top()).fn);
            g_events.pop();
        }
        fn();
    }
}
