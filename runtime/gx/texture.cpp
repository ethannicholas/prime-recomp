// GX texture decoding (all formats -> RGBA8), TLUT (palette) memory, and the
// guest-side texture cache.
#include "../runtime.h"
#include "gx.h"
#include "render.h"
#include "texture.h"
#include <unordered_map>

namespace gx {

static uint8_t g_tlut_mem[0x80000];  // TMEM upper half (palettes)
static uint32_t g_next_tex_id = 1;
uint32_t g_frame_counter;

static inline uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    return r | (g << 8) | (b << 16) | (a << 24);
}
static inline uint16_t be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

static inline uint32_t decode_565(uint16_t v) {
    uint32_t r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    return rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}
static inline uint32_t decode_5a3(uint16_t v) {
    if (v & 0x8000) {
        uint32_t r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
        return rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255);
    }
    uint32_t a = (v >> 12) & 7, r = (v >> 8) & 0xF, g = (v >> 4) & 0xF, b = v & 0xF;
    return rgba(r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1));
}
static inline uint32_t decode_ia8(uint16_t v) {
    uint32_t a = v >> 8, i = v & 0xFF;
    return rgba(i, i, i, a);
}

static inline uint32_t decode_tlut(const uint8_t* pal, uint32_t idx, uint32_t tlut_fmt) {
    uint16_t v = be16(pal + idx * 2);
    switch (tlut_fmt) {
    case 0: return decode_ia8(v);
    case 1: return decode_565(v);
    default: return decode_5a3(v);
    }
}

struct FmtInfo { uint32_t bw, bh, bpp; };  // block width/height in texels, bits per texel
static FmtInfo fmt_info(uint32_t fmt) {
    switch (fmt) {
    case TF_I4: case TF_CI4: case TF_CMPR: return {8, 8, 4};
    case TF_I8: case TF_IA4: case TF_CI8: return {8, 4, 8};
    case TF_IA8: case TF_RGB565: case TF_RGB5A3: case TF_CI14X2: return {4, 4, 16};
    case TF_RGBA8: return {4, 4, 32};
    default: return {8, 8, 4};
    }
}

uint32_t texture_size_bytes(uint32_t fmt, uint32_t w, uint32_t h) {
    FmtInfo fi = fmt_info(fmt);
    uint32_t bx = (w + fi.bw - 1) / fi.bw, by = (h + fi.bh - 1) / fi.bh;
    return bx * by * fi.bw * fi.bh * fi.bpp / 8;
}

static void decode_dxt_block(const uint8_t* src, uint32_t* dst, uint32_t stride, uint32_t w, uint32_t h, uint32_t x0, uint32_t y0) {
    uint16_t c0 = be16(src), c1 = be16(src + 2);
    uint32_t pal[4];
    pal[0] = decode_565(c0);
    pal[1] = decode_565(c1);
    auto ch = [](uint32_t c, int s) { return (c >> s) & 0xFF; };
    if (c0 > c1) {
        pal[2] = rgba((2 * ch(pal[0], 0) + ch(pal[1], 0)) / 3, (2 * ch(pal[0], 8) + ch(pal[1], 8)) / 3,
                      (2 * ch(pal[0], 16) + ch(pal[1], 16)) / 3, 255);
        pal[3] = rgba((ch(pal[0], 0) + 2 * ch(pal[1], 0)) / 3, (ch(pal[0], 8) + 2 * ch(pal[1], 8)) / 3,
                      (ch(pal[0], 16) + 2 * ch(pal[1], 16)) / 3, 255);
    } else {
        pal[2] = rgba((ch(pal[0], 0) + ch(pal[1], 0)) / 2, (ch(pal[0], 8) + ch(pal[1], 8)) / 2,
                      (ch(pal[0], 16) + ch(pal[1], 16)) / 2, 255);
        pal[3] = rgba((ch(pal[0], 0) + ch(pal[1], 0)) / 2, (ch(pal[0], 8) + ch(pal[1], 8)) / 2,
                      (ch(pal[0], 16) + ch(pal[1], 16)) / 2, 0);
    }
    for (uint32_t y = 0; y < 4; y++) {
        uint8_t bits = src[4 + y];
        for (uint32_t x = 0; x < 4; x++) {
            uint32_t px = x0 + x, py = y0 + y;
            if (px < w && py < h) dst[py * stride + px] = pal[(bits >> (6 - 2 * x)) & 3];
        }
    }
}

// Decode one mip level. `src` points to the level's data in guest memory.
static void decode_level(const uint8_t* src, uint32_t fmt, uint32_t w, uint32_t h, const uint8_t* pal,
                         uint32_t tlut_fmt, uint32_t* dst) {
    FmtInfo fi = fmt_info(fmt);
    uint32_t bx_count = (w + fi.bw - 1) / fi.bw, by_count = (h + fi.bh - 1) / fi.bh;
    for (uint32_t by = 0; by < by_count; by++) {
        for (uint32_t bx = 0; bx < bx_count; bx++) {
            uint32_t x0 = bx * fi.bw, y0 = by * fi.bh;
            auto put = [&](uint32_t x, uint32_t y, uint32_t c) {
                uint32_t px = x0 + x, py = y0 + y;
                if (px < w && py < h) dst[py * w + px] = c;
            };
            switch (fmt) {
            case TF_I4:
                for (uint32_t y = 0; y < 8; y++)
                    for (uint32_t x = 0; x < 8; x++) {
                        uint8_t b = src[y * 4 + x / 2];
                        uint32_t i = (x & 1) ? (b & 0xF) : (b >> 4);
                        i *= 17;
                        put(x, y, rgba(i, i, i, i));
                    }
                src += 32;
                break;
            case TF_I8:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++) {
                        uint32_t i = src[y * 8 + x];
                        put(x, y, rgba(i, i, i, i));
                    }
                src += 32;
                break;
            case TF_IA4:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++) {
                        uint8_t b = src[y * 8 + x];
                        uint32_t a = (b >> 4) * 17, i = (b & 0xF) * 17;
                        put(x, y, rgba(i, i, i, a));
                    }
                src += 32;
                break;
            case TF_IA8:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) put(x, y, decode_ia8(be16(src + (y * 4 + x) * 2)));
                src += 32;
                break;
            case TF_RGB565:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) put(x, y, decode_565(be16(src + (y * 4 + x) * 2)));
                src += 32;
                break;
            case TF_RGB5A3:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) put(x, y, decode_5a3(be16(src + (y * 4 + x) * 2)));
                src += 32;
                break;
            case TF_RGBA8:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++) {
                        uint32_t k = (y * 4 + x) * 2;
                        uint32_t a = src[k], r = src[k + 1], g = src[32 + k], b = src[32 + k + 1];
                        put(x, y, rgba(r, g, b, a));
                    }
                src += 64;
                break;
            case TF_CI4:
                for (uint32_t y = 0; y < 8; y++)
                    for (uint32_t x = 0; x < 8; x++) {
                        uint8_t b = src[y * 4 + x / 2];
                        put(x, y, decode_tlut(pal, (x & 1) ? (b & 0xF) : (b >> 4), tlut_fmt));
                    }
                src += 32;
                break;
            case TF_CI8:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 8; x++) put(x, y, decode_tlut(pal, src[y * 8 + x], tlut_fmt));
                src += 32;
                break;
            case TF_CI14X2:
                for (uint32_t y = 0; y < 4; y++)
                    for (uint32_t x = 0; x < 4; x++)
                        put(x, y, decode_tlut(pal, be16(src + (y * 4 + x) * 2) & 0x3FFF, tlut_fmt));
                src += 32;
                break;
            case TF_CMPR:
                for (uint32_t sb = 0; sb < 4; sb++) {
                    decode_dxt_block(src, dst, w, w, h, x0 + (sb & 1) * 4, y0 + (sb >> 1) * 4);
                    src += 8;
                }
                break;
            default:
                for (uint32_t y = 0; y < fi.bh; y++)
                    for (uint32_t x = 0; x < fi.bw; x++) put(x, y, rgba(255, 0, 255, 255));
                src += fi.bw * fi.bh * fi.bpp / 8;
                break;
            }
        }
    }
}

// FNV-1a over 8-byte words, for texture change detection. Every texture a frame samples
// is hashed once per frame, several megabytes in a race, and a single FNV chain is bound
// by the latency of its multiply: four independent lanes run in parallel and are folded
// together at the end, which is about three times the throughput on the same data.
static uint64_t hash_bytes(const uint8_t* p, size_t n, uint64_t seed) {
    constexpr uint64_t kPrime = 0x100000001b3ull;
    uint64_t h0 = 0xcbf29ce484222325ull ^ seed, h1 = h0 ^ 0x9e3779b97f4a7c15ull,
             h2 = h0 ^ 0x3c6ef372fe94f82bull, h3 = h0 ^ 0xdaa66d2b78dd3b40ull;
    size_t i = 0;
    for (; i + 32 <= n; i += 32) {
        uint64_t w0, w1, w2, w3;
        memcpy(&w0, p + i, 8);
        memcpy(&w1, p + i + 8, 8);
        memcpy(&w2, p + i + 16, 8);
        memcpy(&w3, p + i + 24, 8);
        h0 = (h0 ^ w0) * kPrime; h0 ^= h0 >> 29;
        h1 = (h1 ^ w1) * kPrime; h1 ^= h1 >> 29;
        h2 = (h2 ^ w2) * kPrime; h2 ^= h2 >> 29;
        h3 = (h3 ^ w3) * kPrime; h3 ^= h3 >> 29;
    }
    uint64_t h = (h0 ^ (h1 * kPrime)) ^ ((h2 ^ (h3 * kPrime)) * kPrime);
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, p + i, 8);
        h = (h ^ w) * kPrime;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * kPrime;
    return h;
}

void tlut_load(uint32_t src_addr, uint32_t tmem_off, uint32_t bytes) {
    if (tmem_off + bytes > sizeof(g_tlut_mem)) bytes = sizeof(g_tlut_mem) - tmem_off;
    memcpy(g_tlut_mem + tmem_off, phys_ptr(src_addr), bytes);
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------
struct CacheEntry {
    std::shared_ptr<TexData> tex;
    uint64_t hash;
    uint32_t last_frame;
    uint32_t checked_frame;
    bool efb;
    uint32_t efb_w, efb_h, efb_fmt;
};

struct Key {
    uint32_t addr, fmt, w, h, levels, tlut_off, tlut_fmt;
    bool operator==(const Key& o) const { return memcmp(this, &o, sizeof(Key)) == 0; }
};
struct KeyHash {
    size_t operator()(const Key& k) const { return hash_bytes((const uint8_t*)&k, sizeof(k), 0); }
};

static std::unordered_map<Key, CacheEntry, KeyHash> g_cache;
static std::unordered_map<uint32_t, CacheEntry> g_efb_copies;  // by address

uint32_t texture_register_efb_copy(uint32_t addr, uint32_t w, uint32_t h, uint32_t fmt) {
    addr &= 0x03FFFFFF;
    // A copy to the same place with the same geometry is the same target being redrawn,
    // so it keeps its id and the renderer re-renders into the texture it already has.
    // Minting a fresh id every frame instead stranded one GL texture per copy per frame
    // -- around 2.5 MB a frame here, since EFB textures are exempt from eviction.
    auto it = g_efb_copies.find(addr);
    if (it != g_efb_copies.end() && it->second.efb_w == w && it->second.efb_h == h &&
        it->second.efb_fmt == fmt) {
        it->second.last_frame = g_frame_counter;
        return it->second.tex->id;
    }
    CacheEntry e{};
    e.tex = std::make_shared<TexData>();
    e.tex->id = g_next_tex_id++;
    e.tex->width = w;
    e.tex->height = h;
    e.efb = true;
    e.efb_w = w; e.efb_h = h; e.efb_fmt = fmt;
    e.last_frame = g_frame_counter;
    g_efb_copies[addr] = e;
    return e.tex->id;
}

TexLookup texture_lookup(const TexParams& p, std::vector<std::shared_ptr<TexData>>& new_textures) {
    uint32_t addr = p.addr & 0x03FFFFFF;
    // EFB copies at this address take priority (the copy isn't written back to RAM).
    auto eit = g_efb_copies.find(addr);
    if (eit != g_efb_copies.end()) {
        eit->second.last_frame = g_frame_counter;
        return {eit->second.tex->id, true};
    }
    uint32_t tlut_bytes = 0;
    if (p.fmt == TF_CI4) tlut_bytes = 16 * 2;
    else if (p.fmt == TF_CI8) tlut_bytes = 256 * 2;
    else if (p.fmt == TF_CI14X2) tlut_bytes = 16384 * 2;
    Key k{addr, p.fmt, p.width, p.height, p.levels, tlut_bytes ? p.tlut_off : 0, tlut_bytes ? p.tlut_fmt : 0};
    CacheEntry& e = g_cache[k];
    if (e.tex && e.checked_frame == g_frame_counter) {
        e.last_frame = g_frame_counter;
        return {e.tex->id, false};
    }
    // Compute data size over all levels.
    uint32_t total = 0, w = p.width, h = p.height;
    for (uint32_t l = 0; l < p.levels; l++) {
        total += texture_size_bytes(p.fmt, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    if (addr + total > RAM_SIZE) return {0, false};
    const uint8_t* src = phys_ptr(addr);
    const uint8_t* pal = g_tlut_mem + (p.tlut_off < sizeof(g_tlut_mem) ? p.tlut_off : 0);
    uint64_t hsh = hash_bytes(src, total, 0);
    if (tlut_bytes) hsh = hash_bytes(pal, std::min<uint32_t>(tlut_bytes, sizeof(g_tlut_mem) - p.tlut_off), hsh);
    e.checked_frame = g_frame_counter;
    e.last_frame = g_frame_counter;
    if (e.tex && e.hash == hsh) return {e.tex->id, false};

    auto td = std::make_shared<TexData>();
    td->id = g_next_tex_id++;
    td->width = p.width;
    td->height = p.height;
    w = p.width; h = p.height;
    for (uint32_t l = 0; l < p.levels; l++) {
        std::vector<uint32_t> px(w * h);
        decode_level(src, p.fmt, w, h, pal, p.tlut_fmt, px.data());
        td->levels.push_back(std::move(px));
        src += texture_size_bytes(p.fmt, w, h);
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    e.tex = td;
    e.hash = hsh;
    new_textures.push_back(td);
    return {td->id, false};
}

void texture_invalidate_efb_copy(uint32_t addr) { g_efb_copies.erase(addr & 0x03FFFFFF); }

// Drop decoded textures the game has stopped referencing. Each entry holds every mip
// level as RGBA8, so a long race would otherwise accumulate hundreds of megabytes.
void texture_evict() {
    static constexpr uint32_t kIdleFrames = 240;  // ~8 s at 30 fps
    if ((g_frame_counter & 63) != 0) return;
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (g_frame_counter - it->second.last_frame > kIdleFrames) it = g_cache.erase(it);
        else ++it;
    }
    // EFB copies too. Most are a fixed set of targets redrawn every frame, but the spray
    // copies out around fifty 32x32 and 64x64 sprites a frame to addresses that rotate,
    // so at speed this map grows without bound and takes a GL texture with each entry.
    for (auto it = g_efb_copies.begin(); it != g_efb_copies.end();) {
        if (g_frame_counter - it->second.last_frame > kIdleFrames) it = g_efb_copies.erase(it);
        else ++it;
    }
}

}  // namespace gx
