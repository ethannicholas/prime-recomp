// Generates GLSL for the GX pixel pipeline (TEV) from BP state.
#include "shadergen.h"
#include "gl.h"   // MP_GLSL_VERSION
#include <cstdio>
#include <cstring>
#include <string>
#include <cstdlib>

namespace gx {

ShaderKey make_shader_key(const PixelState& st) {
    ShaderKey k;
    memset(&k, 0, sizeof(k));
    const uint32_t* bp = st.bp;
    k.genmode = bp[0x00] & ~0xC000u;  // without cull mode
    uint32_t nstages = ((bp[0x00] >> 10) & 15) + 1;
    uint32_t nind = (bp[0x00] >> 16) & 7;
    for (uint32_t i = 0; i < (nstages + 1) / 2; i++) k.order[i] = bp[0x28 + i];
    for (uint32_t i = 0; i < nstages; i++) {
        k.cenv[i] = bp[0xC0 + 2 * i];
        k.aenv[i] = bp[0xC1 + 2 * i];
        k.indcmd[i] = bp[0x10 + i];
    }
    for (int i = 0; i < 8; i++) k.ksel[i] = bp[0xF6 + i];
    k.alpha_func = (bp[0xF3] >> 16) & 0xFF;
    k.iref = nind ? bp[0x27] & ((1u << (6 * nind)) - 1) : 0;
    k.fog = (bp[0xF1] >> 20) & 0xF;  // proj bit + fsel
    k.num_texgens = st.num_texgens;
    k.efb_has_alpha = (bp[0x43] & 7) == 1;
    uint32_t used = 0;
    for (uint32_t s = 0; s < nstages; s++) {
        uint32_t order = bp[0x28 + s / 2] >> ((s & 1) * 12);
        if (order & 0x40) used |= 1u << (order & 7);
    }
    for (uint32_t i = 0; i < nind; i++) used |= 1u << ((bp[0x27] >> (6 * i)) & 7);
    for (int m = 0; m < 8; m++)
        if ((used & (1u << m)) && st.tex_is_efb[m]) k.efb_tex_mask |= 1u << m;
    return k;
}

static const char* kColorIn[16] = {
    "prev.rgb", "prev.aaa", "c0.rgb", "c0.aaa", "c1.rgb", "c1.aaa", "c2.rgb", "c2.aaa",
    "tex.rgb", "tex.aaa", "ras.rgb", "ras.aaa", "ivec3(255)", "ivec3(128)", "konst.rgb", "ivec3(0)"};
static const char* kAlphaIn[8] = {"prev.a", "c0.a", "c1.a", "c2.a", "tex.a", "ras.a", "konst.a", "0"};
static const char* kDest[4] = {"prev", "c0", "c1", "c2"};

static std::string konst_color(uint32_t sel) {
    static const int frac[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    char b[64];
    if (sel < 8) { snprintf(b, sizeof(b), "ivec3(%d)", frac[sel]); return b; }
    if (sel >= 12 && sel <= 15) { snprintf(b, sizeof(b), "u_konst[%u].rgb", sel - 12); return b; }
    if (sel >= 16) {
        static const char* comp = "rgba";
        snprintf(b, sizeof(b), "ivec3(u_konst[%u].%c)", (sel - 16) & 3, comp[(sel - 16) >> 2]);
        return b;
    }
    return "ivec3(255)";
}
static std::string konst_alpha(uint32_t sel) {
    static const int frac[8] = {255, 223, 191, 159, 128, 96, 64, 32};
    char b[64];
    if (sel < 8) { snprintf(b, sizeof(b), "%d", frac[sel]); return b; }
    if (sel >= 16) {
        static const char* comp = "rgba";
        snprintf(b, sizeof(b), "u_konst[%u].%c", (sel - 16) & 3, comp[(sel - 16) >> 2]);
        return b;
    }
    return "255";
}

static std::string swizzle(const ShaderKey& k, uint32_t table) {
    // Swap table n lives in ksel[2n] (r,g) and ksel[2n+1] (b,a).
    static const char* comp = "rgba";
    uint32_t a = k.ksel[table * 2], b = k.ksel[table * 2 + 1];
    std::string s;
    s += comp[a & 3];
    s += comp[(a >> 2) & 3];
    s += comp[b & 3];
    s += comp[(b >> 2) & 3];
    return s;
}

static const char* kCompare[8] = {"false", "(%s < %s)", "(%s == %s)", "(%s <= %s)", "(%s > %s)", "(%s != %s)", "(%s >= %s)", "true"};

// The morph between theater and stereo crops the scene to a window that opens out of the
// game's own frustum, which is what clip distances are for. Desktop GL has them in core; ES
// needs an extension, and its directive has to come straight after #version, ahead of the
// precision statements MP_GLSL_VERSION carries. Without it the morph runs uncropped rather
// than not at all.
std::string glsl_header_with_clip() {
    std::string s = MP_GLSL_VERSION;
#ifdef MP_GL_ES
    s.insert(s.find('\n') + 1,
             "#ifdef GL_EXT_clip_cull_distance\n"
             "#extension GL_EXT_clip_cull_distance : enable\n"
             "#define MP_CLIP 1\n"
             "#endif\n");
#else
    s += "#define MP_CLIP 1\n";
#endif
    return s;
}

std::string gen_vertex_shader() {
    std::string s = glsl_header_with_clip();
    s += R"(
layout(location = 0) in vec3 a_pos;
layout(location = 1) in vec4 a_col0;
layout(location = 2) in vec4 a_col1;
)";
    // GLSL ES does not allow arrays of vertex inputs ("cannot declare arrays of this
    // qualifier"), though desktop GLSL does, so declare the eight texture coordinate
    // sets individually. Locations 3..10 match the attribute setup in render_init().
    char buf[96];
    for (int i = 0; i < 8; i++) {
        snprintf(buf, sizeof(buf), "layout(location = %d) in vec3 a_tex%d;\n", 3 + i, i);
        s += buf;
    }
    s += R"(uniform mat4 u_proj;
uniform vec4 u_vp_a;   // x: 2*(offx-xoff)/w - 1, y: 2*sx/w, z: 2*(offy-yoff)/h - 1, w: 2*sy/h (scissor offset)
uniform vec4 u_vp_b;   // x: 2*offz/16777215 - 1, y: 2*sz/16777215
uniform float u_point_size;
// VR. a_pos arrives in the game's view space, so an eye is just another transform in
// front of the projection -- which is why both eyes can share one vertex buffer and
// the CPU-side transform in xf.cpp runs once, not twice.
//   0 = flat, the game's own projection and GX viewport transform
//   1 = world geometry through the eye projection
//   2 = a 2D element, painted on the HUD frame out in front of the game's camera
//   3 = part way between theater and stereo: u_proj is the whole morphed chain, folded
//       on the CPU, and u_crop cuts the scene to the window opening out of the game's
//       frustum. See render_set_vr_morph.
uniform int u_vr;
uniform mat4 u_view;       // eye transform, relative to the game's camera; under 3, fog's
uniform mat4 u_crop;       // four clip distances, each linear in the vertex
// The game's own projection reduced to what depth needs, as a function of view-space z:
// clip z = .x * z + .y and clip w = .z * z + .w, so the last pair is (-1, 0) for a
// frustum and (0, 1) for an ortho batch. Fog needs the depth GX would have written
// whatever matrix the vertex is actually drawn with; see v_fogz.
uniform vec4 u_zproj;
out vec4 v_col0;
out vec4 v_col1;
out vec3 v_tex[8];
// The game's screen z as a numerator and the w to divide it by. Both are affine in the
// vertex position, so perspective-correct interpolation delivers them exactly and the
// ratio is the real value at the fragment -- which a single interpolated z/w would not be.
out vec2 v_fogz;
void main() {
)";
    // Fog is a curve over the 24-bit depth GX writes, and in an eye gl_FragCoord.z is
    // not that: it comes from the headset's own frustum, whose near and far are nothing
    // like the game's. Fed to the curve it saturates a few metres out, so everything
    // beyond came back the fog colour -- which is what turned the sea bed black in
    // stereo while the flat view was fine. So the game's own screen z is rebuilt here
    // from its projection, independently of the matrix the vertex is drawn with.
    //
    // The depth fed in is this eye's, not the game camera's. Fog is a distance cue, and
    // the distance that matters to a viewer who has turned their head is the one along
    // their own line of sight: with the game's z, something off to the side fogs as
    // though it were as near as its forward depth, and stops fogging at all when it
    // leaves the game's 60-degree frustum. MP_EYE_FOGZ=0 takes the game's depth instead,
    // which fogs exactly as the flat view does whatever the head is doing.
    static const bool eye_fog_z = !(getenv("MP_EYE_FOGZ") && atoi(getenv("MP_EYE_FOGZ")) == 0);
    s += eye_fog_z ? "    float fz = (u_vr == 1 || u_vr == 3) ? (u_view * vec4(a_pos, 1.0)).z : a_pos.z;\n"
                   : "    float fz = a_pos.z;\n";
    s += R"(    float gz = u_zproj.x * fz + u_zproj.y;
    float gw = u_zproj.z * fz + u_zproj.w;
    v_fogz = vec2((0.5 * u_vp_b.x + 0.5) * gw + 0.5 * u_vp_b.y * gz, gw);
    if (u_vr == 1) {
        // Straight to clip space: the eye's render target is the whole viewport, so
        // the GX viewport transform does not apply. No Y negation either -- that
        // exists only to cancel the flip the EFB blit does, and nothing blits here.
        gl_Position = u_proj * (u_view * vec4(a_pos, 1.0));
    } else if (u_vr == 2) {
        // A 2D element in an eye. u_proj here is not the game's projection alone but the
        // whole chain folded on the CPU: that projection, the frame the overlay is
        // painted on, and the eye. GX's viewport transform is left out on purpose -- it
        // places the frame in the EFB, which an eye does not render into. Every term is
        // constant for the draw, so there is nothing left to do per vertex.
        gl_Position = u_proj * vec4(a_pos, 1.0);
    } else if (u_vr == 3) {
        gl_Position = u_proj * vec4(a_pos, 1.0);
    } else {
        vec4 clip = u_proj * vec4(a_pos, 1.0);
        vec4 p;
        p.x = u_vp_a.x * clip.w + u_vp_a.y * clip.x;
        p.y = u_vp_a.z * clip.w + u_vp_a.w * clip.y;
        p.z = u_vp_b.x * clip.w + u_vp_b.y * clip.z;
        p.w = clip.w;
        // GX's viewport transform points Y down and the EFB blit flips the image back,
        // so the flat path negates Y here to cancel it.
        gl_Position = vec4(p.x, -p.y, p.z, p.w);
    }
#ifdef MP_CLIP
    // Ignored unless enabled, which only the morph does.
    vec4 cd = (u_vr == 3) ? u_crop * vec4(a_pos, 1.0) : vec4(1.0);
    gl_ClipDistance[0] = cd.x;
    gl_ClipDistance[1] = cd.y;
    gl_ClipDistance[2] = cd.z;
    gl_ClipDistance[3] = cd.w;
#endif
    gl_PointSize = u_point_size;
    v_col0 = a_col0;
    v_col1 = a_col1;
)";
    for (int i = 0; i < 8; i++) {
        snprintf(buf, sizeof(buf), "    v_tex[%d] = a_tex%d;\n", i, i);
        s += buf;
    }
    s += "}\n";
    return s;
}

std::string gen_pixel_shader(const ShaderKey& k) {
    static const int tev_stop = getenv("MP_TEV_STOP") ? atoi(getenv("MP_TEV_STOP")) : -1;
    static const int tev_reg = getenv("MP_TEV_REG") ? atoi(getenv("MP_TEV_REG")) : 0;
    static const int tev_alpha = getenv("MP_TEV_ALPHA") ? atoi(getenv("MP_TEV_ALPHA")) : 0;
    static const bool no_alpha_test = getenv("MP_NO_ALPHA_TEST") != nullptr;
    static const bool no_fog = getenv("MP_NO_FOG") != nullptr;
    static const bool ras_white = getenv("MP_RAS_WHITE") != nullptr;
    static const int snap_stage = getenv("MP_SNAP") ? atoi(getenv("MP_SNAP")) : -1;
    std::string s;
    char buf[1024];
    // The zero-argument case appends directly: handing a runtime format string to
    // snprintf with no arguments trips -Wformat-security, and it is wasted work anyway.
    auto W = [&](const char* fmt, auto... args) {
        if constexpr (sizeof...(args) == 0) {
            s += fmt;
        } else {
            snprintf(buf, sizeof(buf), fmt, args...);
            s += buf;
        }
    };
    uint32_t nstages = ((k.genmode >> 10) & 15) + 1;
    uint32_t nind = (k.genmode >> 16) & 7;

    s += MP_GLSL_VERSION;
    s += "in vec4 v_col0;\nin vec4 v_col1;\nin vec3 v_tex[8];\n";
    // Only the shaders that fog read it, so every other one is generated exactly as before.
    if ((k.fog & 7) >= 2) s += "in vec2 v_fogz;\n";
    s += "uniform sampler2D u_tex[8];\n";
    s += "uniform ivec4 u_reg[4];\nuniform ivec4 u_konst[4];\n";
    s += "uniform vec2 u_texsize[8];\n";
    s += "uniform ivec3 u_indmtx[6];\n";  // 3 matrices x 2 rows (11-bit signed entries)
    s += "uniform int u_indscale[3];\n";
    s += "uniform ivec2 u_alpharef;\n";
    s += "uniform vec4 u_fog;\n";       // A, C, bmag, bshift
    s += "uniform vec3 u_fogcolor;\n";
    s += "layout(location = 0) out vec4 o_color;\n";
    s += "uniform vec2 u_indcoordscale[4];\n";
    s += "vec2 u_indscalef(int i) { return u_indcoordscale[i]; }\n";
    // A macro, not a function: a sampler array has to be indexed by a constant
    // expression in GLSL ES (and strictly in GLSL 330 as well). Every call site
    // passes a literal map index, so expansion makes the index constant.
    s += "uniform int u_screen_uv;\nuniform vec2 u_screen_px;\n";
    // Converts an indirect offset from the copy's own texels to the eye grab's, for
    // the stages whose coordinate an eye takes over. (1,1) everywhere else, so the
    // flat path is unaffected. See apply_state().
    s += "uniform vec2 u_screen_ripple;\n";
    s += "#define sample_tex(m, uv) ivec4(round(texture(u_tex[m], (uv)) * 255.0))\n";
    s += "void main() {\n";
    s += "  ivec4 prev = u_reg[0], c0 = u_reg[1], c1 = u_reg[2], c2 = u_reg[3];\n";
    s += "  ivec4 col0 = ivec4(round(v_col0 * 255.0)), col1 = ivec4(round(v_col1 * 255.0));\n";
    s += "  ivec4 tex = ivec4(0), ras = ivec4(0), konst = ivec4(0);\n";
    s += "  int alphabump = 0;\n";
    s += "  ivec4 snap = ivec4(0);\n";  // MP_SNAP target; unused unless MP_SHOW=snap
    s += "  vec2 tc_prev = vec2(0.0);\n";

    // Which texgens feed a stage that reads an EFB copy, and so may have their
    // coordinate taken over in an eye. Collected up front because the substituted
    // coordinate is declared with the texgens but only read down in the stages.
    uint32_t subst_coords = 0;
    for (uint32_t st = 0; st < nstages; st++) {
        uint32_t order = k.order[st / 2] >> ((st & 1) * 12);
        if ((k.efb_tex_mask >> (order & 7)) & 1) subst_coords |= 1u << ((order >> 3) & 7);
    }
    // texcoords (projective divide)
    for (uint32_t i = 0; i < 8; i++) {
        if (i < k.num_texgens) W("  vec2 uv%u = v_tex[%u].xy / (v_tex[%u].z == 0.0 ? 1.0 : v_tex[%u].z);\n", i, i, i, i);
        else W("  vec2 uv%u = vec2(0.0);\n", i);
        // A texgen that addresses a copy of the whole frame, or one of the spray's
        // grabs, is a screen-space lookup, and in an eye the screen is this eye's.
        // The substituted coordinate is kept beside the game's rather than replacing
        // it, because an indirect stage sampling an ordinary bump map through the
        // same texgen still wants the game's -- addressing that map by the fragment's
        // position stretches a per-droplet ripple across the whole eye. u_screen_uv is
        // 0 everywhere else, so the flat path takes the game's coordinate either way.
        if ((subst_coords >> i) & 1)
            W("  vec2 suv%u = ((u_screen_uv & %u) != 0) ? gl_FragCoord.xy * u_screen_px : uv%u;\n",
              i, 1u << i, i);
    }
    // indirect stages
    for (uint32_t i = 0; i < nind; i++) {
        uint32_t map = (k.iref >> (6 * i)) & 7, coord = (k.iref >> (6 * i + 3)) & 7;
        W("  ivec4 indtex%u = sample_tex(%u, uv%u * u_indscalef(%u));\n", i, map, coord, i);
    }

    for (uint32_t st = 0; st < nstages; st++) {
        uint32_t order = k.order[st / 2] >> ((st & 1) * 12);
        uint32_t map = order & 7, coord = (order >> 3) & 7;
        bool tex_en = (order >> 6) & 1;
        uint32_t chan = (order >> 7) & 7;
        uint32_t cenv = k.cenv[st], aenv = k.aenv[st];
        uint32_t ind = k.indcmd[st];
        W("  // stage %u\n  {\n", st);
        // A stage that reads an EFB copy is one whose coordinate an eye may take
        // over, so the parts of the lookup that only mean something in the copy's
        // own space are made conditional on it. Every other shader is generated
        // exactly as before, which keeps the flat path byte for byte unchanged.
        const bool subst = (k.efb_tex_mask >> map) & 1;
        const uint32_t cbit = 1u << coord;
        // ---- indirect texture offset ----
        W("    vec2 tc = %s%u * u_texsize[%u];\n", subst ? "suv" : "uv", coord, map);
        uint32_t bt = ind & 3, fmt = (ind >> 2) & 3, bias = (ind >> 4) & 7, bs = (ind >> 7) & 3, mid = (ind >> 9) & 15;
        uint32_t sw = (ind >> 13) & 7, tw = (ind >> 16) & 7;
        bool fb = (ind >> 20) & 1;
        if (bt < nind && (mid != 0 || bs != 0)) {
            static const int mask[4] = {255, 31, 15, 7};
            W("    ivec3 icrd = indtex%u.abg & %d;\n", bt, mask[fmt]);
            if (bs) {
                static const char* bsc[4] = {"", "a", "b", "g"};
                static const int bmask[4] = {0xF8, 0xE0, 0xF0, 0xF8};
                W("    alphabump = indtex%u.%s & %d;\n", bt, bsc[bs], bmask[fmt]);
            }
            int b = fmt == 0 ? -128 : 1;
            if (bias & 1) W("    icrd.x += %d;\n", b);
            if (bias & 2) W("    icrd.y += %d;\n", b);
            if (bias & 4) W("    icrd.z += %d;\n", b);
            if (mid >= 1 && mid <= 3) {
                uint32_t m = mid - 1;
                W("    vec2 ioff = vec2(dot(vec3(u_indmtx[%u]), vec3(icrd)), dot(vec3(u_indmtx[%u]), vec3(icrd))) * exp2(float(u_indscale[%u])) / 1024.0;\n",
                  m * 2, m * 2 + 1, m);
            } else if (mid >= 5 && mid <= 7) {
                // Dynamic matrices: the stage's own coordinate scaled by one component of
                // the indirect sample -- [s 0 0; t 0 0] for S, [0 s 0; 0 t 0] for T.
                W("    vec2 ioff = tc * float(icrd.x) / 256.0 * exp2(float(u_indscale[%u]));\n", mid - 5);
            } else if (mid >= 9 && mid <= 11) {
                W("    vec2 ioff = tc * float(icrd.y) / 256.0 * exp2(float(u_indscale[%u]));\n", mid - 9);
            } else {
                W("    vec2 ioff = vec2(0.0);\n");
            }
        } else {
            W("    vec2 ioff = vec2(0.0);\n");
        }
        // An offset from a static indirect matrix is an absolute displacement in the
        // copy's texels. Substituted, those texels are the eye grab's and each covers
        // a different angle, so it is converted. The dynamic matrices (5..11) scale
        // the stage's own coordinate instead, so their offset is already relative to
        // whatever space the coordinate is in and needs no conversion.
        if (subst && bt < nind && mid >= 1 && mid <= 3)
            W("    if ((u_screen_uv & %u) != 0) ioff *= u_screen_ripple;\n", cbit);
        // A wrap folds the coordinate into the copy's own size. Substituted, the
        // coordinate is the fragment's position on the whole eye target, and folding
        // that tiles the eye into 32-pixel squares instead of addressing it.
        static const float wrapsz[8] = {0, 256, 128, 64, 32, 16, 0.001f, 0};
        const char* wi = "";
        if (subst && (sw || tw)) {
            W("    if ((u_screen_uv & %u) == 0) {\n", cbit);
            wi = "  ";
        }
        if (sw == 6) W("    %stc.x = 0.0;\n", wi);
        else if (sw) W("    %stc.x = mod(tc.x, %.1f);\n", wi, wrapsz[sw]);
        if (tw == 6) W("    %stc.y = 0.0;\n", wi);
        else if (tw) W("    %stc.y = mod(tc.y, %.1f);\n", wi, wrapsz[tw]);
        if (*wi) W("    }\n");
        W("    tc += ioff;\n");
        // "Add previous" carries the previous stage's whole coordinate, not just its
        // offset: a stage can zero its own coordinate with a wrap of 0 and look up at
        // the stage before's plus a bump, which is how the racer's reflection is warped.
        if (fb) W("    tc += tc_prev;\n");
        W("    tc_prev = tc;\n");
        // ---- texture ----
        if (tex_en) {
            uint32_t tswap = (aenv >> 2) & 3;
            W("    tex = sample_tex(%u, tc / u_texsize[%u]).%s;\n", map, map, swizzle(k, tswap).c_str());
        } else {
            W("    tex = ivec4(255);\n");
        }
        // ---- rasterized color ----
        {
            uint32_t rswap = aenv & 3;
            std::string sw_ = swizzle(k, rswap);
            // MP_RAS_WHITE=1 forces the rasterized colour opaque white, to test whether
            // a bad image comes from the CPU-side vertex colour/lighting in xf.cpp
            // rather than from the TEV program itself.
            if (ras_white) W("    ras = ivec4(255);\n");
            else switch (chan) {
            case 0: W("    ras = col0.%s;\n", sw_.c_str()); break;
            case 1: W("    ras = col1.%s;\n", sw_.c_str()); break;
            case 5: W("    ras = ivec4(alphabump);\n"); break;
            case 6: W("    ras = ivec4(alphabump | (alphabump >> 5));\n"); break;
            default: W("    ras = ivec4(0);\n"); break;
            }
        }
        // ---- konst ----
        {
            uint32_t ks = k.ksel[st / 2];
            uint32_t kc = (st & 1) ? (ks >> 14) & 31 : (ks >> 4) & 31;
            uint32_t ka = (st & 1) ? (ks >> 19) & 31 : (ks >> 9) & 31;
            W("    konst = ivec4(%s, %s);\n", konst_color(kc).c_str(), konst_alpha(ka).c_str());
        }
        // ---- color combiner ----
        {
            uint32_t d = cenv & 15, c = (cenv >> 4) & 15, b = (cenv >> 8) & 15, a = (cenv >> 12) & 15;
            uint32_t bias_ = (cenv >> 16) & 3, op = (cenv >> 18) & 1, clamp = (cenv >> 19) & 1, scale = (cenv >> 20) & 3, dest = (cenv >> 22) & 3;
            W("    ivec3 ca = %s & 255, cb = %s & 255, cc = %s & 255, cd = %s;\n", kColorIn[a], kColorIn[b], kColorIn[c], kColorIn[d]);
            if (bias_ != 3) {
                static const char* biast[3] = {"", " + 128", " - 128"};
                static const char* sl[4] = {"", " << 1", " << 2", ""};
                static const char* lb[4] = {"", " + 128", "", " + 127"};
                // The lerp weights both terms rather than using GX's (b - a) * c form.
                // The two are algebraically identical, but (b - a) is negative whenever
                // b < a, and GLSL ES leaves >> on a negative value undefined -- a driver
                // may reassociate the shift onto that term. On an Adreno 740 the original
                // form returned zero for most of the screen, which is what made the title
                // screen render black.
                W("    ivec3 ccx = cc + (cc >> 7);\n");
                W("    ivec3 cr = (((cd%s)%s) %c ((((ca * (256 - ccx) + cb * ccx)%s)%s) >> 8))%s;\n",
                  biast[bias_], sl[scale], op ? '-' : '+', sl[scale],
                  lb[2 * op + (scale != 3 ? 1 : 0)], scale == 3 ? " / 2" : "");
            } else {
                uint32_t mode = (scale << 1) | op;
                switch (mode) {
                case 0: W("    ivec3 cr = cd + ((ca.r > cb.r) ? cc : ivec3(0));\n"); break;
                case 1: W("    ivec3 cr = cd + ((ca.r == cb.r) ? cc : ivec3(0));\n"); break;
                case 2: W("    ivec3 cr = cd + (((ca.g << 8 | ca.r) > (cb.g << 8 | cb.r)) ? cc : ivec3(0));\n"); break;
                case 3: W("    ivec3 cr = cd + (((ca.g << 8 | ca.r) == (cb.g << 8 | cb.r)) ? cc : ivec3(0));\n"); break;
                case 4: W("    ivec3 cr = cd + (((ca.b << 16 | ca.g << 8 | ca.r) > (cb.b << 16 | cb.g << 8 | cb.r)) ? cc : ivec3(0));\n"); break;
                case 5: W("    ivec3 cr = cd + (((ca.b << 16 | ca.g << 8 | ca.r) == (cb.b << 16 | cb.g << 8 | cb.r)) ? cc : ivec3(0));\n"); break;
                case 6: W("    ivec3 cr = cd + ivec3(greaterThan(ca, cb)) * cc;\n"); break;
                case 7: W("    ivec3 cr = cd + ivec3(equal(ca, cb)) * cc;\n"); break;
                }
            }
            if (clamp) W("    %s.rgb = clamp(cr, 0, 255);\n", kDest[dest]);
            else W("    %s.rgb = clamp(cr, -1024, 1023);\n", kDest[dest]);
        }
        // ---- alpha combiner ----
        {
            uint32_t d = (aenv >> 4) & 7, c = (aenv >> 7) & 7, b = (aenv >> 10) & 7, a = (aenv >> 13) & 7;
            uint32_t bias_ = (aenv >> 16) & 3, op = (aenv >> 18) & 1, clamp = (aenv >> 19) & 1, scale = (aenv >> 20) & 3, dest = (aenv >> 22) & 3;
            W("    int aa = %s & 255, ab = %s & 255, ac = %s & 255, ad = %s;\n", kAlphaIn[a], kAlphaIn[b], kAlphaIn[c], kAlphaIn[d]);
            if (bias_ != 3) {
                static const char* biast[3] = {"", " + 128", " - 128"};
                static const char* sl[4] = {"", " << 1", " << 2", ""};
                static const char* lb[4] = {"", " + 128", "", " + 127"};
                // Same weighted form as the colour combiner, for the same reason: (ab - aa)
                // is negative whenever ab < aa, and >> on a negative is undefined in
                // GLSL ES. The rounding-bias condition also matched on `scale == 3` here
                // where the colour path uses `scale != 3`; the two are the same equation,
                // so this was inverted and is corrected to agree.
                W("    int acx = ac + (ac >> 7);\n");
                W("    int ar = (((ad%s)%s) %c ((((aa * (256 - acx) + ab * acx)%s)%s) >> 8))%s;\n",
                  biast[bias_], sl[scale], op ? '-' : '+', sl[scale],
                  lb[2 * op + (scale != 3 ? 1 : 0)], scale == 3 ? " / 2" : "");
            } else {
                uint32_t mode = (scale << 1) | op;
                switch (mode) {
                case 0: W("    int ar = ad + ((ca.r > cb.r) ? ac : 0);\n"); break;
                case 1: W("    int ar = ad + ((ca.r == cb.r) ? ac : 0);\n"); break;
                case 2: W("    int ar = ad + (((ca.g << 8 | ca.r) > (cb.g << 8 | cb.r)) ? ac : 0);\n"); break;
                case 3: W("    int ar = ad + (((ca.g << 8 | ca.r) == (cb.g << 8 | cb.r)) ? ac : 0);\n"); break;
                case 4: W("    int ar = ad + (((ca.b << 16 | ca.g << 8 | ca.r) > (cb.b << 16 | cb.g << 8 | cb.r)) ? ac : 0);\n"); break;
                case 5: W("    int ar = ad + (((ca.b << 16 | ca.g << 8 | ca.r) == (cb.b << 16 | cb.g << 8 | cb.r)) ? ac : 0);\n"); break;
                case 6: W("    int ar = ad + ((aa > ab) ? ac : 0);\n"); break;
                case 7: W("    int ar = ad + ((aa == ab) ? ac : 0);\n"); break;
                }
            }
            if (clamp) W("    %s.a = clamp(ar, 0, 255);\n", kDest[dest]);
            else W("    %s.a = clamp(ar, -1024, 1023);\n", kDest[dest]);
        }
        s += "  }\n";
        // MP_SNAP=N copies prev after stage N into `snap`, which MP_SHOW=snap displays
        // at the end. This bisects the chain without altering control flow, so unlike
        // MP_TEV_STOP it cannot change what it measures.
        if (snap_stage >= 0 && (int)st == snap_stage) s += "  snap = prev;\n";
        // Debug: MP_TEV_STOP=N ends the chain after stage N and writes the running
        // result straight out, to bisect which stage a bad image comes from.
        // MP_TEV_REG picks which register to show (0=prev, 1=c0, 2=c1, 3=c2), and
        // MP_TEV_ALPHA=1 shows that register's alpha as greyscale instead of its rgb.
        if (tev_stop >= 0 && (int)st == tev_stop) {
            // 0-3 pick a TEV register, 4 the texel this stage sampled, 5 the
            // rasterized colour, 6 the konst. Showing the texel separates a bad
            // texture or texcoord from bad combiner inputs.
            static const char* kShow[7] = {"prev", "c0", "c1", "c2", "tex", "ras", "konst"};
            const char* reg = kShow[tev_reg % 7];
            if (tev_alpha) W("  o_color = vec4(vec3(float(%s.a & 255) / 255.0), 1.0);\n", reg);
            else W("  o_color = vec4(vec3(%s.rgb & 255) / 255.0, 1.0);\n", reg);
            s += "  return;\n";
            break;
        }
    }
    s += "  ivec4 outc = prev & 255;\n";

    // alpha test
    {
        uint32_t f0 = k.alpha_func & 7, f1 = (k.alpha_func >> 3) & 7, logic = (k.alpha_func >> 6) & 3;
        char t0[128], t1[128];
        snprintf(t0, sizeof(t0), kCompare[f0], "outc.a", "u_alpharef.x");
        snprintf(t1, sizeof(t1), kCompare[f1], "outc.a", "u_alpharef.y");
        static const char* lop[4] = {"&&", "||", "!=", "=="};
        // MP_NO_ALPHA_TEST=1 skips it, to tell a discard apart from a shading bug.
        if (!(f0 == 7 && f1 == 7 && logic == 0) && !no_alpha_test)
            W("  if (!(%s %s %s)) discard;\n", t0, lop[logic], t1);
    }
    // fog
    {
        uint32_t fsel = k.fog & 7;
        bool ortho = (k.fog >> 3) & 1;
        if (fsel >= 2 && !no_fog) {
            // Not gl_FragCoord.z: that is the depth of whatever projection actually drew
            // this fragment, and in an eye that is the headset's, not the game's. v_fogz
            // carries the game's own screen z instead -- see the vertex shader. Clamping
            // stands in for the depth range the hardware would have applied, and covers
            // the geometry an eye can see outside the game's frustum, whose reconstructed
            // z lands outside [0,1] (behind the game's camera it even goes negative).
            s += "  float zs = clamp(v_fogz.x / v_fogz.y, 0.0, 1.0) * 16777215.0;\n";
            if (ortho) s += "  float ze = u_fog.x * (zs / 16777215.0);\n";
            else s += "  float ze = (u_fog.x * 16777216.0) / (u_fog.z - floor(zs / exp2(u_fog.w)));\n";
            s += "  float fog = clamp(ze - u_fog.y, 0.0, 1.0);\n";
            switch (fsel) {
            case 4: s += "  fog = 1.0 - exp2(-8.0 * fog);\n"; break;
            case 5: s += "  fog = 1.0 - exp2(-8.0 * fog * fog);\n"; break;
            case 6: s += "  fog = exp2(-8.0 * (1.0 - fog));\n"; break;
            case 7: s += "  fog = exp2(-8.0 * (1.0 - fog) * (1.0 - fog));\n"; break;
            default: break;
            }
            s += "  vec3 fc = mix(vec3(outc.rgb), u_fogcolor * 255.0, fog);\n";
            s += "  outc.rgb = ivec3(round(fc));\n";
        }
    }
    s += "  o_color = vec4(outc) / 255.0;\n";
    if (getenv("MP_FLAT")) s += "  o_color = vec4(1.0, 0.0, 1.0, 1.0);\n";
    // MP_SHOW=prev|c0|c1|c2|konst (plus MP_SHOW_ALPHA=1) replaces the final colour
    // with a TEV value. Unlike MP_TEV_STOP this changes no control flow at all --
    // every stage, the alpha test and fog still run exactly as they normally would --
    // so it cannot perturb what it is measuring.
    if (const char* show = getenv("MP_SHOW")) {
        // "probe" packs stage 12's three inputs into one image so they are measured in
        // a single run, on the same pixels of the same frame: red is prev as it entered
        // the stage (MP_SNAP), green is c1's red, blue is c1's alpha. Comparing these
        // across separate runs is unsound because the attract sequence drifts.
        if (!strcmp(show, "probe"))
            s += "  o_color = vec4(float(snap.r & 255) / 255.0, float(c1.r & 255) / 255.0,"
                 " float(c1.a & 255) / 255.0, 1.0);\n";
        // "probe2" shows one stage's input, its blend factor and its output together:
        // red is prev entering stage MP_SNAP+1, green is c1's alpha (the factor), blue
        // is prev at the end. If red is bright and green is zero, blue must match red.
        else if (!strcmp(show, "probe2"))
            s += "  o_color = vec4(float(snap.r & 255) / 255.0, float(c1.a & 255) / 255.0,"
                 " float(prev.r & 255) / 255.0, 1.0);\n";
        else if (getenv("MP_SHOW_ALPHA")) W("  o_color = vec4(vec3(float(%s.a & 255) / 255.0), 1.0);\n", show);
        else W("  o_color = vec4(vec3(%s.rgb & 255) / 255.0, 1.0);\n", show);
    }
    s += "}\n";
    return s;
}

}  // namespace gx
