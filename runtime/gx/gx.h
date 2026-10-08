// GX (Flipper graphics) command processing: shared state definitions.
#pragma once
#include <cstdint>
#include <vector>

namespace gx {

// Primitive types (command byte & 0xF8)
enum Prim : uint8_t {
    PRIM_QUADS = 0x80, PRIM_QUADS2 = 0x88, PRIM_TRIANGLES = 0x90, PRIM_TRISTRIP = 0x98,
    PRIM_TRIFAN = 0xA0, PRIM_LINES = 0xA8, PRIM_LINESTRIP = 0xB0, PRIM_POINTS = 0xB8,
};

struct State {
    uint32_t bp[256];
    uint32_t cp[256];
    uint32_t xf_mem[0x800];   // 0x0000-0x07FF: matrices, lights
    uint32_t xf_regs[0x100];  // 0x1000-0x10FF
    uint32_t bp_mask = 0xFFFFFF;
    // Set whenever a BP or XF register the pixel state is built from takes a new value;
    // cleared by the front end once it has snapshotted. A race frame issues some 13,000
    // draw commands but changes this state only about 1,000 times, so between changes the
    // front end hands back the last snapshot without building one to compare.
    bool pixel_dirty = true;
    // Bumped whenever a CP register is written. The vertex layout and the byte size of a
    // vertex are a few dozen bit extractions each and were both re-derived for every one
    // of those 13,000 draw commands, from registers the game sets once per model. Each is
    // cached against this counter, separately, so that neither can clear a flag the other
    // still needs. See layout_for() in xf.cpp and vertex_size() in cmd.cpp.
    uint32_t cp_gen = 1;
};

extern State g_state;

// Byte-size of one vertex for the given VAT index, based on current VCD/VAT.
uint32_t vertex_size(int vat);

// Process a buffer of FIFO commands. Returns bytes consumed (may stop early on a
// partial command when `partial_ok`).
uint32_t process(const uint8_t* data, uint32_t len, bool partial_ok);

// Callbacks to the renderer (weakly defined null implementations in cmd.cpp).
struct DrawCall {
    uint8_t prim;
    uint8_t vat;
    uint16_t count;
    const uint8_t* data;  // vertex data (big endian, as in FIFO)
    uint32_t stride;
};
void renderer_draw(const DrawCall& dc);
void renderer_efb_copy(uint32_t dest_addr, bool to_xfb);
void renderer_bp_write(uint32_t reg, uint32_t value);

}  // namespace gx
