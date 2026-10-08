// DSP interface: mailboxes, ARAM DMA, audio (AI) DMA. The DSP itself is HLE'd
// (see dsp_hle.cpp) - this file only models the CPU-side registers.
#include "../runtime.h"
#include "dsp_hle.h"
#include <algorithm>
#include <deque>
#include <mutex>

static uint16_t g_dspcr;
static uint16_t g_mbox_in_h, g_mbox_in_l;
static uint16_t g_ar_size, g_ar_mode = 1, g_ar_refresh;
static uint32_t g_ar_mm, g_ar_aram, g_ar_cnt;
static uint32_t g_aid_addr;  // AI DMA registers (as written)
static uint16_t g_aid_ctrl;  // bit 15 enable, 14..0 length in 32-byte blocks
static uint32_t g_aid_cur_addr, g_aid_blocks_left;
static bool g_aid_running;

// DSP -> CPU mail queue
static std::mutex g_mail_mutex;
static std::deque<uint32_t> g_mail_out;

enum : uint16_t {
    CR_RES = 1 << 0, CR_PIINT = 1 << 1, CR_HALT = 1 << 2, CR_AID = 1 << 3, CR_AID_MASK = 1 << 4,
    CR_ARAM = 1 << 5, CR_ARAM_MASK = 1 << 6, CR_DSP = 1 << 7, CR_DSP_MASK = 1 << 8, CR_DMA = 1 << 9,
    CR_INT_BITS = CR_AID | CR_ARAM | CR_DSP,
};

static void dsp_update_irq() {
    bool irq = ((g_dspcr & CR_AID) && (g_dspcr & CR_AID_MASK)) ||
               ((g_dspcr & CR_ARAM) && (g_dspcr & CR_ARAM_MASK)) ||
               ((g_dspcr & CR_DSP) && (g_dspcr & CR_DSP_MASK));
    pi_set_interrupt(INT_DSP, irq);
}

void dsp_raise(uint16_t bit) {
    g_dspcr |= bit;
    dsp_update_irq();
}

void dsp_send_mail(uint32_t mail, bool interrupt) {
    {
        std::lock_guard<std::mutex> lk(g_mail_mutex);
        g_mail_out.push_back(mail);
    }
    LOG(LOG_DSP, "DSP->CPU mail %08X%s", mail, interrupt ? " (int)" : "");
    if (interrupt) dsp_raise(CR_DSP);
}

void dsp_mail_clear() {
    std::lock_guard<std::mutex> lk(g_mail_mutex);
    g_mail_out.clear();
}

static uint16_t mail_out_hi() {
    std::lock_guard<std::mutex> lk(g_mail_mutex);
    if (g_mail_out.empty()) return 0;
    return (uint16_t)(g_mail_out.front() >> 16) | 0x8000;
}
static uint16_t mail_out_lo() {
    std::lock_guard<std::mutex> lk(g_mail_mutex);
    if (g_mail_out.empty()) return 0;
    uint16_t v = (uint16_t)g_mail_out.front();
    g_mail_out.pop_front();
    return v;
}

// ---------------------------------------------------------------------------
// ARAM DMA
// ---------------------------------------------------------------------------
static void aram_dma() {
    uint32_t len = g_ar_cnt & 0x7FFFFFFF;
    bool to_mram = g_ar_cnt & 0x80000000u;
    uint32_t aram = g_ar_aram & (ARAM_SIZE - 1);
    if (aram + len > ARAM_SIZE) len = ARAM_SIZE - aram;
    // The ARAM side is clamped above; this is the main-memory side, which nothing was
    // checking. `len` itself is left alone so that the log line and the completion delay
    // still describe the transfer the guest asked for.
    const uint32_t moved = dma_fit(to_mram ? "ARAM->MRAM" : "MRAM->ARAM", g_ar_mm, len);
    if (to_mram) memcpy(phys_ptr(g_ar_mm), g_aram + aram, moved);
    else memcpy(g_aram + aram, phys_ptr(g_ar_mm), moved);
    LOG(LOG_DSP, "ARAM DMA %s mm=%08X ar=%08X len=%X", to_mram ? "ARAM->MRAM" : "MRAM->ARAM", g_ar_mm, g_ar_aram, len);
    g_ar_cnt &= 0x80000000u;
    g_dspcr |= CR_DMA;
    // Complete after a short delay (~ARAM bandwidth), then raise ARINT.
    event_schedule_in(TB_FREQ / 50000 + (uint64_t)len * TB_FREQ / (256 << 20), [] {
        g_dspcr &= ~CR_DMA;
        dsp_raise(CR_ARAM);
    });
}

// ---------------------------------------------------------------------------
// Audio DMA: streams 32-byte blocks at the AI DMA sample rate.
// ---------------------------------------------------------------------------
extern void audio_push_dma(const int16_t* samples_be, uint32_t frames);
extern uint32_t ai_dma_sample_rate();

static void aid_block_done();

// When the block in flight finishes on the DMA's own clock, which runs from when the
// DMA was enabled. Events run late by however long the guest took to poll for them;
// timing each block from when the last one's event ran would add all of that lateness
// up, and the DMA would deliver audibly fewer samples a second than its rate says.
static uint64_t g_aid_due;

static void aid_start_block() {
    g_aid_cur_addr = g_aid_addr;
    g_aid_blocks_left = g_aid_ctrl & 0x7FFF;
    // The interrupt fires when a DMA starts, so the game can queue the next buffer.
    dsp_raise(CR_AID);
    uint32_t bytes = g_aid_blocks_left * 32;
    uint64_t dur = (uint64_t)bytes / 4 * TB_FREQ / ai_dma_sample_rate();
    if (!dur) dur = TB_FREQ / 1000;
    uint64_t now = now_ticks();
    g_aid_due += dur;
    // A guest that stalled for longer than anything downstream buffers has lost that
    // audio; start the clock again rather than replay the backlog at speed.
    if (g_aid_due + TB_FREQ / 10 < now) g_aid_due = now;
    // Behind by less than that, catch up -- but leave the guest time to take this
    // interrupt and queue a buffer it has actually filled before the block is over.
    event_schedule(std::max(g_aid_due, now + dur / 4), aid_block_done);
}

static void aid_block_done() {
    uint32_t bytes = g_aid_blocks_left * 32;
    static int n;
    if (getenv("MP_AXSTATS") && n++ % 400 == 0) fprintf(stderr, "[aid] block addr=%08X bytes=%u ctrl=%04X\n", g_aid_cur_addr, bytes, g_aid_ctrl);
    bytes = dma_fit("AI", g_aid_cur_addr, bytes);
    if (bytes) audio_push_dma((const int16_t*)phys_ptr(g_aid_cur_addr), bytes / 4);
    g_aid_blocks_left = 0;
    if (g_aid_ctrl & 0x8000) aid_start_block();
    else g_aid_running = false;
}

// ---------------------------------------------------------------------------
void dsp_init() { dsp_hle_reset(); }

uint16_t dsp_read16(uint32_t off) {
    switch (off & 0xFF) {
    case 0x00: return g_mbox_in_h;
    case 0x02: return g_mbox_in_l;
    case 0x04: return mail_out_hi();
    case 0x06: return mail_out_lo();
    case 0x0A: return g_dspcr;
    case 0x12: return g_ar_size;
    case 0x16: return g_ar_mode;
    case 0x1A: return g_ar_refresh;
    case 0x20: return g_ar_mm >> 16;
    case 0x22: return (uint16_t)g_ar_mm;
    case 0x24: return g_ar_aram >> 16;
    case 0x26: return (uint16_t)g_ar_aram;
    case 0x28: return g_ar_cnt >> 16;
    case 0x2A: return (uint16_t)g_ar_cnt;
    case 0x30: return g_aid_addr >> 16;
    case 0x32: return (uint16_t)g_aid_addr;
    case 0x36: return g_aid_ctrl;
    case 0x3A: return (uint16_t)(g_aid_blocks_left ? g_aid_blocks_left - 1 : 0);
    }
    LOG(LOG_DSP, "DSP read %02X", off & 0xFF);
    return 0;
}

void dsp_write16(uint32_t off, uint16_t v) {
    switch (off & 0xFF) {
    case 0x00: g_mbox_in_h = v; return;
    case 0x02: {
        g_mbox_in_l = v;
        uint32_t mail = ((uint32_t)g_mbox_in_h << 16) | v;
        LOG(LOG_DSP, "CPU->DSP mail %08X", mail);
        g_mbox_in_h &= 0x7FFF;  // DSP consumed it immediately
        dsp_hle_mail(mail);
        return;
    }
    case 0x0A: {
        uint16_t clr = v & CR_INT_BITS;
        g_dspcr = (uint16_t)((g_dspcr & (CR_INT_BITS | CR_DMA)) & ~clr) | (v & ~(CR_INT_BITS | CR_DMA | CR_RES));
        if (v & CR_RES) {
            LOG(LOG_DSP, "DSP reset");
            dsp_hle_reset();
        }
        if (!(v & 0x800)) {
            // DSPInit cleared: the IPL's tiny init program runs and reports back.
            g_dspcr &= ~0x400;
            dsp_mail_clear();
            dsp_send_mail(0x80544348, false);
        }
        if (v & CR_PIINT) {
            g_dspcr &= ~CR_PIINT;
            dsp_hle_cpu_interrupt();
        }
        dsp_update_irq();
        return;
    }
    case 0x12: g_ar_size = v; return;
    case 0x16: g_ar_mode = v; return;
    case 0x1A: g_ar_refresh = v; return;
    case 0x20: g_ar_mm = (g_ar_mm & 0xFFFF) | ((uint32_t)v << 16); return;
    case 0x22: g_ar_mm = (g_ar_mm & 0xFFFF0000u) | (v & ~0x1Fu); return;
    case 0x24: g_ar_aram = (g_ar_aram & 0xFFFF) | ((uint32_t)v << 16); return;
    case 0x26: g_ar_aram = (g_ar_aram & 0xFFFF0000u) | (v & ~0x1Fu); return;
    case 0x28: g_ar_cnt = (g_ar_cnt & 0xFFFF) | ((uint32_t)v << 16); return;
    case 0x2A: g_ar_cnt = (g_ar_cnt & 0xFFFF0000u) | (v & ~0x1Fu); aram_dma(); return;
    case 0x30: g_aid_addr = (g_aid_addr & 0xFFFF) | ((uint32_t)(v & 0x03FF) << 16); return;
    case 0x32: g_aid_addr = (g_aid_addr & 0xFFFF0000u) | (v & ~0x1Fu); return;
    case 0x36: {
        bool was = g_aid_ctrl & 0x8000;
        g_aid_ctrl = v;
        if ((v & 0x8000) && !was && !g_aid_running) {
            g_aid_running = true;
            g_aid_due = now_ticks();
            aid_start_block();
        }
        return;
    }
    }
    LOG(LOG_DSP, "DSP write %02X = %04X", off & 0xFF, v);
}
