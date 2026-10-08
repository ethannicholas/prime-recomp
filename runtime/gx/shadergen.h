#pragma once
#include "render.h"
#include <cstring>
#include <string>

namespace gx {

struct ShaderKey {
    uint32_t genmode;
    uint32_t order[8];
    uint32_t cenv[16], aenv[16];
    uint32_t indcmd[16];
    uint32_t ksel[8];
    uint32_t alpha_func;
    uint32_t iref;
    uint32_t fog;
    uint32_t num_texgens;
    uint32_t efb_has_alpha;
    uint32_t efb_tex_mask;
    bool operator==(const ShaderKey& o) const { return memcmp(this, &o, sizeof(*this)) == 0; }
};

struct ShaderKeyHash {
    size_t operator()(const ShaderKey& k) const {
        const uint32_t* p = (const uint32_t*)&k;
        size_t h = 1469598103934665603ull;
        for (size_t i = 0; i < sizeof(k) / 4; i++) h = (h ^ p[i]) * 1099511628211ull;
        return h;
    }
};

ShaderKey make_shader_key(const PixelState& st);
std::string gen_vertex_shader();
// MP_GLSL_VERSION, plus what writing gl_ClipDistance needs; defines MP_CLIP where it can.
std::string glsl_header_with_clip();
std::string gen_pixel_shader(const ShaderKey& k);

}  // namespace gx
