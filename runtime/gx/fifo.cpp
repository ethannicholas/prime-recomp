// Command Processor / Pixel Engine registers, write-gather pipe and FIFO feeding.
#include "../runtime.h"
#include "gx.h"
#include <vector>

uint32_t pi_fifo_base();
uint32_t pi_fifo_end();
uint32_t& pi_fifo_wptr();

static uint16_t g_cp_sr, g_cp_cr, g_cp_clear;
static uint32_t g_cp_base, g_cp_end, g_cp_hiwm, g_cp_lowm, g_cp_rwdist, g_cp_wptr, g_cp_rptr, g_cp_bp;
static uint16_t g_pe_ctrl, g_pe_token;
static uint16_t g_pe_regs[0x20];

static uint8_t g_gp_buf[64];
static uint32_t g_gp_len;
static std::vector<uint8_t> g_pending;  // unconsumed partial command bytes

// CP control: bit1 = breakpoint enable, bit5 = breakpoint interrupt enable (Dolphin UCPCtrlReg)
enum { CP_CR_READ = 1, CP_CR_BPEN = 2, CP_CR_OVFINT = 4, CP_CR_UNFINT = 8, CP_CR_LINK = 16, CP_CR_BPINT = 32 };
enum { PE_TOKEN_EN = 1, PE_FINISH_EN = 2, PE_TOKEN_INT = 4, PE_FINISH_INT = 8 };

static void pe_update_irq() {
    pi_set_interrupt(INT_PE_TOKEN, (g_pe_ctrl & PE_TOKEN_INT) && (g_pe_ctrl & PE_TOKEN_EN));
    pi_set_interrupt(INT_PE_FINISH, (g_pe_ctrl & PE_FINISH_INT) && (g_pe_ctrl & PE_FINISH_EN));
}

void pe_signal_token(uint16_t token, bool interrupt) {
    if (getenv("MP_TOKLOG")) fprintf(stderr, "[tok] %04X%s\n", token, interrupt ? " int" : "");
    g_pe_token = token;
    if (interrupt) { g_pe_ctrl |= PE_TOKEN_INT; pe_update_irq(); }
}
void pe_signal_finish() {
    g_pe_ctrl |= PE_FINISH_INT;
    pe_update_irq();
}

void cp_init() {}

// Feed bytes to the command processor, handling commands split across chunks.
static void cp_feed(const uint8_t* data, uint32_t len) {
    if (g_pending.empty()) {
        uint32_t used = gx::process(data, len, true);
        if (used < len) g_pending.assign(data + used, data + len);
        return;
    }
    g_pending.insert(g_pending.end(), data, data + len);
    uint32_t used = gx::process(g_pending.data(), (uint32_t)g_pending.size(), true);
    g_pending.erase(g_pending.begin(), g_pending.begin() + used);
    static size_t warned = 1 << 16;
    if (g_pending.size() > warned) {
        warned *= 4;
        LOG(LOG_GX, "FIFO parser waiting on a %zu-byte partial command (first bytes %02X %02X %02X %02X)", g_pending.size(),
            g_pending[0], g_pending[1], g_pending[2], g_pending[3]);
    }
}

// Consume everything between the CP read pointer and the CPU write pointer (the GP
// FIFO lives in RAM; data written while reads are disabled is processed later).
static void cp_update_irq() {
    bool bp_irq = (g_cp_sr & 0x10) && (g_cp_cr & CP_CR_BPINT);
    pi_set_interrupt(INT_CP, bp_irq);
}

// Consume everything between the CP read pointer and the CPU write pointer (the GP
// FIFO lives in RAM; data written while reads are disabled is processed later).
// Honors the GP breakpoint, which games use to hold the GPU back.
static void cp_drain() {
    if (!(g_cp_cr & CP_CR_READ) || !(g_cp_cr & CP_CR_LINK)) return;
    uint32_t base = g_cp_base & 0x03FFFFE0, end = g_cp_end & 0x03FFFFE0;
    uint32_t wptr = pi_fifo_wptr() & 0x03FFFFE0;
    if (!end || end <= base) return;
    uint32_t r = g_cp_rptr & 0x03FFFFE0;
    if (r < base || r >= end) r = base;
    bool bp_on = g_cp_cr & CP_CR_BPEN;
    uint32_t bp = g_cp_bp & 0x03FFFFE0;
    // The GP halts whenever its read pointer sits at an enabled breakpoint; moving or
    // disabling the breakpoint lets it continue. Games use this to keep the GPU one
    // frame behind the CPU.
    for (int guard = 0; r != wptr && guard < 4; guard++) {
        if (bp_on && r == bp) break;
        uint32_t stop = wptr > r ? wptr : end;
        if (bp_on && bp > r && bp < stop) stop = bp;
        if (stop > r) cp_feed(phys_ptr(r), stop - r);
        r = stop >= end ? base : stop;
    }
    bool hit = bp_on && r == bp;
    if (hit) g_cp_sr |= 0x10; else g_cp_sr &= ~0x10;
    cp_update_irq();
    g_cp_rptr = r;
    g_cp_wptr = wptr;
    g_cp_rwdist = wptr >= r ? wptr - r : (end - r) + (wptr - base);
}

static void gp_flush32() {
    uint32_t& wptr = pi_fifo_wptr();
    uint32_t addr = wptr & 0x03FFFFFF;
    memcpy(phys_ptr(addr), g_gp_buf, dma_fit("write-gather", addr, 32));
    addr += 32;
    if (addr >= pi_fifo_end()) {
        addr = pi_fifo_base();
        wptr = (wptr ^ 0x04000000) & 0x04000000;  // toggle wrap bit
        wptr |= addr;
    } else {
        wptr = (wptr & 0x04000000) | addr;
    }
    cp_drain();
    memmove(g_gp_buf, g_gp_buf + 32, g_gp_len - 32);
    g_gp_len -= 32;
}

void gp_write8(uint8_t v) {
    g_gp_buf[g_gp_len++] = v;
    if (g_gp_len >= 32) gp_flush32();
}
void gp_write16(uint16_t v) {
    g_gp_buf[g_gp_len++] = v >> 8;
    g_gp_buf[g_gp_len++] = (uint8_t)v;
    if (g_gp_len >= 32) gp_flush32();
}
#ifdef MP_CALL_TRACE
const char* func_name(uint32_t addr);
// MP_GP_STACK=<hex word> prints the guest call stack the first few times that word is
// pushed into the write-gather pipe. Unlike the drain side, the guest is still inside the
// code that wanted the command here, so this names it. Needs MP_TRACE_CALLS.
static void gp_stack_probe(uint32_t v) {
    static const char* want = getenv("MP_GP_STACK");
    if (!want) return;
    static const uint32_t w = (uint32_t)strtoul(want, nullptr, 16);
    if (v != w) return;
    static int shown;
    if (shown++ >= 3) return;
    CPU* c = cpu_current();
    fprintf(stderr, "[gp] word %08X written; guest call stack innermost first:\n", v);
    const uint32_t have = c && c->depth < 256 ? c->depth : 0;
    for (uint32_t i = 1; i <= have; i++)
        fprintf(stderr, "[gp]   %08X %s\n", c->stack[have - i], func_name(c->stack[have - i]));
}
#endif

void gp_write32(uint32_t v) {
#ifdef MP_CALL_TRACE
    gp_stack_probe(v);
#endif
    g_gp_buf[g_gp_len++] = v >> 24;
    g_gp_buf[g_gp_len++] = (uint8_t)(v >> 16);
    g_gp_buf[g_gp_len++] = (uint8_t)(v >> 8);
    g_gp_buf[g_gp_len++] = (uint8_t)v;
    if (g_gp_len >= 32) gp_flush32();
}

static uint16_t lo(uint32_t v) { return (uint16_t)v; }
static uint16_t hi(uint32_t v) { return (uint16_t)(v >> 16); }
static void set_lo(uint32_t& r, uint16_t v) { r = (r & 0xFFFF0000u) | v; }
static void set_hi(uint32_t& r, uint16_t v) { r = (r & 0xFFFFu) | ((uint32_t)v << 16); }

uint16_t cp_read16(uint32_t off) {
    switch (off & 0xFF) {
    case 0x00: return 0x0C | (g_cp_sr & 0x13);  // GP read idle + command idle
    case 0x02: return g_cp_cr;
    case 0x04: return g_cp_clear;
    case 0x20: return lo(g_cp_base); case 0x22: return hi(g_cp_base);
    case 0x24: return lo(g_cp_end); case 0x26: return hi(g_cp_end);
    case 0x28: return lo(g_cp_hiwm); case 0x2A: return hi(g_cp_hiwm);
    case 0x2C: return lo(g_cp_lowm); case 0x2E: return hi(g_cp_lowm);
    case 0x30: return lo(g_cp_rwdist); case 0x32: return hi(g_cp_rwdist);
    case 0x34: return lo(g_cp_wptr); case 0x36: return hi(g_cp_wptr);
    case 0x38: return lo(g_cp_rptr); case 0x3A: return hi(g_cp_rptr);
    case 0x3C: return lo(g_cp_bp); case 0x3E: return hi(g_cp_bp);
    }
    return 0;  // perf counters etc.
}

void cp_write16(uint32_t off, uint16_t v) {
    switch (off & 0xFF) {
    case 0x00: g_cp_sr = v; break;
    case 0x02:
        if ((v ^ g_cp_cr) & ~1u) LOG(LOG_GX, "CP_CR %04X -> %04X", g_cp_cr, v);
        g_cp_cr = v;
        if (!(v & CP_CR_BPEN)) g_cp_sr &= ~0x10;
        cp_update_irq();
        cp_drain();
        break;
    case 0x04: g_cp_clear = v; g_cp_sr &= ~(v & 3); break;
    case 0x20: set_lo(g_cp_base, v & ~0x1F); LOG(LOG_GX, "CP base lo %04X", v); break; case 0x22: set_hi(g_cp_base, v & 0x3FF); break;
    case 0x24: set_lo(g_cp_end, v & ~0x1F); break; case 0x26: set_hi(g_cp_end, v & 0x3FF); break;
    case 0x28: set_lo(g_cp_hiwm, v); break; case 0x2A: set_hi(g_cp_hiwm, v); break;
    case 0x2C: set_lo(g_cp_lowm, v); break; case 0x2E: set_hi(g_cp_lowm, v); break;
    case 0x30: set_lo(g_cp_rwdist, v); break; case 0x32: set_hi(g_cp_rwdist, v); break;
    case 0x34: set_lo(g_cp_wptr, v); break; case 0x36: set_hi(g_cp_wptr, v); break;
    case 0x38: set_lo(g_cp_rptr, v); break; case 0x3A: set_hi(g_cp_rptr, v); break;
    case 0x3C: set_lo(g_cp_bp, v); cp_drain(); break;
    case 0x3E: set_hi(g_cp_bp, v); cp_drain(); break;
    }
}

uint16_t pe_read16(uint32_t off) {
    switch (off & 0xFF) {
    case 0x0A: return g_pe_ctrl;
    case 0x0E: return g_pe_token;
    }
    return g_pe_regs[(off & 0x3F) >> 1];
}

void pe_write16(uint32_t off, uint16_t v) {
    switch (off & 0xFF) {
    case 0x0A:
        g_pe_ctrl = (uint16_t)((g_pe_ctrl & (PE_TOKEN_INT | PE_FINISH_INT)) & ~(v & (PE_TOKEN_INT | PE_FINISH_INT))) |
                    (v & (PE_TOKEN_EN | PE_FINISH_EN));
        pe_update_irq();
        return;
    case 0x0E: g_pe_token = v; return;
    }
    g_pe_regs[(off & 0x3F) >> 1] = v;
}

MP_WEAK uint32_t efb_peek(uint32_t addr) { return 0; }
MP_WEAK void efb_poke(uint32_t addr, uint32_t v) {}

void debug_dump_fifo() {
    fprintf(stderr, "FIFO: CP cr=%04X sr=%04X base=%08X end=%08X wptr=%08X rptr=%08X rwdist=%08X bp=%08X | PI base=%08X end=%08X wptr=%08X | pending=%zu gp=%u\n",
            g_cp_cr, g_cp_sr, g_cp_base, g_cp_end, g_cp_wptr, g_cp_rptr, g_cp_rwdist, g_cp_bp, pi_fifo_base(), pi_fifo_end(), pi_fifo_wptr(),
            g_pending.size(), g_gp_len);
    fprintf(stderr, "  PE token=%04X ctrl=%04X\n", g_pe_token, g_pe_ctrl);
    {
        uint32_t w = pi_fifo_wptr() & 0x03FFFFE0;
        fprintf(stderr, "  RAM FIFO before wptr:");
        for (uint32_t a = w - 256; a < w; a++) { if ((a & 31) == 0) fprintf(stderr, "\n   %08X:", a); fprintf(stderr, " %02X", *phys_ptr(a)); }
        fprintf(stderr, "\n  gp:");
        for (uint32_t i = 0; i < g_gp_len; i++) fprintf(stderr, " %02X", g_gp_buf[i]);
        fprintf(stderr, "\n");
    }
    for (uint32_t a = 0; a < 0x01800000 - 5; a++) {
        const uint8_t* q = phys_ptr(a);
        if (q[0] == 0x61 && (q[1] == 0x47 || q[1] == 0x48) && q[2] == 0x00 && q[3] == 0xB0 && (q[4] == 0x04 || q[4] == 0x05))
            fprintf(stderr, "  token write found at %08X: %02X %02X%02X%02X\n", a, q[1], q[2], q[3], q[4]);
    }
    fprintf(stderr, "  pending:");
    for (size_t i = 0; i < g_pending.size() && i < 64; i++) fprintf(stderr, " %02X", g_pending[i]);
    fprintf(stderr, "\n  vtx size for vat%d = %u\n", g_pending.empty() ? 0 : g_pending[0] & 7, g_pending.empty() ? 0 : gx::vertex_size(g_pending[0] & 7));
    fprintf(stderr, "  VCD lo=%08X hi=%08X VAT%d A=%08X B=%08X C=%08X\n", gx::g_state.cp[0x50], gx::g_state.cp[0x60], g_pending.empty() ? 0 : g_pending[0] & 7,
            gx::g_state.cp[0x70 + (g_pending.empty() ? 0 : g_pending[0] & 7)], gx::g_state.cp[0x80 + (g_pending.empty() ? 0 : g_pending[0] & 7)],
            gx::g_state.cp[0x90 + (g_pending.empty() ? 0 : g_pending[0] & 7)]);
}
