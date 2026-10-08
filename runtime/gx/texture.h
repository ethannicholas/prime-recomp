#pragma once
#include <cstdint>
#include <memory>
#include <vector>

namespace gx {

struct TexData;

enum TexFmt : uint32_t {
    TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6,
    TF_CI4 = 8, TF_CI8 = 9, TF_CI14X2 = 10, TF_CMPR = 14,
};

struct TexParams {
    uint32_t addr, fmt, width, height, levels;
    uint32_t tlut_off, tlut_fmt;
};

struct TexLookup {
    uint32_t id;
    bool efb;
};

extern uint32_t g_frame_counter;

TexLookup texture_lookup(const TexParams& p, std::vector<std::shared_ptr<TexData>>& new_textures);
uint32_t texture_register_efb_copy(uint32_t addr, uint32_t w, uint32_t h, uint32_t fmt);
void texture_invalidate_efb_copy(uint32_t addr);

// Drop decoded textures the game has stopped referencing. Call once per frame.
void texture_evict();
void tlut_load(uint32_t src_addr, uint32_t tmem_off, uint32_t bytes);
uint32_t texture_size_bytes(uint32_t fmt, uint32_t w, uint32_t h);

}  // namespace gx
