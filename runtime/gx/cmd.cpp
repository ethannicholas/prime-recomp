// GX FIFO command stream parser.
#include "../runtime.h"
#include "gx.h"
#include <cmath>

const char* func_name(uint32_t addr);
void pe_signal_token(uint16_t token, bool interrupt);
void pe_signal_finish();

namespace gx {

State g_state;

static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static const uint32_t kCompSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};
static const uint32_t kColorSize[8] = {2, 3, 4, 2, 3, 4, 0, 0};

static inline uint32_t attr_size(uint32_t desc, uint32_t direct_size) {
    switch (desc) {
    case 1: return direct_size;
    case 2: return 1;
    case 3: return 2;
    default: return 0;
    }
}

static uint32_t vertex_size_uncached(int vat) {
    const uint32_t* cp = g_state.cp;
    uint32_t vcd_lo = cp[0x50], vcd_hi = cp[0x60];
    uint32_t a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];
    uint32_t size = 0;
    if (vcd_lo & 1) size++;                      // PNMTXIDX
    for (int i = 0; i < 8; i++) if (vcd_lo & (2u << i)) size++;  // TEXnMTXIDX
    // position
    {
        uint32_t cnt = (a & 1) ? 3 : 2;
        size += attr_size((vcd_lo >> 9) & 3, cnt * kCompSize[(a >> 1) & 7]);
    }
    // normal
    {
        uint32_t desc = (vcd_lo >> 11) & 3;
        if (desc) {
            bool nbt = (a >> 9) & 1;
            bool idx3 = (a >> 31) & 1;
            uint32_t comp = kCompSize[(a >> 10) & 7];
            if (desc == 1) size += (nbt ? 9 : 3) * comp;
            else {
                uint32_t isz = desc == 2 ? 1 : 2;
                size += (nbt && idx3) ? 3 * isz : isz;
            }
        }
    }
    // colors
    size += attr_size((vcd_lo >> 13) & 3, kColorSize[(a >> 14) & 7]);
    size += attr_size((vcd_lo >> 15) & 3, kColorSize[(a >> 18) & 7]);
    // texcoords
    uint32_t tcnt[8], tfmt[8];
    tcnt[0] = (a >> 21) & 1; tfmt[0] = (a >> 22) & 7;
    tcnt[1] = b & 1;         tfmt[1] = (b >> 1) & 7;
    tcnt[2] = (b >> 9) & 1;  tfmt[2] = (b >> 10) & 7;
    tcnt[3] = (b >> 18) & 1; tfmt[3] = (b >> 19) & 7;
    tcnt[4] = (b >> 27) & 1; tfmt[4] = (b >> 28) & 7;
    tcnt[5] = (c >> 5) & 1;  tfmt[5] = (c >> 6) & 7;
    tcnt[6] = (c >> 14) & 1; tfmt[6] = (c >> 15) & 7;
    tcnt[7] = (c >> 23) & 1; tfmt[7] = (c >> 24) & 7;
    for (int i = 0; i < 8; i++) {
        uint32_t desc = (vcd_hi >> (2 * i)) & 3;
        size += attr_size(desc, (tcnt[i] ? 2 : 1) * kCompSize[tfmt[i]]);
    }
    return size;
}

static void load_cp(uint8_t reg, uint32_t v) {
    g_state.cp[reg] = v;
    g_state.cp_gen++;
}

static void load_xf(uint32_t addr, uint32_t n, const uint8_t* data) {
#ifdef MP_CALL_TRACE
    // MP_POSMTX_STACK=<z> prints the guest call stack whenever a position matrix with
    // that view-space Z is loaded, which names the code placing the object. Needs a
    // build with MP_TRACE_CALLS; see ENTER()/RET() in recomp.h.
    static const char* want_z = getenv("MP_POSMTX_STACK");
    if (want_z && n == 12 && (addr & 3) == 0 && addr < 0x100) {
        float tz, ty;
        const uint32_t w = be32(data + 44), wy = be32(data + 28);
        memcpy(&tz, &w, 4);
        memcpy(&ty, &wy, 4);
        static int shown;
        // The rig descends while it turns, so its Y is anywhere in a range; Z is exact.
        if (fabsf(tz - (float)atof(want_z)) < 0.05f && ty > 25.0f && ty < 65.0f &&
            shown++ < 4) {
            CPU* c = cpu_current();
            fprintf(stderr, "[posmtx] id=%u ty=%.1f tz=%.1f, guest call stack innermost first:\n",
                    addr / 4, (double)ty, (double)tz);
            const uint32_t have = c && c->depth < 256 ? c->depth : 0;
            for (uint32_t i = 1; i <= have; i++)
                fprintf(stderr, "[posmtx]   %08X %s\n", c->stack[have - i],
                        func_name(c->stack[have - i]));
        }
    }
#endif
    for (uint32_t i = 0; i < n; i++, addr++) {
        uint32_t v = be32(data + 4 * i);
        if (addr < 0x800) g_state.xf_mem[addr] = v;
        else if (addr >= 0x1000 && addr < 0x1100) {
            const uint32_t reg = addr - 0x1000;
            uint32_t& r = g_state.xf_regs[reg];
            if (r == v) continue;
            r = v;
            // The ones the pixel state is built from: colour channel count, viewport,
            // projection, texgen count. The rest feed the vertex transform only.
            if (reg == 0x09 || (reg >= 0x1A && reg <= 0x26) || reg == 0x3F) g_state.pixel_dirty = true;
        }
    }
}

static void load_indexed(int array, uint32_t w) {
    uint32_t index = w >> 16;
    uint32_t size = ((w >> 12) & 0xF) + 1;
    uint32_t addr = w & 0xFFF;
    uint32_t base = g_state.cp[0xA0 + array];
    uint32_t stride = g_state.cp[0xB0 + array];
    const uint8_t* src = phys_ptr(base + index * stride);
    for (uint32_t i = 0; i < size; i++) {
        if (addr + i < 0x800) g_state.xf_mem[addr + i] = be32(src + 4 * i);
    }
}

static void load_bp(uint32_t w) {
    uint32_t reg = w >> 24;
    uint32_t v = w & 0xFFFFFF;
    if (reg == 0xFE) { g_state.bp_mask = v; return; }
    if (g_state.bp_mask != 0xFFFFFF) {
        v = (g_state.bp[reg] & ~g_state.bp_mask) | (v & g_state.bp_mask);
        g_state.bp_mask = 0xFFFFFF;
    }
    if (g_state.bp[reg] != v) g_state.pixel_dirty = true;
    g_state.bp[reg] = v;
    switch (reg) {
    case 0x45:  // PE_DONE
        if (v & 2) pe_signal_finish();
        break;
    case 0x47: pe_signal_token((uint16_t)v, false); break;
    case 0x48: pe_signal_token((uint16_t)v, true); break;
    case 0x52: {  // copy execute
        uint32_t dest = (g_state.bp[0x4B] & 0x1FFFFF) << 5;
        renderer_efb_copy(dest, (v >> 14) & 1);
        // The copy may have minted a texture id for an address a draw samples.
        g_state.pixel_dirty = true;
        break;
    }
    case 0x65: g_state.pixel_dirty = true; break;  // TLUT load: palette contents changed
    }
    renderer_bp_write(reg, v);
}

// Cached against State::cp_gen, since it is derived from the CP registers alone. It is
// asked for once per draw command, and a race frame issues some 13,000 of those.
uint32_t vertex_size(int vat) {
    static uint32_t cached[8];
    static uint32_t valid, gen;
    if (gen != g_state.cp_gen) { valid = 0; gen = g_state.cp_gen; }
    if (!(valid & (1u << vat))) {
        cached[vat] = vertex_size_uncached(vat);
        valid |= 1u << vat;
    }
    return cached[vat];
}

uint32_t process(const uint8_t* data, uint32_t len, bool partial_ok) {
    uint32_t pos = 0;
    while (pos < len) {
        const uint8_t* p = data + pos;
        uint32_t avail = len - pos;
        uint8_t cmd = p[0];
        uint32_t need;
        switch (cmd) {
        case 0x00: pos += 1; continue;           // NOP
        case 0x48: pos += 1; continue;           // invalidate vertex cache
        case 0x08:                               // LOAD_CP_REG
            if (avail < 6) goto partial;
            load_cp(p[1], be32(p + 2));
            pos += 6;
            continue;
        case 0x10: {                             // LOAD_XF_REG
            if (avail < 5) goto partial;
            uint32_t w = be32(p + 1);
            uint32_t n = ((w >> 16) & 0xF) + 1;
            need = 5 + 4 * n;
            if (avail < need) goto partial;
            load_xf(w & 0xFFFF, n, p + 5);
            pos += need;
            continue;
        }
        case 0x20: case 0x28: case 0x30: case 0x38:  // LOAD_INDX_A..D
            if (avail < 5) goto partial;
            load_indexed(12 + ((cmd - 0x20) >> 3), be32(p + 1));
            pos += 5;
            continue;
        case 0x40: {                             // CALL_DL
            if (avail < 9) goto partial;
            uint32_t addr = be32(p + 1) & 0x03FFFFFF, size = be32(p + 5);
            process(phys_ptr(addr), size, false);
            pos += 9;
            continue;
        }
        case 0x44: pos += 1; continue;
        case 0x61:                               // LOAD_BP_REG
            if (avail < 5) goto partial;
            load_bp(be32(p + 1));
            pos += 5;
            continue;
        default:
            if (cmd & 0x80) {
                if (avail < 3) goto partial;
                int vat = cmd & 7;
                uint16_t count = be16(p + 1);
                uint32_t vsz = vertex_size(vat);
                need = 3 + vsz * count;
                if (avail < need) goto partial;
                DrawCall dc{(uint8_t)(cmd & 0xF8), (uint8_t)vat, count, p + 3, vsz};
                renderer_draw(dc);
                pos += need;
                continue;
            }
            LOG(LOG_GX, "unknown GX opcode %02X at +%u of %u", cmd, pos, len);
            pos += 1;  // resync by skipping
            continue;
        }
    partial:
        if (!partial_ok) LOG(LOG_GX, "truncated command %02X in display list", cmd);
        return partial_ok ? pos : len;
    }
    return pos;
}


}  // namespace gx
