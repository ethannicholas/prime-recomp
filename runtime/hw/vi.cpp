// Video Interface: retrace interrupts and framebuffer registers.
#include "../runtime.h"
#include <atomic>

static uint16_t g_vi[0x80];
static uint64_t g_frame_start;
static constexpr uint64_t FIELD_TICKS = (uint64_t)(TB_FREQ / 59.94);
static constexpr int LINES_PER_FIELD = 263;
std::atomic<uint32_t> g_vi_retrace_count{0};
std::atomic<uint32_t> g_vi_xfb_addr{0};

static void vi_update_irq() {
    bool any = false;
    for (int i = 0; i < 4; i++) {
        uint16_t hi = g_vi[(0x30 + 4 * i) >> 1];
        if ((hi & 0x8000) && (hi & 0x1000)) any = true;
    }
    pi_set_interrupt(INT_VI, any);
}

static void vi_retrace() {
    g_frame_start = now_ticks();
    g_vi_retrace_count++;
    uint32_t tfbl = ((uint32_t)g_vi[0x1C >> 1] << 16) | g_vi[0x1E >> 1];
    g_vi_xfb_addr = (tfbl & 0x10000000) ? ((tfbl & 0xFFFFFF) << 5) : (tfbl & 0xFFFFFF);
    // Raise display interrupts 0 and 1 if enabled.
    for (int i = 0; i < 2; i++) {
        uint16_t& hi = g_vi[(0x30 + 4 * i) >> 1];
        if (hi & 0x1000) hi |= 0x8000;
    }
    vi_update_irq();
    event_schedule(g_frame_start + FIELD_TICKS, vi_retrace);
}

void vi_init() {
    event_schedule_in(FIELD_TICKS, vi_retrace);
}

uint16_t vi_read16(uint32_t off) {
    off &= 0xFF;
    switch (off) {
    case 0x2C: {  // DPV: current vertical position (1-based)
        uint64_t t = now_ticks() - g_frame_start;
        uint32_t line = (uint32_t)(t * LINES_PER_FIELD / FIELD_TICKS) % LINES_PER_FIELD;
        return (uint16_t)(line + 1);
    }
    case 0x2E: return 1;
    case 0x6E: return 0;  // DTV status: no component cable
    default: return g_vi[off >> 1];
    }
}

void vi_write16(uint32_t off, uint16_t v) {
    off &= 0xFF;
    if (getenv("MP_VILOG") && off < 0x40 && g_vi[off >> 1] != v) fprintf(stderr, "[vi] reg %02X = %04X\n", off, v);
    g_vi[off >> 1] = v;
    if (off >= 0x30 && off < 0x40) vi_update_irq();
}
