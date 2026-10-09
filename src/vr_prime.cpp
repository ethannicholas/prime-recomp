// Metroid Prime's answers to the headset frontend's questions (gcn-recomp/runtime/vr_game.h).
//
// Prime is first person: the game's camera is Samus's head, so the frontend's default --
// the game's camera as the origin of the room, head tracking looking around from it -- is
// already the right eye, and no first-person hook is needed. What the game does have to say
// is when that camera is in use. Stereo is for first-person play; the morph ball's camera,
// cinematics and the menus (where there is no camera manager at all) go on the theater
// panel. See docs/dev/vr.md.
//
// Which camera is current, from the game's own state. The addresses and layouts are this
// disc's (GM8E01 revision 2), from analysis/symbols.txt and the Metroid Prime decompilation;
// nothing here is the game's code.
//
//   CCameraManager   has no vtable. Its first field is the current camera's TUniqueId
//                    (16 bits), and the word at +0x8 is the number of cinematic cameras in
//                    use (the size of mCineCameras). The id does not move off the
//                    first-person camera for a cinematic: GetCurrentCamera prefers the
//                    newest cinematic camera whenever that count is non-zero, and
//                    IsInCinematicCamera is a test of that one word. In revision 2 it holds
//                    pointers to the first-person and ball cameras side by side at +0x88
//                    (mFpCamera, mBallCamera). It is built in a static arena, `sAllocSpace`
//                    (0x8045D614 on the frigate, found that way), not on the heap.
//   CEntity          vtable at +0, area id at +4, its own TUniqueId at +8; every camera
//                    is one.
//
// The camera alone is not enough: the pause and map screens, and the world's name shown on
// black between worlds, all keep the first-person camera current with no cinematic. What
// marks them is that the world is not drawn. CMFGame::Draw draws the world only when
// CInGameGuiManager::GetIsGameDraw says to (the pause screen's blur turns that off once it is
// up), and between worlds CMFGameLoader::Draw draws just the transition. Patches at both
// (recomp/patches.txt) report the answer through prime_world_drawn; it is a state, not an
// event, so the render thread reading it a frame early or late sees at worst a transition
// one frame early.
//
// There is no global pointer to it, so it is found: the word pair that points at a
// first-person camera and a ball camera, each recognised by its vtable. A pair found where a
// previous world's manager stood would point at freed cameras whose memory still holds their
// vtables, so the ball camera, which is a heap block of its own, must also still be
// allocated (the block header layout is in heap_check.cpp). The first-person camera sits
// inside a larger block and cannot be checked that way. Once found the pair is re-checked
// every frame, and looked for again when it stops checking out -- at most every two
// seconds, because a search reads all of RAM on the render thread, and over the menus,
// where there is nothing to find, it would otherwise run constantly.
#include "runtime.h"
#include "vr_game.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint32_t kFirstPersonCameraVtable = 0x803DA720;  // __vt__18CFirstPersonCamera
constexpr uint32_t kBallCameraVtable = 0x803DBC50;         // __vt__11CBallCamera
constexpr uint32_t kCameraPairOffset = 0x88;               // CCameraManager::mFpCamera, rev 2
constexpr uint32_t kCinematicCount = 0x8;                  // CCameraManager::mCineCameras.size()
constexpr uint32_t kSentinel = 0xEFEFEFEF, kCanary = 0xEAEAEAEA, kHeader = 0x20;

inline bool in_ram(uint32_t a) { return a >= 0x80000000u && a - 0x80000000u < RAM_SIZE - 0x400; }

// Whether `p` is the payload of an allocated heap block.
bool live_block(uint32_t p) {
    if (!in_ram(p - kHeader) || (p & 0x1F)) return false;
    const uint32_t h = p - kHeader;
    return mem_r32(h) == kSentinel && mem_r32(h + 0x1C) == kCanary &&
           (mem_r32(h + 0x10) & 1);  // bit 0 of the back link: allocated
}

bool is_camera(uint32_t obj, uint32_t vtable) {
    return in_ram(obj) && !(obj & 3) && mem_r32(obj) == vtable;
}

// `pair` is a manager's mFpCamera, with mBallCamera after it, and both are live.
bool live_pair(uint32_t pair) {
    const uint32_t ball = mem_r32(pair + 4);
    return is_camera(mem_r32(pair), kFirstPersonCameraVtable) && is_camera(ball, kBallCameraVtable) &&
           live_block(ball);
}

uint32_t search() {
    for (uint32_t a = 0x80000000u + kCameraPairOffset; a < 0x80000000u + RAM_SIZE - 8; a += 4)
        if (live_pair(a)) return a;
    return 0;
}

// The manager's camera pair, for the guest thread's gun hook below: found here, on the
// render thread, and re-checked there before use.
std::atomic<uint32_t> g_camera_pair{0};

// The game's latest answer to whether it draws its world (the patches above), written on the
// guest thread, read on the render thread.
std::atomic<uint32_t> g_world_drawn{0};

bool wants_stereo(const gx::Batch&) {
    static const bool log = getenv("GCN_STEREOLOG") != nullptr;
    static uint32_t pair = 0, frames_since_search = 1000;
    struct Publish {
        uint32_t& p;
        ~Publish() { g_camera_pair.store(p, std::memory_order_relaxed); }
    } publish{pair};
    if (!pair || !live_pair(pair)) {
        if (pair && log) fprintf(stderr, "[prime] camera manager gone\n");
        pair = 0;
        if (++frames_since_search < 120) return false;
        frames_since_search = 0;
        pair = search();
        if (!pair) return false;
        if (log) fprintf(stderr, "[prime] camera manager at %08X\n", pair - kCameraPairOffset);
    }
    const uint32_t mgr = pair - kCameraPairOffset;
    const uint16_t current = mem_r16(mgr);
    const uint32_t cinematic = mem_r32(mgr + kCinematicCount);
    const uint16_t first_person = mem_r16(mem_r32(pair) + 8);
    const bool world = g_world_drawn.load(std::memory_order_relaxed) != 0;
    if (log) {
        static uint32_t last = ~0u, last_cine = ~0u;
        static bool last_world = false;
        if (current != last || cinematic != last_cine || world != last_world) {
            last = current;
            last_cine = cinematic;
            last_world = world;
            fprintf(stderr, "[prime] current camera %04X, %u cinematic (first person %04X, ball %04X), world %s\n",
                    current, cinematic, first_person, mem_r16(mem_r32(pair + 4) + 8), world ? "drawn" : "not drawn");
        }
    }
    return current == first_person && cinematic == 0 && world;
}

// ---------------------------------------------------------------------------
// The arm cannon in the right hand.
//
// CPlayerGun (revision 2's layout, confirmed against the generated code; `this` is r28
// throughout CPlayerGun::Update):
//
//   +0x3E8  mXf           the gun's base transform, which CPlayer sets every frame from the
//                         first-person camera and the aiming cursor
//   +0x478  mAssistAimXf  the direction shots leave in: UpdateNormalShotCycle and
//                         FireSecondary fire from the muzzle along this transform's rotation,
//                         which CPlayer points at the aim-assist target
//   +0x4A8  mGunWorldXf   mXf * mGunLocalXf * the camera bob, then the recoil motion on top;
//                         what the cannon is drawn with and where the muzzle is
//   +0x832  bit 0x04      mLockedOn
//
// CPlayerGun::Update computes mGunWorldXf at 0x800412B8, after the locators are read; the
// patch at 0x800412AC (recomp/patches.txt) runs this just before it. Setting mXf there from
// the controller keeps the game's own animation, bob and recoil on top of the hand, and
// moves the muzzle with it. Setting mAssistAimXf's rotation sends the shots where the
// cannon points; when locked on it is left alone, so the shots still find the target.
//
// A CTransform4f is three rows of four floats: the columns are right, forward and up (Retro's
// world is z-up, y-forward) and the translation. Every CActor, cameras included, keeps its
// transform at +0x34.
//
// The controller's pose arrives in the eyes' frame (x right, y up, z back, origin at the
// camera). The renderer draws Prime's near layers scaled by foreground_scale about the
// camera, so a gun placed at h / foreground_scale is seen at h, at that scale.

constexpr uint32_t kGunXf = 0x3E8, kGunAssistAimXf = 0x478, kGunFlags = 0x832;
constexpr uint8_t kGunLockedOn = 0x04;
constexpr uint32_t kActorXf = 0x34;

// Where the cannon is, from mXf. At rest mXf's origin is 0.25 right, 0.30 ahead and 0.35
// below the camera, turned with it (GCN_GUNLOG), and the cannon is drawn in its own depth
// band, 1/32-1/8 (draws 783-792 of frame 9600 on the new-game route, GCN_DRAWLOG). Its body
// spans 0.37-1.00 ahead of the camera, so its centre is 0.38 in front of mXf's origin in the
// gun's own frame (x right, y up, z back). Seen at foreground_scale that is scaled too, so
// mXf's origin is put that much behind the hand, and the cannon's centre is seen kGunAhead
// in front of the controller's aim point.
constexpr float kCannonFromXf[3] = {0.0f, 0.0f, -0.38f};
constexpr float kGunAhead = 0.05f;

constexpr int kMaxSmoothing = 16;

struct GunConfig {
    float scale = 1.0f;            // foreground_scale
    float offset[3] = {};          // where mXf's origin is seen, in units in the controller's frame
    float pitch_rad = 0.0f;        // gun_pitch_deg: the cannon's tilt against the controller
    bool on = true;                // gun_follows_hand
    int smoothing = 3;             // gun_smoothing: game frames the hand is averaged over
    bool loaded = false;
};
GunConfig g_gun;

// Culling to the eyes, below.
struct CullConfig {
    float margin_rad = 10.0f * 3.14159265f / 180.0f;  // cull_margin_deg
    bool on = true;                                    // cull_to_eyes
};
CullConfig g_cull;

// gun_x, gun_y and gun_z move the cannon from there, in metres in the controller's frame.
void config_loaded(const VrConfig& c) {
    g_gun.scale = c.foreground_scale > 0.0f ? c.foreground_scale : 1.0f;
    const float u = c.units_per_metre;
    const float nudge[3] = {c.get("gun_x", 0.0f), c.get("gun_y", 0.0f), c.get("gun_z", 0.0f) - kGunAhead};
    for (int i = 0; i < 3; i++) g_gun.offset[i] = u * nudge[i] - g_gun.scale * kCannonFromXf[i];
    g_gun.pitch_rad = c.get("gun_pitch_deg", 0.0f) * 3.14159265f / 180.0f;
    g_gun.on = c.get("gun_follows_hand", 1.0f) != 0.0f;
    const int n = (int)c.get("gun_smoothing", 3.0f);
    g_gun.smoothing = n < 1 ? 1 : n > kMaxSmoothing ? kMaxSmoothing : n;
    g_gun.loaded = true;
    g_cull.margin_rad = c.get("cull_margin_deg", 10.0f) * 3.14159265f / 180.0f;
    g_cull.on = c.get("cull_to_eyes", 1.0f) != 0.0f;
}

inline float rd_f(uint32_t a) { const uint32_t u = mem_r32(a); float f; memcpy(&f, &u, 4); return f; }
inline void wr_f(uint32_t a, float f) { uint32_t u; memcpy(&u, &f, 4); mem_w32(a, u); }

void read_xf(uint32_t a, float m[12]) { for (int i = 0; i < 12; i++) m[i] = rd_f(a + 4 * i); }

// The eyes' frame to the camera's own: x right, y forward, z up.
inline void eye_to_camera(const float v[3], float out[3]) { out[0] = v[0]; out[1] = -v[2]; out[2] = v[1]; }

// `m`'s rotation applied to `v`.
inline void rotate(const float m[12], const float v[3], float out[3]) {
    for (int i = 0; i < 3; i++) out[i] = m[4 * i] * v[0] + m[4 * i + 1] * v[1] + m[4 * i + 2] * v[2];
}

// With GCN_GUNLOG set, the game's own mXf in the camera's frame whenever it moves by more
// than a little: where the cannon sits when nothing is in the hand.
void log_rest(uint32_t gun, const float cam[12]) {
    static float last[3] = {1e9f, 1e9f, 1e9f};
    float g[12], d[3], local[3], fwd[3];
    read_xf(gun + kGunXf, g);
    for (int i = 0; i < 3; i++) d[i] = g[4 * i + 3] - cam[4 * i + 3];
    for (int i = 0; i < 3; i++) local[i] = cam[i] * d[0] + cam[4 + i] * d[1] + cam[8 + i] * d[2];
    for (int i = 0; i < 3; i++) fwd[i] = cam[i] * g[1] + cam[4 + i] * g[5] + cam[8 + i] * g[9];
    if (fabsf(local[0] - last[0]) + fabsf(local[1] - last[1]) + fabsf(local[2] - last[2]) < 0.05f) return;
    memcpy(last, local, sizeof(last));
    fprintf(stderr, "[prime] gun at %.3f %.3f %.3f from the camera (right, forward, up), pointing %.3f %.3f %.3f\n",
            local[0], local[1], local[2], fwd[0], fwd[1], fwd[2]);
}

// GCN_GUN_HAND="x y z yaw pitch": a controller held still, for looking at the result without
// a headset -- metres in the eyes' frame, degrees left and up. The flat picture shows the
// cannon where the eye sees it, at twice the distance and size.
// The hand averaged over the last g_gun.smoothing game frames, to take the tracking's jitter
// out of the cannon. Positions are averaged as they are; orientations as quaternions, each
// turned to agree in sign with the newest and the sum renormalised, which is close enough
// for the small turns between neighbouring frames. A gap in tracking starts it afresh.
void smooth_hand(vr::HandPose* hand, bool tracked) {
    static vr::HandPose history[kMaxSmoothing];
    static int count = 0, next = 0;
    if (!tracked) {
        count = 0;
        return;
    }
    history[next] = *hand;
    next = (next + 1) % kMaxSmoothing;
    if (count < g_gun.smoothing) count++;
    vr::HandPose avg{};
    for (int k = 0; k < count; k++) {
        const vr::HandPose& h = history[(next - 1 - k + kMaxSmoothing) % kMaxSmoothing];
        const float d = h.rot[0] * hand->rot[0] + h.rot[1] * hand->rot[1] + h.rot[2] * hand->rot[2] + h.rot[3] * hand->rot[3];
        const float sign = d < 0.0f ? -1.0f : 1.0f;
        for (int i = 0; i < 3; i++) avg.pos[i] += h.pos[i] / count;
        for (int i = 0; i < 4; i++) avg.rot[i] += sign * h.rot[i];
    }
    const float len = sqrtf(avg.rot[0] * avg.rot[0] + avg.rot[1] * avg.rot[1] + avg.rot[2] * avg.rot[2] + avg.rot[3] * avg.rot[3]);
    for (int i = 0; i < 4; i++) avg.rot[i] /= len;
    *hand = avg;
}

bool stand_in_hand(vr::HandPose* out) {
    static const char* env = getenv("GCN_GUN_HAND");
    static vr::HandPose pose;
    static const bool ok = [] {
        float yaw, pitch;
        if (!env || sscanf(env, "%f %f %f %f %f", &pose.pos[0], &pose.pos[1], &pose.pos[2], &yaw, &pitch) != 5)
            return false;
        const float h = 3.14159265f / 360.0f;  // half a degree, in radians
        const float cy = cosf(yaw * h), sy = sinf(yaw * h), cp = cosf(pitch * h), sp = sinf(pitch * h);
        // Yaw about y, then pitch about the turned x.
        pose.rot[0] = cy * sp;
        pose.rot[1] = sy * cp;
        pose.rot[2] = -sy * sp;
        pose.rot[3] = cy * cp;
        return true;
    }();
    if (ok) *out = pose;
    return ok;
}

void config_defaults(VrConfig& c);

// The desktop reads no vr.txt; it gets the defaults the headset would start from.
void ensure_config() {
    if (g_gun.loaded) return;
    VrConfig d;
    config_defaults(d);
    config_loaded(d);
}

void aim_gun(uint32_t gun) {
    static const bool log = getenv("GCN_GUNLOG") != nullptr;
    ensure_config();
    const uint32_t pair = g_camera_pair.load(std::memory_order_relaxed);
    if (!pair) return;
    const uint32_t camera = mem_r32(pair);
    if (!is_camera(camera, kFirstPersonCameraVtable)) return;
    float cam[12];
    read_xf(camera + kActorXf, cam);

    vr::HandPose hand;
    const bool tracked = g_gun.on && (vr::hand_pose(vr::kRightHand, &hand) || stand_in_hand(&hand));
    smooth_hand(&hand, tracked);
    if (!tracked) {
        if (log) log_rest(gun, cam);
        return;
    }

    // The controller's axes in the eyes' frame, tilted by gun_pitch_deg about its own x.
    const float x = hand.rot[0], y = hand.rot[1], z = hand.rot[2], w = hand.rot[3];
    float ax[3] = {1 - 2 * (y * y + z * z), 2 * (x * y + w * z), 2 * (x * z - w * y)};
    float ay[3] = {2 * (x * y - w * z), 1 - 2 * (x * x + z * z), 2 * (y * z + w * x)};
    float az[3] = {2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)};
    const float cp = cosf(g_gun.pitch_rad), sp = sinf(g_gun.pitch_rad);
    for (int i = 0; i < 3; i++) {
        const float yi = ay[i], zi = az[i];
        ay[i] = cp * yi + sp * zi;
        az[i] = -sp * yi + cp * zi;
    }

    // Where the gun's origin is seen, in the eyes' frame, then where it has to be put.
    float at[3];
    for (int i = 0; i < 3; i++)
        at[i] = (hand.pos[i] + ax[i] * g_gun.offset[0] + ay[i] * g_gun.offset[1] + az[i] * g_gun.offset[2]) /
                g_gun.scale;

    // Into the camera's frame (the cannon points along the controller's -z), then the world.
    const float back[3] = {-az[0], -az[1], -az[2]};
    float right_c[3], fwd_c[3], up_c[3], at_c[3];
    eye_to_camera(ax, right_c);
    eye_to_camera(back, fwd_c);
    eye_to_camera(ay, up_c);
    eye_to_camera(at, at_c);
    float right[3], fwd[3], up[3], pos[3];
    rotate(cam, right_c, right);
    rotate(cam, fwd_c, fwd);
    rotate(cam, up_c, up);
    rotate(cam, at_c, pos);
    for (int i = 0; i < 3; i++) pos[i] += cam[4 * i + 3];

    for (int i = 0; i < 3; i++) {
        const uint32_t row = gun + kGunXf + 16 * i;
        wr_f(row, right[i]);
        wr_f(row + 4, fwd[i]);
        wr_f(row + 8, up[i]);
        wr_f(row + 12, pos[i]);
    }
    if (!(mem_r8(gun + kGunFlags) & kGunLockedOn)) {
        for (int i = 0; i < 3; i++) {
            const uint32_t row = gun + kGunAssistAimXf + 16 * i;
            wr_f(row, right[i]);
            wr_f(row + 4, fwd[i]);
            wr_f(row + 8, up[i]);
        }
    }
}

// ---------------------------------------------------------------------------
// Culling against what the eyes see.
//
// Prime culls the world, its actors and their particles against the camera's own frustum,
// and tightly, so turning the head in the headset shows things missing at the edges. Every
// frustum it culls the frame with comes from one constructor,
// __ct__14CFrustumPlanesFRC12CTransform4ffffbf (0x80345CEC), given the camera's transform
// in r4; the patches in recomp/patches.txt route three of its calls through this --
// CStateManager::SetupViewForDraw (DrawWorld's frustum, and the renderer's clipping planes
// for the area geometry), CStateManager::PreRender (each actor's PreRender) and
// CStateManager::ResetViewAfterDraw (the planes put back after the world). Its other callers
// are a light's view for shadows and the HUD's markers, left as they are.
//
// CFrustumPlanes: a count at +0, then that many planes of four floats (n, d), a point
// outside when n.p >= d: the normals point out (PointInFrustumPlanes). The count is 5 or 6:
// the near plane, four sides and maybe a far one; on the frigate SetupViewForDraw and
// PreRender build 5, the near plane first. Below, planes are worked with inward, n.p > d,
// and turned back out as they are written.
// Each plane the game built is replaced by its counterpart around the eyes, in the same
// slot. The counterpart is one frustum around both eyes: the head's orientation (the left
// eye's), wide enough for both fields, its apex between the eyes and each plane moved out by
// half their separation, which contains each eye's own frustum. The game culls a frame or
// two before the frame is shown, and the head can turn in between, so each side is widened
// by cull_margin_deg as well.

extern "C" void fn_80345CEC(CPU* c);  // __ct__14CFrustumPlanesFRC12CTransform4ffffbf


// `v` turned by the unit quaternion `q`, or by its inverse.
void qrot(const float q[4], const float v[3], float out[3], bool inverse = false) {
    const float x = inverse ? -q[0] : q[0], y = inverse ? -q[1] : q[1], z = inverse ? -q[2] : q[2], w = q[3];
    const float t[3] = {2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])};
    out[0] = v[0] + w * t[0] + (y * t[2] - z * t[1]);
    out[1] = v[1] + w * t[1] + (z * t[0] - x * t[2]);
    out[2] = v[2] + w * t[2] + (x * t[1] - y * t[0]);
}

// GCN_CULL_HEAD="yaw pitch": eyes held still, for looking at the result without a headset
// -- degrees left and up, a Quest-like field, 64 mm apart. The flat picture then shows the
// world culled to where that head looks.
bool stand_in_eyes(vr::EyeView out[2]) {
    static const char* env = getenv("GCN_CULL_HEAD");
    static vr::EyeView eyes[2];
    static const bool ok = [] {
        float yaw, pitch;
        if (!env || sscanf(env, "%f %f", &yaw, &pitch) != 2) return false;
        const float h = 3.14159265f / 360.0f;
        const float cy = cosf(yaw * h), sy = sinf(yaw * h), cp = cosf(pitch * h), sp = sinf(pitch * h);
        for (int e = 0; e < 2; e++) {
            vr::EyeView& v = eyes[e];
            v.rot[0] = cy * sp;
            v.rot[1] = sy * cp;
            v.rot[2] = -sy * sp;
            v.rot[3] = cy * cp;
            const float side[3] = {e ? 0.032f : -0.032f, 0.0f, 0.0f};
            qrot(v.rot, side, v.pos);
            v.tan_left = -1.19f;  // 50 degrees
            v.tan_right = 1.19f;
            v.tan_up = 1.0f;
            v.tan_down = -1.0f;
        }
        return true;
    }();
    if (ok) memcpy(out, eyes, sizeof(eyes));
    return ok;
}

void cull_to_eyes(CPU* c) {
    const uint32_t frustum = c->r[3], xf = c->r[4];
    fn_80345CEC(c);
    ensure_config();
    vr::EyeView ev[2];
    if (!g_cull.on || !(vr::eye_views(ev) || stand_in_eyes(ev))) return;
    const uint32_t count = mem_r32(frustum);
    if (count > 6) return;

    // The eyes' field in the head's frame (the left eye's orientation): each eye's corner
    // rays turned into it, so that eyes which are not parallel are covered too.
    const float* qh = ev[0].rot;
    float lo_x = 0, hi_x = 0, lo_y = 0, hi_y = 0;
    bool first = true;
    for (int e = 0; e < 2; e++)
        for (int k = 0; k < 4; k++) {
            const float d[3] = {k & 1 ? ev[e].tan_right : ev[e].tan_left, k & 2 ? ev[e].tan_up : ev[e].tan_down, -1};
            float w[3], h[3];
            qrot(ev[e].rot, d, w);
            qrot(qh, w, h, true);
            if (-h[2] < 1e-3f) return;  // an eye looking sideways off the head: leave the game's
            const float tx = h[0] / -h[2], ty = h[1] / -h[2];
            if (first || tx < lo_x) lo_x = tx;
            if (first || tx > hi_x) hi_x = tx;
            if (first || ty < lo_y) lo_y = ty;
            if (first || ty > hi_y) hi_y = ty;
            first = false;
        }
    // Each side widened by the margin, short of a right angle.
    const float limit = 85.0f * 3.14159265f / 180.0f;
    auto widen = [&](float t) { return tanf(fminf(atanf(t) + g_cull.margin_rad, limit)); };
    const float L = widen(-lo_x), R = widen(hi_x), D = widen(-lo_y), U = widen(hi_y);

    float apex[3], slack = 0;
    for (int i = 0; i < 3; i++) {
        apex[i] = 0.5f * (ev[0].pos[i] + ev[1].pos[i]);
        const float s = 0.5f * (ev[0].pos[i] - ev[1].pos[i]);
        slack += s * s;
    }
    slack = sqrtf(slack);

    float cam[12];
    read_xf(xf, cam);
    const float cam_pos[3] = {cam[3], cam[7], cam[11]};
    const float right[3] = {cam[0], cam[4], cam[8]}, fwd[3] = {cam[1], cam[5], cam[9]}, up[3] = {cam[2], cam[6], cam[10]};
    // The eyes' frame to the world: x right, y up, z back about the camera.
    auto to_world = [&](const float v[3], float out[3]) {
        float cv[3];
        eye_to_camera(v, cv);
        rotate(cam, cv, out);
    };
    float apex_w[3];
    to_world(apex, apex_w);
    for (int i = 0; i < 3; i++) apex_w[i] += cam_pos[i];
    auto dot = [](const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };

    static const bool log = getenv("GCN_CULLLOG") != nullptr;
    static uint32_t calls = 0;
    const bool show = log && calls++ % 600 == 0;
    if (show)
        fprintf(stderr, "[prime] cull %08X: camera right %.2f %.2f %.2f fwd %.2f %.2f %.2f up %.2f %.2f %.2f at %.2f %.2f %.2f\n",
                c->lr, right[0], right[1], right[2], fwd[0], fwd[1], fwd[2], up[0], up[1], up[2], cam_pos[0], cam_pos[1],
                cam_pos[2]);

    // Normals in the head's frame, inward: left, right, bottom, top, near, far.
    const float sides[6][3] = {{1, 0, -L}, {-1, 0, -R}, {0, 1, -D}, {0, -1, -U}, {0, 0, -1}, {0, 0, 1}};
    for (uint32_t p = 0; p < count; p++) {
        const uint32_t at = frustum + 4 + 16 * p;
        const float n[3] = {-rd_f(at), -rd_f(at + 4), -rd_f(at + 8)}, d = -rd_f(at + 12);
        const float f = dot(n, fwd), r = dot(n, right), u = dot(n, up);
        const int which = f > 0.95f ? 4 : f < -0.95f ? 5 : fabsf(r) > fabsf(u) ? (r > 0 ? 0 : 1) : (u > 0 ? 2 : 3);
        float nh[3], ne[3], nw[3];
        const float len = sqrtf(dot(sides[which], sides[which]));
        for (int i = 0; i < 3; i++) nh[i] = sides[which][i] / len;
        qrot(qh, nh, ne);
        to_world(ne, nw);
        // The far plane keeps the game's distance, measured from the eyes; every other
        // plane passes through the eyes.
        const float far = which == 5 ? -(d - dot(n, cam_pos)) : 0.0f;
        if (show)
            fprintf(stderr, "[prime]   plane %u (%d): game %.3f %.3f %.3f %.2f -> %.3f %.3f %.3f %.2f\n", p, which, n[0], n[1],
                    n[2], d, nw[0], nw[1], nw[2], dot(nw, apex_w) - slack - far);
        wr_f(at, -nw[0]);
        wr_f(at + 4, -nw[1]);
        wr_f(at + 8, -nw[2]);
        wr_f(at + 12, -(dot(nw, apex_w) - slack - far));
    }

    if (show)
        fprintf(stderr, "[prime] culling to the eyes: %u planes, half-fields %.1f left %.1f right %.1f down %.1f up\n",
                count, atanf(L) * 57.2958f, atanf(R) * 57.2958f, atanf(D) * 57.2958f, atanf(U) * 57.2958f);
}

void config_defaults(VrConfig& c) {
    // Prime's world units are taken to be metres. A guess, to be judged from inside the
    // headset like Blue Storm's 50 was.
    c.units_per_metre = 1.0f;
    // Close enough to see the arm cannon, far enough to keep depth precision for rooms
    // that are a few hundred metres across.
    c.near_m = 0.05f;
    c.far_m = 1000.0f;
    // Prime gives each layer its own band of the depth buffer through the viewport's z
    // range (CGraphics::SetDepthRange). In one frame of play on the frigate: the sky in
    // 0.999-1, the world in 0.125-1, the arm cannon in 1/32-1/8, and the visor frame and
    // the HUD in 0-1/512. The sky is modelled about 58 units out around the camera, so it
    // is drawn at infinity instead. The cannon is modelled 0.4-1.0 ahead and read as twice
    // its size in stereo, so everything nearer than the world is drawn at half the distance
    // and half the size, at the same angular size.
    // See docs/dev/vr.md.
    c.background_band = 0.99f;
    c.foreground_band = 0.5f;
    c.foreground_scale = 0.5f;
    // Leaving stereo is a cut, not a fold. Every exit -- the pause screen's blur, the ball,
    // a cinematic, the world's name between worlds -- is noticed only once the game is
    // already drawing something that looks wrong in stereo, so the fold would only show
    // more of it. Entering keeps transition_s.
    c.transition_out_s = 0.0f;
    // The menus, the map and the ball are drawn by the game as 3D scenes, so the panel
    // shows them as a stereo pair, a window into the room rather than a picture of it.
    c.theater_stereo = true;
    // Its HUD layer, the band below 1/512 (the visor frame, the map's own frame), goes on
    // the panel itself; the map's rooms, drawn with no band, keep their depth behind it.
    c.panel_band = 1.0f / 512.0f;
}

const bool installed = [] {
    vr::GameHooks h;
    h.config_defaults = config_defaults;
    h.config_loaded = config_loaded;
    h.wants_stereo = wants_stereo;
    vr::set_game_hooks(h);
    return true;
}();

}  // namespace

// The patch in CPlayerGun::Update (recomp/patches.txt); r28 is the gun.
extern "C" void prime_aim_gun(CPU* c) { aim_gun(c->r[28]); }

// The patches in CMFGame::Draw and CMFGameLoader::Draw (recomp/patches.txt): whether the
// game draws its world this frame.
extern "C" void prime_world_drawn(uint32_t drawn) { g_world_drawn.store(drawn, std::memory_order_relaxed); }

// The patches at three calls of the CFrustumPlanes constructor (recomp/patches.txt), in
// place of the call.
extern "C" void prime_cull_frustum(CPU* c) { cull_to_eyes(c); }
