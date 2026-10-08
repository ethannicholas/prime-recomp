// Data passed from the GX front end (guest thread) to the render back end (main thread).
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace gx {

// Vertex after the XF stage (except projection): view-space position, lit colors,
// generated texture coordinates.
struct GpuVertex {
    // Does nothing on purpose. The batch's vertex array is grown with resize(), which
    // value-initialises what it adds -- and for a trivial type that means zeroing all
    // 116 of these bytes immediately before the transform writes every one of them. A
    // user-provided constructor makes that default-initialisation instead, which leaves
    // them alone.
    GpuVertex() {}
    float pos[3];
    uint8_t col[2][4];
    float tex[8][3];
};

// Decoded texture (RGBA8, all mip levels), immutable once created.
struct TexData {
    uint32_t id;
    uint32_t width, height;
    std::vector<std::vector<uint32_t>> levels;  // RGBA8, row 0 = top
};

// Everything the pixel pipeline needs for a draw.
struct PixelState {
    uint32_t bp[256];
    uint32_t tev_reg[4][2];    // [reg][0=RA,1=BG] 11-bit signed components
    uint32_t tev_konst[4][2];
    float proj[7];             // XF 0x1020..0x1026 (last is type as float)
    float viewport[6];         // XF 0x101A..0x101F
    uint32_t tex_id[8];        // texture per texmap (0 = none)
    uint8_t tex_is_efb[8];     // texmap is an EFB copy (sampled from render target copy)
    uint8_t num_texgens;
    uint8_t num_colors;
    // The draw's position matrix was the identity, so the game placed this geometry in
    // view space itself rather than in the world. Here that means the countdown light
    // rig, which is 3D but belongs to the HUD: it hangs a fixed distance in front of the
    // camera and never moves with the course. Stereo has to tell the two apart --
    // re-projected into an eye as world geometry, the rig becomes a solid object
    // standing in the water between the viewer and the racer.
    uint8_t view_space;
};

enum class CmdType : uint8_t { Draw, EfbCopy, Present };

struct EfbCopyCmd {
    uint32_t src_x, src_y, src_w, src_h;   // EFB rect
    uint32_t dst_w, dst_h;
    uint32_t tex_id;                       // destination texture (0 when to XFB)
    uint32_t format;                       // GX copy texture format (+0x10 if intensity)
    bool to_xfb;
    bool depth;                            // copying the Z buffer
    bool clear;
    bool clear_color, clear_alpha, clear_z;
    uint32_t clear_rgba;                   // 0xRRGGBBAA
    uint32_t clear_z_value;                // 24-bit
};

struct Cmd {
    CmdType type;
    uint8_t prim;       // GL-ish: 0 = triangles, 1 = lines, 2 = points
    uint32_t state;     // index into Batch::states
    uint32_t first, count;  // range of Batch::indices
    // The draw's position matrix: an index into Batch::mtxs. The vertices arrive already
    // transformed by it, so nothing here needs it to draw; it is kept for what the stereo
    // path reads off it -- which draws are the player's racer, and where the ski is. The
    // index only changes when the matrix does, so a run of draws sharing one shares it.
    uint32_t mtx;
    EfbCopyCmd copy;
};

struct Batch {
    std::vector<Cmd> cmds;
    // Each GX vertex once, in the order the draws delivered them; the quads, strips and
    // fans the game draws with are turned into lists by the indices instead of by
    // repeating vertices, which would nearly double the buffer -- and this buffer is
    // uploaded and fetched for every pass of every frame.
    std::vector<GpuVertex> verts;
    std::vector<uint32_t> indices;
    std::vector<PixelState> states;
    // The position matrices the draws used, twelve floats each in GX's row-major 3x4
    // layout (row r is m[r*4..r*4+3], the last column the translation). See Cmd::mtx.
    std::vector<float> mtxs;
    std::vector<std::shared_ptr<TexData>> new_textures;

    // A race frame's batch is eight megabytes or so, and a fresh one grew into that
    // from nothing every frame -- reallocating and copying the vertex array a dozen
    // times over on the guest thread, then freeing it all on the render thread. So a
    // batch that is destroyed hands its storage, capacity intact, to a pool the front
    // end builds the next one from. The hand-off is in the destructor so that no
    // frontend has to know; `pooled` marks the copies resting in the pool, which must
    // not hand themselves over again.
    Batch() = default;
    Batch(Batch&&) = default;
    Batch& operator=(Batch&&) = default;
    ~Batch();
    bool pooled = false;
};

// Submission queue (guest thread -> render thread)
void submit_batch(std::unique_ptr<Batch> b);
std::unique_ptr<Batch> take_batch(int timeout_ms);

// Frame pacing / stats
extern std::atomic<uint32_t> g_frames_submitted;

}  // namespace gx
