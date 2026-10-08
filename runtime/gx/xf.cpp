// GX front end (runs on the guest thread): vertex decoding, XF (transform, lighting,
// texgen) on the CPU, primitive assembly, pixel-state snapshots and EFB copies.
#include "../runtime.h"
#include "gx.h"
#include "render.h"
#include "texture.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <condition_variable>
#include <deque>

namespace gx {

std::atomic<uint32_t> g_frames_submitted{0};
static std::unique_ptr<Batch> g_batch;
static uint32_t g_tev_reg[4][2], g_tev_konst[4][2];

// MP_FRAMETIME=1 prints one line per presented frame with where the guest thread's time
// went: the interval since the last present, how much of it was the GX front end (vertex
// decode, transform and lighting, texture hashing and decoding), the batch's shape, and
// how full the submission queue was -- a full queue means the guest was waiting on the
// renderer, not the other way round. The renderer prints its own line per batch.
static const bool g_frametime = getenv("MP_FRAMETIME") != nullptr;
static double g_fe_ms, g_tex_ms;
static uint32_t g_fe_draws;
static std::chrono::steady_clock::time_point g_last_present;
static inline double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}
size_t queue_depth();

// ---------------------------------------------------------------------------
// Submission queue
// ---------------------------------------------------------------------------
static std::mutex g_q_mutex;
static std::condition_variable g_q_cv;
static std::deque<std::unique_ptr<Batch>> g_queue;

// A batch holds every vertex of a frame -- around 10 MB during a race -- so the queue
// has to be a hard bound, not a hint. If the renderer is slower than the guest (a
// mobile GPU, say) an advisory wait that queues anyway grows this by tens of megabytes
// a second; on a Quest 3 that reached 3.8 GB and the app was killed by lowmemorykiller.
static constexpr size_t kMaxQueuedBatches = 8;

void submit_batch(std::unique_ptr<Batch> b) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    // Wait for room, which throttles the guest to the renderer's rate. The timeout is
    // only a safety valve: if nothing is consuming at all (no window, a stalled render
    // thread) the guest must not block forever, so drop the oldest frame instead.
    if (!g_q_cv.wait_for(lk, std::chrono::seconds(2),
                         [] { return g_queue.size() < kMaxQueuedBatches; })) {
        if (!g_queue.empty()) g_queue.pop_front();
    }
    g_queue.push_back(std::move(b));
    g_q_cv.notify_all();
}

size_t queue_depth() {
    std::lock_guard<std::mutex> lk(g_q_mutex);
    return g_queue.size();
}

std::unique_ptr<Batch> take_batch(int timeout_ms) {
    std::unique_lock<std::mutex> lk(g_q_mutex);
    if (!g_q_cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [] { return !g_queue.empty(); })) return nullptr;
    auto b = std::move(g_queue.front());
    g_queue.pop_front();
    g_q_cv.notify_all();
    return b;
}

// The storage pool; see Batch in render.h. Bounded so that a frontend that lets batches
// pile up somewhere does not keep that many frames' worth of capacity alive.
// Never destroyed: batches are recycled from static destructors at exit, and the pool
// must still be there when they are.
static std::mutex& pool_mutex() { static auto* m = new std::mutex; return *m; }
static std::vector<Batch>& pool() { static auto* p = new std::vector<Batch>; return *p; }
static constexpr size_t kMaxPooled = 12;

Batch::~Batch() {
    if (pooled) return;
    Batch b;
    b.pooled = true;
    b.cmds.swap(cmds);
    b.verts.swap(verts);
    b.indices.swap(indices);
    b.states.swap(states);
    b.mtxs.swap(mtxs);
    b.new_textures.swap(new_textures);
    // Emptied now, on the thread that is done with it: the decoded textures go with it.
    b.cmds.clear();
    b.verts.clear();
    b.indices.clear();
    b.states.clear();
    b.mtxs.clear();
    b.new_textures.clear();
    std::lock_guard<std::mutex> lk(pool_mutex());
    if (pool().size() < kMaxPooled) pool().push_back(std::move(b));
}

static Batch& batch() {
    if (!g_batch) {
        g_batch = std::make_unique<Batch>();
        std::lock_guard<std::mutex> lk(pool_mutex());
        if (!pool().empty()) {
            *g_batch = std::move(pool().back());
            pool().pop_back();
            g_batch->pooled = false;
        }
    }
    return *g_batch;
}

// MP_MTXLOG=<n> dumps the nth presented frame's draws; see draw_impl.
static const uint32_t g_mtxlog = getenv("MP_MTXLOG") ? (uint32_t)atoi(getenv("MP_MTXLOG")) : 0;
static uint32_t g_mtx_draw;
// Whether this is the frame MP_MTXLOG dumps.
static bool mtxlog_frame() { return g_mtxlog && g_frame_counter == g_mtxlog; }

static void flush_batch() {
    if (g_batch && !g_batch->cmds.empty()) submit_batch(std::move(g_batch));
    g_batch.reset();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static inline float xf_f(uint32_t i) {
    uint32_t v = g_state.xf_mem[i & 0x7FF];
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static inline float xfr_f(uint32_t reg) {
    uint32_t v = g_state.xf_regs[reg & 0xFF];
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static inline float read_comp(const uint8_t*& p, uint32_t fmt, float scale) {
    switch (fmt) {
    case 0: return (float)(*p++) * scale;
    case 1: return (float)(int8_t)(*p++) * scale;
    case 2: { float v = (float)rd16(p) * scale; p += 2; return v; }
    case 3: { float v = (float)(int16_t)rd16(p) * scale; p += 2; return v; }
    case 4: { uint32_t u = rd32(p); p += 4; float f; memcpy(&f, &u, 4); return f; }
    default: return 0;
    }
}
static inline uint32_t comp_size(uint32_t fmt) { static const uint32_t s[8] = {1, 1, 2, 2, 4, 0, 0, 0}; return s[fmt & 7]; }

// color as 0xRRGGBBAA
static uint32_t read_color(const uint8_t* p, uint32_t fmt) {
    switch (fmt) {
    case 0: { uint16_t v = rd16(p); uint32_t r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
              return (((r << 3) | (r >> 2)) << 24) | (((g << 2) | (g >> 4)) << 16) | (((b << 3) | (b >> 2)) << 8) | 0xFF; }
    case 1: case 2: return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | 0xFF;
    case 3: { uint16_t v = rd16(p);
              return ((((v >> 12) & 15) * 17) << 24) | ((((v >> 8) & 15) * 17) << 16) | ((((v >> 4) & 15) * 17) << 8) | ((v & 15) * 17); }
    case 4: { uint32_t v = ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
              auto e = [](uint32_t x) { return (x << 2) | (x >> 4); };
              return (e((v >> 18) & 63) << 24) | (e((v >> 12) & 63) << 16) | (e((v >> 6) & 63) << 8) | e(v & 63); }
    case 5: return rd32(p);
    }
    return 0xFFFFFFFF;
}
static uint32_t color_size(uint32_t fmt) { static const uint32_t s[8] = {2, 3, 4, 2, 3, 4, 4, 4}; return s[fmt & 7]; }

struct InVertex {
    float pos[3];
    float nrm[3], bin[3], tan[3];
    uint32_t col[2];
    float tex[8][2];
    uint8_t pnmtx;
    uint8_t texmtx[8];
};

struct Layout {
    uint32_t pnmtx, texmtx;       // presence bits
    uint32_t pos_desc, pos_fmt, pos_cnt; float pos_scale;
    uint32_t nrm_desc, nrm_fmt; bool nbt, idx3; float nrm_scale;
    uint32_t col_desc[2], col_fmt[2];
    uint32_t tex_desc[8], tex_fmt[8], tex_cnt[8]; float tex_scale[8];
};

static Layout make_layout(int vat) {
    const uint32_t* cp = g_state.cp;
    uint32_t lo = cp[0x50], hi = cp[0x60], a = cp[0x70 + vat], b = cp[0x80 + vat], c = cp[0x90 + vat];
    Layout L{};
    L.pnmtx = lo & 1;
    L.texmtx = (lo >> 1) & 0xFF;
    L.pos_desc = (lo >> 9) & 3; L.pos_cnt = (a & 1) ? 3 : 2; L.pos_fmt = (a >> 1) & 7;
    L.pos_scale = 1.0f / (float)(1u << ((a >> 4) & 31));
    L.nrm_desc = (lo >> 11) & 3; L.nbt = (a >> 9) & 1; L.nrm_fmt = (a >> 10) & 7; L.idx3 = (a >> 31) & 1;
    L.nrm_scale = (L.nrm_fmt == 1 || L.nrm_fmt == 0) ? 1.0f / 64 : (L.nrm_fmt == 4 ? 1.0f : 1.0f / 16384);
    L.col_desc[0] = (lo >> 13) & 3; L.col_fmt[0] = (a >> 14) & 7;
    L.col_desc[1] = (lo >> 15) & 3; L.col_fmt[1] = (a >> 18) & 7;
    uint32_t tc[8] = {(a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1};
    uint32_t tf[8] = {(a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7};
    uint32_t ts[8] = {(a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31};
    for (int i = 0; i < 8; i++) {
        L.tex_desc[i] = (hi >> (2 * i)) & 3;
        L.tex_cnt[i] = tc[i] ? 2 : 1;
        L.tex_fmt[i] = tf[i];
        L.tex_scale[i] = 1.0f / (float)(1u << ts[i]);
    }
    return L;
}

// Layouts come from the CP registers only, which the game sets per model rather than
// per draw, so they are cached against State::cp_gen.
static Layout g_layouts[8];
static uint32_t g_layouts_valid, g_layouts_gen;

static const Layout& layout_for(int vat) {
    if (g_layouts_gen != g_state.cp_gen) {
        g_layouts_valid = 0;
        g_layouts_gen = g_state.cp_gen;
    }
    if (!(g_layouts_valid & (1u << vat))) {
        g_layouts[vat] = make_layout(vat);
        g_layouts_valid |= 1u << vat;
    }
    return g_layouts[vat];
}

// Resolve an attribute pointer: direct data or indexed array element.
static const uint8_t* attr_ptr(const uint8_t*& p, uint32_t desc, int array, uint32_t direct_size) {
    if (desc == 1) { const uint8_t* r = p; p += direct_size; return r; }
    uint32_t idx;
    if (desc == 2) idx = *p++;
    else { idx = rd16(p); p += 2; }
    uint32_t base = g_state.cp[0xA0 + array], stride = g_state.cp[0xB0 + array];
    return phys_ptr(base + idx * stride);
}

// The matrix indices a vertex that does not carry its own falls back to. XF registers,
// so they belong to the per-draw plan rather than to the CP-derived Layout.
static uint8_t g_defpnmtx, g_deftex[8];

static void decode_vertex(const Layout& L, const uint8_t*& p, InVertex& v) {
    v.pnmtx = L.pnmtx ? *p++ : g_defpnmtx;
    memcpy(v.texmtx, g_deftex, sizeof(v.texmtx));
    if (L.texmtx)
        for (int i = 0; i < 8; i++)
            if (L.texmtx & (1u << i)) v.texmtx[i] = *p++;
    if (L.pos_desc) {
        const uint8_t* q = attr_ptr(p, L.pos_desc, 0, L.pos_cnt * comp_size(L.pos_fmt));
        v.pos[0] = read_comp(q, L.pos_fmt, L.pos_scale);
        v.pos[1] = read_comp(q, L.pos_fmt, L.pos_scale);
        v.pos[2] = L.pos_cnt == 3 ? read_comp(q, L.pos_fmt, L.pos_scale) : 0.0f;
    }
    if (L.nrm_desc) {
        uint32_t csz = comp_size(L.nrm_fmt);
        if (L.nbt && L.idx3 && L.nrm_desc != 1) {
            float* dst[3] = {v.nrm, v.bin, v.tan};
            for (int k = 0; k < 3; k++) {
                const uint8_t* q = attr_ptr(p, L.nrm_desc, 1, 0);
                q += k * 3 * csz;
                for (int j = 0; j < 3; j++) dst[k][j] = read_comp(q, L.nrm_fmt, L.nrm_scale);
            }
        } else {
            const uint8_t* q = attr_ptr(p, L.nrm_desc, 1, (L.nbt ? 9 : 3) * csz);
            for (int j = 0; j < 3; j++) v.nrm[j] = read_comp(q, L.nrm_fmt, L.nrm_scale);
            if (L.nbt) {
                for (int j = 0; j < 3; j++) v.bin[j] = read_comp(q, L.nrm_fmt, L.nrm_scale);
                for (int j = 0; j < 3; j++) v.tan[j] = read_comp(q, L.nrm_fmt, L.nrm_scale);
            }
        }
    }
    for (int ci = 0; ci < 2; ci++) {
        if (!L.col_desc[ci]) { v.col[ci] = 0xFFFFFFFF; continue; }
        const uint8_t* q = attr_ptr(p, L.col_desc[ci], 2 + ci, color_size(L.col_fmt[ci]));
        v.col[ci] = read_color(q, L.col_fmt[ci]);
    }
    for (int t = 0; t < 8; t++) {
        if (!L.tex_desc[t]) { v.tex[t][0] = v.tex[t][1] = 0; continue; }
        const uint8_t* q = attr_ptr(p, L.tex_desc[t], 4 + t, L.tex_cnt[t] * comp_size(L.tex_fmt[t]));
        v.tex[t][0] = read_comp(q, L.tex_fmt[t], L.tex_scale[t]);
        v.tex[t][1] = L.tex_cnt[t] == 2 ? read_comp(q, L.tex_fmt[t], L.tex_scale[t]) : 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Lighting
// ---------------------------------------------------------------------------
struct Vec3 { float x, y, z; };
static inline Vec3 v3(float x, float y, float z) { return {x, y, z}; }
static inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
// One division and three multiplies rather than three divisions. This is the hottest
// floating-point operation in the front end -- once for the normal and once per light
// per vertex -- and a division is not pipelined on the cores this runs on.
//
// The reciprocal costs up to an ulp against dividing three times. The result is a
// direction fed to the lighting, whose output is quantised to eight bits per channel
// before it leaves this file, so the difference cannot reach a pixel; if an accuracy
// question ever lands here, this is the line to put back.
static inline Vec3 normalize(Vec3 a) {
    float l = sqrtf(dot(a, a));
    if (!(l > 0)) return a;
    const float inv = 1.0f / l;
    return Vec3{a.x * inv, a.y * inv, a.z * inv};
}

// Eight-bit channel to float, by table. Four divisions per unpack and up to eight
// unpacks per vertex made this one of the most expensive things in the transform, and a
// table of the 256 possible results is exactly equal to the division it replaces.
static const float kU8toF[256] = {
#define MP_U8_ROW(n) (n) / 255.0f, (n + 1) / 255.0f, (n + 2) / 255.0f, (n + 3) / 255.0f,                      (n + 4) / 255.0f, (n + 5) / 255.0f, (n + 6) / 255.0f, (n + 7) / 255.0f
#define MP_U8_ROW32(n) MP_U8_ROW(n), MP_U8_ROW(n + 8), MP_U8_ROW(n + 16), MP_U8_ROW(n + 24)
    MP_U8_ROW32(0),   MP_U8_ROW32(32),  MP_U8_ROW32(64),  MP_U8_ROW32(96),
    MP_U8_ROW32(128), MP_U8_ROW32(160), MP_U8_ROW32(192), MP_U8_ROW32(224),
#undef MP_U8_ROW32
#undef MP_U8_ROW
};

static inline void unpack_rgba(uint32_t c, float out[4]) {
    out[0] = kU8toF[(c >> 24) & 0xFF]; out[1] = kU8toF[(c >> 16) & 0xFF];
    out[2] = kU8toF[(c >> 8) & 0xFF];  out[3] = kU8toF[c & 0xFF];
}

// A light's parameters, read out of XF memory once per draw rather than once per vertex
// per light. Twelve unpacked floats, a colour unpack and a normalise is a great deal of
// work to repeat for every vertex when none of it changes within a draw, and this game
// lights most of what it draws.
struct LightParams {
    float col[4];
    Vec3 pos, dir, cosatt, distatt, distatt_n;
};
static LightParams g_lights[8];
static uint32_t g_lights_loaded;   // which of g_lights are valid for the current draw

static void load_light(int i) {
    const uint32_t base = 0x600 + i * 0x10;
    LightParams& L = g_lights[i];
    unpack_rgba(g_state.xf_mem[base + 3], L.col);
    L.pos = v3(xf_f(base + 10), xf_f(base + 11), xf_f(base + 12));
    L.dir = v3(xf_f(base + 13), xf_f(base + 14), xf_f(base + 15));
    L.cosatt = v3(xf_f(base + 4), xf_f(base + 5), xf_f(base + 6));
    L.distatt = v3(xf_f(base + 7), xf_f(base + 8), xf_f(base + 9));
    L.distatt_n = normalize(L.distatt);
}

// Compute light accumulation for one channel component set (rgb or alpha).
static void light_channel(uint32_t ctrl, Vec3 pos, Vec3 nrm, float lacc[4], bool alpha) {
    uint32_t mask = ((ctrl >> 2) & 0xF) | (((ctrl >> 11) & 0xF) << 4);
    uint32_t diff = (ctrl >> 7) & 3;
    uint32_t attn = (ctrl >> 9) & 3;
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1u << i))) continue;
        if (!(g_lights_loaded & (1u << i))) {
            load_light(i);
            g_lights_loaded |= 1u << i;
        }
        const LightParams& L = g_lights[i];
        const float* col = L.col;
        const Vec3 lpos = L.pos, ldir_param = L.dir, cosatt = L.cosatt, distatt = L.distatt;
        Vec3 ldir;
        float a = 1.0f;
        if (attn == 1) {  // specular
            ldir = normalize(sub(lpos, pos));  // GXInitSpecularDir puts the light "far away" along -dir
            float nd = dot(nrm, ldir);
            float h = nd >= 0 ? std::max(0.0f, dot(nrm, ldir_param)) : 0.0f;
            Vec3 da = diff == 0 ? distatt : L.distatt_n;
            float num = std::max(0.0f, cosatt.x + cosatt.y * h + cosatt.z * h * h);
            float den = da.x + da.y * h + da.z * h * h;
            a = den != 0 ? num / den : 0.0f;
        } else if (attn == 3) {  // spot
            Vec3 d = sub(lpos, pos);
            float dist2 = dot(d, d), dist = sqrtf(dist2);
            ldir = dist > 0 ? Vec3{d.x / dist, d.y / dist, d.z / dist} : d;
            float c = std::max(0.0f, dot(ldir, ldir_param));
            float num = std::max(0.0f, cosatt.x + cosatt.y * c + cosatt.z * c * c);
            float den = distatt.x + distatt.y * dist + distatt.z * dist2;
            a = den != 0 ? num / den : 0.0f;
        } else {
            ldir = normalize(sub(lpos, pos));
        }
        float d = 1.0f;
        if (diff == 1) d = dot(ldir, nrm);
        else if (diff == 2) d = std::max(0.0f, dot(ldir, nrm));
        float f = a * d;
        if (alpha) lacc[3] += f * col[3];
        else { lacc[0] += f * col[0]; lacc[1] += f * col[1]; lacc[2] += f * col[2]; }
    }
}

// One colour channel's registers, unpacked once per draw. The material and ambient
// registers were unpacked four times per vertex between them, and all four readings are
// of the same two registers.
struct ChanParams {
    uint32_t cctrl, actrl;
    float mat[4], amb[4];
};
static ChanParams g_chan[2];

static void load_chan(int chan) {
    ChanParams& C = g_chan[chan];
    C.cctrl = g_state.xf_regs[0x0E + chan];
    C.actrl = g_state.xf_regs[0x10 + chan];
    unpack_rgba(g_state.xf_regs[0x0C + chan], C.mat);
    unpack_rgba(g_state.xf_regs[0x0A + chan], C.amb);
}

static uint32_t compute_color(int chan, const InVertex& v, Vec3 pos, Vec3 nrm) {
    const ChanParams& C = g_chan[chan];
    const uint32_t cctrl = C.cctrl, actrl = C.actrl;
    // The vertex's own colour, needed only where one of the four selectors asks for it.
    float vcol[4];
    if ((cctrl | actrl) & 0x41) unpack_rgba(v.col[chan], vcol);
    const float* mat = (cctrl & 1) ? vcol : C.mat;
    const float* amb = (cctrl & 0x40) ? vcol : C.amb;
    const float* amat = (actrl & 1) ? vcol : C.mat;
    const float* aamb = (actrl & 0x40) ? vcol : C.amb;
    float out[4];
    // color
    float rgb[3] = {mat[0], mat[1], mat[2]};
    if (cctrl & 2) {
        float lacc[4] = {amb[0], amb[1], amb[2], 0};
        light_channel(cctrl, pos, nrm, lacc, false);
        for (int k = 0; k < 3; k++) rgb[k] = mat[k] * std::clamp(lacc[k], 0.0f, 1.0f);
    }
    // alpha
    float al = amat[3];
    if (actrl & 2) {
        float lacc[4] = {0, 0, 0, aamb[3]};
        light_channel(actrl, pos, nrm, lacc, true);
        al = amat[3] * std::clamp(lacc[3], 0.0f, 1.0f);
    }
    out[0] = rgb[0]; out[1] = rgb[1]; out[2] = rgb[2]; out[3] = al;
    uint32_t r = 0;
    for (int k = 0; k < 4; k++) r = (r << 8) | (uint32_t)lrintf(std::clamp(out[k], 0.0f, 1.0f) * 255.0f);
    return r;
}

// ---------------------------------------------------------------------------
// Transform one vertex
// ---------------------------------------------------------------------------

// What the transform reads that cannot change within a draw: the channel and texgen
// counts, and each texgen's bit-packed description. A draw here averages under eight
// vertices, so unpacking these per vertex was a large share of the transform.
struct GenParams {
    uint32_t proj, form, type, row, srcrow, light, post, postnorm;
};
// Whether any texgen embosses, which is the only thing that uses the transformed
// binormal and tangent. Without one, normalising them per vertex -- two square roots
// and six divisions -- produced values nothing read.
static bool g_need_tangents;
static GenParams g_gen[8];
static uint32_t g_numcol, g_ntex;
static bool g_dualtex;

// The position and normal matrices for the pnmtx last seen, so a draw whose vertices all
// index one matrix -- every draw in this game -- reads it out of XF memory once. Reset
// per draw, since a matrix may be loaded between draws.
static uint32_t g_mtx_cached;
static float g_posmtx[12], g_nrmmtx[9];

static void load_matrices(uint8_t pnmtx) {
    const uint32_t m = (pnmtx & 63) * 4;
    for (int i = 0; i < 12; i++) g_posmtx[i] = xf_f(m + i);
    const uint32_t n = 0x400 + ((pnmtx & 31) * 3);
    for (int i = 0; i < 9; i++) g_nrmmtx[i] = xf_f(n + i);
    g_mtx_cached = pnmtx;
}

// Called once per draw, before the vertex loop.
static void load_xf_plan() {
    g_numcol = g_state.xf_regs[0x09] & 3;
    g_ntex = g_state.xf_regs[0x3F] & 15;
    g_dualtex = g_state.xf_regs[0x12] & 1;
    const uint32_t mi0 = g_state.xf_regs[0x18], mi1 = g_state.xf_regs[0x19];
    g_defpnmtx = (uint8_t)(mi0 & 63);
    g_deftex[0] = (uint8_t)((mi0 >> 6) & 63);  g_deftex[1] = (uint8_t)((mi0 >> 12) & 63);
    g_deftex[2] = (uint8_t)((mi0 >> 18) & 63); g_deftex[3] = (uint8_t)((mi0 >> 24) & 63);
    g_deftex[4] = (uint8_t)(mi1 & 63);         g_deftex[5] = (uint8_t)((mi1 >> 6) & 63);
    g_deftex[6] = (uint8_t)((mi1 >> 12) & 63); g_deftex[7] = (uint8_t)((mi1 >> 18) & 63);
    g_need_tangents = false;
    for (uint32_t t = 0; t < g_ntex && t < 8; t++) {
        const uint32_t info = g_state.xf_regs[0x40 + t];
        const uint32_t post = g_state.xf_regs[0x50 + t];
        GenParams& G = g_gen[t];
        G.proj = (info >> 1) & 1;
        G.form = (info >> 2) & 1;
        G.type = (info >> 4) & 7;
        G.row = (info >> 7) & 31;
        G.srcrow = (info >> 12) & 7;
        G.light = (info >> 15) & 7;
        G.post = post & 63;
        G.postnorm = (post >> 8) & 1;
        if (G.type == 1) g_need_tangents = true;   // emboss
    }
    load_chan(0);
    load_chan(1);
    g_lights_loaded = 0;
    g_mtx_cached = 0xFFFFFFFFu;
}

static void transform_vertex(const InVertex& v, GpuVertex& o, bool has_nrm) {
    if (v.pnmtx != g_mtx_cached) load_matrices(v.pnmtx);
    const float* pm = g_posmtx;
    const float px = v.pos[0], py = v.pos[1], pz = v.pos[2];
    Vec3 pos = v3(pm[0] * px + pm[1] * py + pm[2] * pz + pm[3],
                  pm[4] * px + pm[5] * py + pm[6] * pz + pm[7],
                  pm[8] * px + pm[9] * py + pm[10] * pz + pm[11]);
    o.pos[0] = pos.x; o.pos[1] = pos.y; o.pos[2] = pos.z;
    const float* nm = g_nrmmtx;
    auto nmul = [&](const float* a) {
        return normalize(v3(nm[0] * a[0] + nm[1] * a[1] + nm[2] * a[2],
                            nm[3] * a[0] + nm[4] * a[1] + nm[5] * a[2],
                            nm[6] * a[0] + nm[7] * a[1] + nm[8] * a[2]));
    };
    Vec3 nrm = has_nrm ? nmul(v.nrm) : v3(0, 0, 1);
    // Only an emboss texgen reads these; see g_need_tangents.
    Vec3 bin = v3(0, 0, 0), tan = v3(0, 0, 0);
    if (has_nrm && g_need_tangents) { bin = nmul(v.bin); tan = nmul(v.tan); }

    const uint32_t numcol = g_numcol;
    for (int ch = 0; ch < 2; ch++) {
        uint32_t c = ch < (int)numcol ? compute_color(ch, v, pos, nrm) : 0xFFFFFFFF;
        o.col[ch][0] = c >> 24; o.col[ch][1] = c >> 16; o.col[ch][2] = c >> 8; o.col[ch][3] = (uint8_t)c;
    }

    const uint32_t ntex = g_ntex;
    const bool dualtex = g_dualtex;
    for (uint32_t t = 0; t < 8; t++) {
        if (t >= ntex) { o.tex[t][0] = o.tex[t][1] = 0; o.tex[t][2] = 1; continue; }
        const GenParams& G = g_gen[t];
        const uint32_t proj = G.proj, form = G.form, type = G.type, row = G.row;
        float src[4] = {0, 0, 1, 1};
        switch (row) {
        case 0: src[0] = v.pos[0]; src[1] = v.pos[1]; src[2] = v.pos[2]; break;
        case 1: src[0] = v.nrm[0]; src[1] = v.nrm[1]; src[2] = v.nrm[2]; break;
        case 2: break;  // colors (used by color texgens)
        case 3: src[0] = v.bin[0]; src[1] = v.bin[1]; src[2] = v.bin[2]; break;  // binormal T
        case 4: src[0] = v.tan[0]; src[1] = v.tan[1]; src[2] = v.tan[2]; break;  // binormal B
        default: if (row >= 5 && row <= 12) { src[0] = v.tex[row - 5][0]; src[1] = v.tex[row - 5][1]; } break;
        }
        if (form == 0) src[2] = 1.0f;  // AB11
        float s = 0, tt = 0, q = 1;
        if (type == 0) {  // regular
            uint32_t tm = (v.texmtx[t] & 63) * 4;
            s = xf_f(tm + 0) * src[0] + xf_f(tm + 1) * src[1] + xf_f(tm + 2) * src[2] + xf_f(tm + 3);
            tt = xf_f(tm + 4) * src[0] + xf_f(tm + 5) * src[1] + xf_f(tm + 6) * src[2] + xf_f(tm + 7);
            if (proj) q = xf_f(tm + 8) * src[0] + xf_f(tm + 9) * src[1] + xf_f(tm + 10) * src[2] + xf_f(tm + 11);
        } else if (type == 1) {  // emboss
            const uint32_t srcrow = G.srcrow, light = G.light;
            if (!(g_lights_loaded & (1u << light))) {
                load_light((int)light);
                g_lights_loaded |= 1u << light;
            }
            Vec3 ldir = normalize(sub(g_lights[light].pos, pos));
            s = o.tex[srcrow][0] + dot(ldir, tan);
            tt = o.tex[srcrow][1] + dot(ldir, bin);
            q = o.tex[srcrow][2];
        } else {  // color0/color1 -> s,t
            int ch = type == 2 ? 0 : 1;
            s = kU8toF[o.col[ch][0]];
            tt = kU8toF[o.col[ch][1]];
        }
        if (dualtex && type == 0) {
            const uint32_t ptm = 0x500 + G.post * 4;
            float in[3] = {s, tt, q};
            if (G.postnorm) {
                float l = sqrtf(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
                if (l > 0) { in[0] /= l; in[1] /= l; in[2] /= l; }
            }
            s = xf_f(ptm + 0) * in[0] + xf_f(ptm + 1) * in[1] + xf_f(ptm + 2) * in[2] + xf_f(ptm + 3);
            tt = xf_f(ptm + 4) * in[0] + xf_f(ptm + 5) * in[1] + xf_f(ptm + 6) * in[2] + xf_f(ptm + 7);
            q = xf_f(ptm + 8) * in[0] + xf_f(ptm + 9) * in[1] + xf_f(ptm + 10) * in[2] + xf_f(ptm + 11);
        }
        o.tex[t][0] = s; o.tex[t][1] = tt; o.tex[t][2] = q;
    }
}

// ---------------------------------------------------------------------------
// Pixel state snapshot
// ---------------------------------------------------------------------------
static void resolve_texture(PixelState& st, int map) {
    const uint32_t* bp = g_state.bp;
    uint32_t base = map < 4 ? 0x80 + map : 0xA0 + (map - 4);
    uint32_t mode0 = bp[base], mode1 = bp[base + 4], img0 = bp[base + 8], img3 = bp[base + 0x14], tlut = bp[base + 0x18];
    TexParams p{};
    p.addr = (img3 & 0x1FFFFF) << 5;
    p.width = (img0 & 0x3FF) + 1;
    p.height = ((img0 >> 10) & 0x3FF) + 1;
    p.fmt = (img0 >> 20) & 0xF;
    bool mips = (mode0 >> 5) & 3;
    uint32_t levels = 1;
    if (mips) {
        uint32_t maxlod = ((mode1 >> 8) & 0xFF) / 16;
        uint32_t w = p.width, h = p.height;
        while (levels <= maxlod && (w > 1 || h > 1)) { w = w > 1 ? w / 2 : 1; h = h > 1 ? h / 2 : 1; levels++; }
    }
    p.levels = levels;
    p.tlut_off = (tlut & 0x3FF) << 9;
    p.tlut_fmt = (tlut >> 10) & 3;
    const auto t0 = g_frametime ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    TexLookup r = texture_lookup(p, batch().new_textures);
    if (g_frametime) g_tex_ms += ms_since(t0);
    st.tex_id[map] = r.id;
    st.tex_is_efb[map] = r.efb;
}

static PixelState g_last_state;
static bool g_have_last_state;
static uint32_t g_last_state_idx;

// Whether position matrix `pnmtx` leaves the view-space Y axis alone -- it may turn the
// object about that axis, but not tilt it -- which is how a thing the game has placed in
// front of the camera is told from world geometry.
//
// Translation is not tested: the countdown rig is placed at (0, 30, -160), and something
// held a fixed distance in front of the camera is as much a part of the HUD as something
// at the origin.
//
// The rotation used to have to be the identity, which is true of the rig only once it has
// finished arriving. It spins in about the vertical axis over ten frames -- cos running
// 0.833 to 1 while it descends -- and for those ten frames it was read as world geometry,
// drawn with the world's pitch taken out of it, and then snapped onto the HUD frame the
// moment the spin stopped. Allowing the turn is what makes it arrive the way it does on a
// television.
//
// Nothing in the world passes this. A frame's worth of matrices at the start of a race --
// 6,464 of them over sixteen frames -- holds three that leave the Y axis alone: the two
// 2D layers at the origin, and the rig. World geometry cannot, because its matrix carries
// the camera, and this game's chase camera is pitched some 23 degrees down at all times.
//
// The matrix is taken from the draw's first vertex. A draw whose vertices indexed
// different position matrices would be judged by that one; none in this game does.
static bool pos_matrix_is_view_space(uint8_t pnmtx) {
    // MP_VS_YAW=0 goes back to demanding the identity rotation, which is what this did
    // before it was taught to allow a turn. For telling apart "the rig is misplaced" from
    // "something else was reclassified by the wider test".
    static const bool allow_yaw = !(getenv("MP_VS_YAW") && atoi(getenv("MP_VS_YAW")) == 0);
    const uint32_t m = (pnmtx & 63) * 4;
    if (!allow_yaw) {
        for (int i = 0; i < 9; i++) {
            const float want = (i % 4 == 0) ? 1.0f : 0.0f;
            if (fabsf(xf_f(m + (i / 3) * 4 + (i % 3)) - want) > 1e-4f) return false;
        }
        return true;
    }
    const float m00 = xf_f(m + 0), m01 = xf_f(m + 1), m02 = xf_f(m + 2);
    const float m10 = xf_f(m + 4), m11 = xf_f(m + 5), m12 = xf_f(m + 6);
    const float m21 = xf_f(m + 9);
    // The Y axis is untouched: no tilt in, and none out.
    if (fabsf(m01) > 1e-4f || fabsf(m10) > 1e-4f || fabsf(m12) > 1e-4f ||
        fabsf(m21) > 1e-4f || fabsf(m11 - 1.0f) > 1e-4f)
        return false;
    // And what is left is a rotation rather than a scale or a skew that happens to keep
    // the Y axis, which no draw in this game produces but which would be read as a HUD
    // element if it ever did.
    return fabsf(m00 * m00 + m02 * m02 - 1.0f) < 1e-3f;
}

// `view_space` is pos_matrix_is_view_space() for the draw: the game put these vertices in
// view space itself instead of placing them in the world. See PixelState.
static uint32_t snapshot_state(bool view_space) {
    // Nothing the state is built from has changed since the last draw, so the answer is
    // the last answer. This is the common case by a wide margin -- see State::pixel_dirty.
    if (g_have_last_state && !g_state.pixel_dirty && g_last_state.view_space == view_space)
        return g_last_state_idx;
    g_state.pixel_dirty = false;
    PixelState st;
    memset(&st, 0, sizeof(st));
    st.view_space = view_space;
    memcpy(st.bp, g_state.bp, sizeof(st.bp));
    memcpy(st.tev_reg, g_tev_reg, sizeof(g_tev_reg));
    memcpy(st.tev_konst, g_tev_konst, sizeof(g_tev_konst));
    for (int i = 0; i < 6; i++) st.proj[i] = xfr_f(0x20 + i);
    st.proj[6] = (float)g_state.xf_regs[0x26];
    for (int i = 0; i < 6; i++) st.viewport[i] = xfr_f(0x1A + i);
    st.num_texgens = g_state.xf_regs[0x3F] & 15;
    st.num_colors = g_state.xf_regs[0x09] & 3;
    // Textures referenced by TEV stages and indirect stages.
    uint32_t genmode = g_state.bp[0x00];
    uint32_t nstages = ((genmode >> 10) & 15) + 1;
    uint32_t used = 0;
    for (uint32_t s = 0; s < nstages; s++) {
        uint32_t order = g_state.bp[0x28 + s / 2] >> ((s & 1) * 12);
        if (order & 0x40) used |= 1u << (order & 7);
    }
    uint32_t nind = (genmode >> 16) & 7;
    for (uint32_t i = 0; i < nind; i++) used |= 1u << ((g_state.bp[0x27] >> (6 * i)) & 7);
    for (int m = 0; m < 8; m++)
        if (used & (1u << m)) resolve_texture(st, m);
    if (g_have_last_state && memcmp(&st, &g_last_state, sizeof(st)) == 0) return g_last_state_idx;
    g_last_state = st;
    g_have_last_state = true;
    g_last_state_idx = (uint32_t)batch().states.size();
    batch().states.push_back(st);
    return g_last_state_idx;
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------
static std::vector<InVertex> g_in;

static void draw_impl(const DrawCall& dc);

void renderer_draw(const DrawCall& dc) {
    if (!g_frametime) { draw_impl(dc); return; }
    const auto t0 = std::chrono::steady_clock::now();
    draw_impl(dc);
    g_fe_ms += ms_since(t0);
    g_fe_draws++;
}

static void draw_impl(const DrawCall& dc) {
    if (dc.count == 0) return;
    const Layout& L = layout_for(dc.vat);
    // Grown, never shrunk: resize() value-initialises whatever it adds, and this buffer
    // is refilled from scratch by decode_vertex every draw, so letting it follow a draw
    // count that swings between one and a few hundred spent its time zeroing bytes that
    // were about to be overwritten.
    if (g_in.size() < dc.count) g_in.resize(dc.count);
    load_xf_plan();
    const uint8_t* p = dc.data;
    for (uint32_t i = 0; i < dc.count; i++) decode_vertex(L, p, g_in[i]);
    // Transformed straight into the batch, each vertex once; the primitive's shape is
    // expressed by the indices below.
    Batch& b = batch();
    const uint32_t base = (uint32_t)b.verts.size();
    b.verts.resize(base + dc.count);
    GpuVertex* out = b.verts.data() + base;
    bool has_nrm = L.nrm_desc != 0;
    for (uint32_t i = 0; i < dc.count; i++) transform_vertex(g_in[i], out[i], has_nrm);

    // MP_MTXLOG=<n> prints the position matrix and projection of every draw in the nth
    // presented frame. The timebase is wall-clock driven, so frame N is a slightly
    // different moment in every run; pick a frame well inside a steady scene.
    uint32_t state = snapshot_state(pos_matrix_is_view_space(g_in[0].pnmtx));
    if (g_mtxlog) {
        // MP_WATCH=a,b,c prints those guest addresses every frame. Sparse snapshots cannot
        // tell a steady flag from one that blinks, so this samples every frame.
        static const char* watch = getenv("MP_WATCH");
        static uint32_t watch_last_frame = ~0u;
        if (watch && g_frame_counter != watch_last_frame) {
            watch_last_frame = g_frame_counter;
            fprintf(stderr, "[watch] f%u", g_frame_counter);
            for (const char* s = watch; s && *s;) {
                const uint32_t a = (uint32_t)strtoul(s, nullptr, 0);
                fprintf(stderr, " %#x=%08x", a, mem_r32(a));
                const char* c = strchr(s, ',');
                s = c ? c + 1 : nullptr;
            }
            fprintf(stderr, "\n");
        }
        if (g_frame_counter == g_mtxlog) g_mtx_draw = 0;
    }
    if (mtxlog_frame()) {
        const uint32_t m = (g_in[0].pnmtx & 63) * 4;
        const PixelState& ps = b.states[state];
        fprintf(stderr, "[mtx] %3u n=%4u pnmtx=%2u st=%u | %8.3f %8.3f %8.3f %10.2f | %8.3f %8.3f %8.3f %10.2f"
                " | %8.3f %8.3f %8.3f %10.2f | out0=%.1f,%.1f,%.1f |",
                g_mtx_draw, dc.count, g_in[0].pnmtx & 63, state,
                xf_f(m + 0), xf_f(m + 1), xf_f(m + 2), xf_f(m + 3),
                xf_f(m + 4), xf_f(m + 5), xf_f(m + 6), xf_f(m + 7),
                xf_f(m + 8), xf_f(m + 9), xf_f(m + 10), xf_f(m + 11),
                out[0].pos[0], out[0].pos[1], out[0].pos[2]);
        // The textures too: a model is recognised by what it is textured with, so this
        // is what ties a draw in one pass to the same model drawn in another.
        for (int i = 0; i < 8; i++)
            if (ps.tex_id[i]) fprintf(stderr, " t%d=%u%s", i, ps.tex_id[i], ps.tex_is_efb[i] ? "*" : "");
        fprintf(stderr, "\n");
        fprintf(stderr, "[prj] %3u type=%d %9.4f %9.4f %9.4f %9.4f %9.4f %9.4f\n", g_mtx_draw,
                (int)g_state.xf_regs[0x26], xfr_f(0x20), xfr_f(0x21), xfr_f(0x22),
                xfr_f(0x23), xfr_f(0x24), xfr_f(0x25));
        g_mtx_draw++;
    }

    // MP_PNMLOG=a-b lists the position matrices a frame actually uses, one line each time
    // the matrix changes, over a window of frames. What it is for: an object the game
    // places in front of the camera is told apart from world geometry by its rotation
    // being identity, so anything camera-attached that *animates* is misread as world
    // geometry for as long as it moves. This is how to see that happen.
    static int pnm_lo = -1, pnm_hi = -1;
    static bool pnm_parsed = false;
    if (!pnm_parsed) {
        pnm_parsed = true;
        if (const char* r = getenv("MP_PNMLOG")) {
            pnm_lo = atoi(r);
            const char* dash = strchr(r, '-');
            pnm_hi = dash ? atoi(dash + 1) : pnm_lo;
        }
    }
    if (pnm_lo >= 0 && (int)g_frame_counter >= pnm_lo && (int)g_frame_counter <= pnm_hi &&
        dc.count) {
        const uint32_t m = (g_in[0].pnmtx & 63) * 4;
        static float last[12];
        static uint32_t last_frame = ~0u;
        float cur[12];
        for (int i = 0; i < 12; i++) cur[i] = xf_f(m + i);
        if (g_frame_counter != last_frame || memcmp(cur, last, sizeof(cur)) != 0) {
            memcpy(last, cur, sizeof(cur));
            last_frame = g_frame_counter;
            fprintf(stderr, "[pnm] f%u n=%4u id=%2u ident=%d | %7.3f %7.3f %7.3f %9.2f |"
                            " %7.3f %7.3f %7.3f %9.2f | %7.3f %7.3f %7.3f %9.2f\n",
                    g_frame_counter, dc.count, g_in[0].pnmtx & 63,
                    (int)pos_matrix_is_view_space(g_in[0].pnmtx), cur[0], cur[1], cur[2], cur[3],
                    cur[4], cur[5], cur[6], cur[7], cur[8], cur[9], cur[10], cur[11]);
        }
    }
    // The draw's position matrix, from its first vertex -- see pos_matrix_is_view_space
    // for why that one stands for the draw. Appended only when it differs from the last
    // one appended, so the draws of one rigid object share an index.
    uint32_t mtx = (uint32_t)b.mtxs.size() / 12;
    {
        const uint32_t m = (g_in[0].pnmtx & 63) * 4;
        float cur[12];
        for (int i = 0; i < 12; i++) cur[i] = xf_f(m + i);
        if (mtx > 0 && memcmp(cur, &b.mtxs[(mtx - 1) * 12], sizeof(cur)) == 0) {
            mtx--;
        } else {
            b.mtxs.insert(b.mtxs.end(), cur, cur + 12);
        }
    }
    uint32_t first = (uint32_t)b.indices.size();
    uint8_t prim = 0;
    auto push = [&](uint32_t i) { b.indices.push_back(base + i); };
    switch (dc.prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (uint32_t i = 0; i + 3 < dc.count; i += 4) { push(i); push(i + 1); push(i + 2); push(i); push(i + 2); push(i + 3); }
        break;
    case PRIM_TRIANGLES:
        for (uint32_t i = 0; i + 2 < dc.count; i += 3) { push(i); push(i + 1); push(i + 2); }
        break;
    case PRIM_TRISTRIP:
        for (uint32_t i = 0; i + 2 < dc.count; i++) {
            if (i & 1) { push(i + 1); push(i); push(i + 2); }
            else { push(i); push(i + 1); push(i + 2); }
        }
        break;
    case PRIM_TRIFAN:
        for (uint32_t i = 1; i + 1 < dc.count; i++) { push(0); push(i); push(i + 1); }
        break;
    case PRIM_LINES:
        prim = 1;
        for (uint32_t i = 0; i + 1 < dc.count; i += 2) { push(i); push(i + 1); }
        break;
    case PRIM_LINESTRIP:
        prim = 1;
        for (uint32_t i = 0; i + 1 < dc.count; i++) { push(i); push(i + 1); }
        break;
    case PRIM_POINTS:
        prim = 2;
        for (uint32_t i = 0; i < dc.count; i++) push(i);
        break;
    }
    uint32_t count = (uint32_t)b.indices.size() - first;
    if (!count) return;
    // Merge with the previous draw when state and primitive type match.
    if (!b.cmds.empty()) {
        Cmd& last = b.cmds.back();
        if (last.type == CmdType::Draw && last.state == state && last.prim == prim &&
            last.mtx == mtx && last.first + last.count == first) {
            last.count += count;
            return;
        }
    }
    Cmd c{};
    c.type = CmdType::Draw;
    c.prim = prim;
    c.state = state;
    c.first = first;
    c.count = count;
    c.mtx = mtx;
    b.cmds.push_back(c);
}

// ---------------------------------------------------------------------------
// BP side effects
// ---------------------------------------------------------------------------
void renderer_bp_write(uint32_t reg, uint32_t v) {
    if (reg >= 0xE0 && reg <= 0xE7) {
        uint32_t r = (reg - 0xE0) / 2, half = (reg - 0xE0) & 1;
        if ((v >> 23) & 1) g_tev_konst[r][half] = v;
        else g_tev_reg[r][half] = v;
        return;
    }
    if (reg == 0x65) {  // load TLUT
        uint32_t src = (g_state.bp[0x64] & 0x1FFFFF) << 5;
        uint32_t tmem = (v & 0x3FF) << 9;
        uint32_t count = ((v >> 10) & 0x7FF) * 16 * 2;
        tlut_load(src, tmem, count);
        return;
    }
}

void renderer_efb_copy(uint32_t dest_addr, bool /*unused*/) {
    const uint32_t* bp = g_state.bp;
    uint32_t v = bp[0x52];
    EfbCopyCmd cc{};
    cc.src_x = bp[0x49] & 0x3FF;
    cc.src_y = (bp[0x49] >> 10) & 0x3FF;
    cc.src_w = (bp[0x4A] & 0x3FF) + 1;
    cc.src_h = ((bp[0x4A] >> 10) & 0x3FF) + 1;
    cc.to_xfb = (v >> 14) & 1;
    bool half = (v >> 9) & 1;
    cc.dst_w = half ? cc.src_w / 2 : cc.src_w;
    cc.dst_h = half ? cc.src_h / 2 : cc.src_h;
    uint32_t tf = (v >> 3) & 0xF;
    cc.format = tf / 2 + (tf & 1) * 8;
    if ((v >> 15) & 1) cc.format |= 0x10;
    cc.depth = (bp[0x43] & 7) == 3;
    cc.clear = (v >> 11) & 1;
    cc.clear_color = (bp[0x41] >> 3) & 1;
    cc.clear_alpha = (bp[0x41] >> 4) & 1;
    cc.clear_z = (bp[0x40] >> 4) & 1;
    cc.clear_rgba = ((bp[0x4F] & 0xFF) << 24) | ((bp[0x50] >> 8 & 0xFF) << 16) | ((bp[0x50] & 0xFF) << 8) | ((bp[0x4F] >> 8) & 0xFF);
    cc.clear_z_value = bp[0x51] & 0xFFFFFF;
    // Logged after the fields are filled in; it used to print cc.clear before it was
    // assigned, so every copy appeared not to clear.
    static const bool copylog = getenv("MP_COPYLOG") != nullptr;
    if (copylog)
        fprintf(stderr, "[copy] f%u xfb=%d dest=%08X dst=%ux%u src=%ux%u@%u,%u fmt=%X clear=%d "
                "half=%d pe=%06X\n", g_frame_counter, cc.to_xfb, dest_addr, cc.dst_w, cc.dst_h,
                cc.src_w, cc.src_h, cc.src_x, cc.src_y, cc.format, cc.clear, (int)half, v);
    if (!cc.to_xfb) cc.tex_id = texture_register_efb_copy(dest_addr, cc.dst_w, cc.dst_h, cc.format);
    // In the MP_MTXLOG dump, so the draws it lists can be read against the pass
    // boundaries: which were drawn into an off-screen target and which into the scene.
    if (mtxlog_frame())
        fprintf(stderr, "[cpy] after draw %u: xfb=%d tex=%u dst=%ux%u src=%ux%u@%u,%u clear=%d\n",
                g_mtx_draw, cc.to_xfb, cc.tex_id, cc.dst_w, cc.dst_h, cc.src_w, cc.src_h,
                cc.src_x, cc.src_y, cc.clear);
    // For display copies the presentation must see the EFB before the copy's clear.
    if (cc.to_xfb) {
        Cmd pc{};
        pc.type = CmdType::Present;
        pc.copy = cc;
        batch().cmds.push_back(pc);
    }
    Cmd c{};
    c.type = CmdType::EfbCopy;
    c.copy = cc;
    batch().cmds.push_back(c);
    static int trace_frame = getenv("MP_TRACE_FRAME") ? atoi(getenv("MP_TRACE_FRAME")) : -1;
    if ((int)g_frame_counter == trace_frame) {
        Batch& bb = batch();
        fprintf(stderr, "[trace] frame %u copy xfb=%d fmt=%X src %ux%u@%u,%u dst %ux%u\n", g_frame_counter, cc.to_xfb, cc.format, cc.src_w, cc.src_h,
                cc.src_x, cc.src_y, cc.dst_w, cc.dst_h);
        for (size_t i = 0; i < bb.cmds.size(); i++) {
            const Cmd& cmd = bb.cmds[i];
            if (cmd.type != CmdType::Draw) { fprintf(stderr, "  [%zu] copy/present\n", i); continue; }
            const PixelState& ps = bb.states[cmd.state];
            uint32_t ns = ((ps.bp[0] >> 10) & 15) + 1;
            fprintf(stderr, "  [%zu] draw n=%u stages=%u tex0=%u%s blend=%06X z=%06X pixfmt=%X order0=%06X c0=%06X a0=%06X c1=%06X a1=%06X kc0=%08X/%08X\n", i, cmd.count, ns,
                    ps.tex_id[0], ps.tex_is_efb[0] ? "(efb)" : "", ps.bp[0x41], ps.bp[0x40], ps.bp[0x43], ps.bp[0x28], ps.bp[0xC0], ps.bp[0xC1], ps.bp[0xC2], ps.bp[0xC3],
                    ps.tev_konst[0][0], ps.tev_konst[0][1]);
            for (uint32_t st = 0; st < ns; st++)
                fprintf(stderr, "      st%u order=%03X cenv=%06X aenv=%06X ksel=%06X\n", st, (ps.bp[0x28 + st / 2] >> ((st & 1) * 12)) & 0xFFF,
                        ps.bp[0xC0 + 2 * st], ps.bp[0xC1 + 2 * st], ps.bp[0xF6 + st / 2]);
            for (int m = 0; m < 8; m++)
                if (ps.tex_id[m]) fprintf(stderr, "      map%d id=%u img0=%06X img3=%06X\n", m, ps.tex_id[m], ps.bp[(m < 4 ? 0x88 + m : 0xA8 + m - 4)], ps.bp[(m < 4 ? 0x94 + m : 0xB4 + m - 4)]);
            for (int m = 0; m < 3; m++) {
                uint32_t a = (ps.bp[0x94 + m] & 0x1FFFFF) << 5;
                const uint8_t* q = phys_ptr(a);
                uint64_t sum = 0; uint32_t mn = 255, mx = 0;
                for (int i = 0; i < 64000; i++) { sum += q[i]; mn = std::min<uint32_t>(mn, q[i]); mx = std::max<uint32_t>(mx, q[i]); }
                fprintf(stderr, "      plane%d @%08X avg=%llu min=%u max=%u\n", m, a, (unsigned long long)(sum / 64000), mn, mx);
            }
            fprintf(stderr, "      num_texgens=%u (bp genmode %u) xf texgen0=%08X texgen1=%08X\n", ps.num_texgens, ps.bp[0] & 15,
                    g_state.xf_regs[0x40], g_state.xf_regs[0x41]);
            for (uint32_t i = cmd.first; i < cmd.first + 2 && i < bb.indices.size(); i++) {
                const uint32_t v = bb.indices[i];
                fprintf(stderr, "      v%u pos=(%g,%g,%g) col0=%02X%02X%02X%02X col1=%02X%02X%02X%02X\n", v,
                        bb.verts[v].pos[0], bb.verts[v].pos[1], bb.verts[v].pos[2],
                        bb.verts[v].col[0][0], bb.verts[v].col[0][1], bb.verts[v].col[0][2], bb.verts[v].col[0][3],
                        bb.verts[v].col[1][0], bb.verts[v].col[1][1], bb.verts[v].col[1][2], bb.verts[v].col[1][3]);
                for (int tc = 0; tc < 8; tc++)
                    fprintf(stderr, "        tex%d=(%g,%g,%g)\n", tc, bb.verts[v].tex[tc][0],
                            bb.verts[v].tex[tc][1], bb.verts[v].tex[tc][2]);
            }
            for (int r = 0; r < 4; r++)
                fprintf(stderr, "      reg%d %06X %06X konst%d %06X %06X\n", r, ps.tev_reg[r][0], ps.tev_reg[r][1], r, ps.tev_konst[r][0], ps.tev_konst[r][1]);
        }
    }
    if (cc.to_xfb && getenv("MP_GXSTATS")) {
        Batch& bb = batch();
        uint32_t draws = 0;
        for (auto& cmd : bb.cmds) draws += cmd.type == CmdType::Draw;
        fprintf(stderr, "[gx] frame %u: %u draws, %zu verts, %zu states, %zu new tex, copy %ux%u@%u,%u\n", g_frame_counter, draws,
                bb.verts.size(), bb.states.size(), bb.new_textures.size(), cc.src_w, cc.src_h, cc.src_x, cc.src_y);
        if (!bb.states.empty()) {
            const PixelState& ps = bb.states.back();
            fprintf(stderr, "     genmode %06X order %06X cenv %06X aenv %06X tex0 id %u img0 %06X img3 %06X mode0 %06X blend %06X z %06X\n",
                    ps.bp[0], ps.bp[0x28], ps.bp[0xC0], ps.bp[0xC1], ps.tex_id[0], ps.bp[0x88], ps.bp[0x94], ps.bp[0x80], ps.bp[0x41], ps.bp[0x40]);
            fprintf(stderr, "     proj %g %g %g %g %g %g type %g  vp %g %g %g %g %g %g\n", ps.proj[0], ps.proj[1], ps.proj[2], ps.proj[3],
                    ps.proj[4], ps.proj[5], ps.proj[6], ps.viewport[0], ps.viewport[1], ps.viewport[2], ps.viewport[3], ps.viewport[4], ps.viewport[5]);
        }
        for (size_t i = 0; i < bb.verts.size() && i < 3; i++) {
            const GpuVertex& gv = bb.verts[i];
            fprintf(stderr, "     v%zu pos %g %g %g col %02X%02X%02X%02X tex0 %g %g %g\n", i, gv.pos[0], gv.pos[1], gv.pos[2], gv.col[0][0], gv.col[0][1],
                    gv.col[0][2], gv.col[0][3], gv.tex[0][0], gv.tex[0][1], gv.tex[0][2]);
        }
    }
    if (cc.to_xfb) {
        if (g_frametime) {
            const Batch& bb = batch();
            size_t decoded = 0;
            for (auto& t : bb.new_textures)
                for (auto& l : t->levels) decoded += l.size() * 4;
            const auto now = std::chrono::steady_clock::now();
            const double interval = g_last_present.time_since_epoch().count() ? ms_since(g_last_present) : 0.0;
            fprintf(stderr, "[ft] f%u guest %6.2fms  fe %6.2fms (tex %5.2f)  draws %4u/%4zu  verts %6zu  idx %6zu  states %4zu  newtex %3zu (%zuKB)  queue %zu\n",
                    g_frame_counter, interval, g_fe_ms, g_tex_ms, g_fe_draws, bb.cmds.size(), bb.verts.size(), bb.indices.size(),
                    bb.states.size(), bb.new_textures.size(), decoded / 1024, queue_depth());
            g_last_present = now;
            g_fe_ms = g_tex_ms = 0;
            g_fe_draws = 0;
        }
        g_frame_counter++;
        texture_evict();
        g_frames_submitted++;
        g_have_last_state = false;
        flush_batch();
    }
}

}  // namespace gx

uint32_t gx_frames_submitted() { return gx::g_frames_submitted.load(); }
