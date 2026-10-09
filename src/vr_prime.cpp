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
    if (log) {
        static uint32_t last = ~0u, last_cine = ~0u;
        if (current != last || cinematic != last_cine) {
            last = current;
            last_cine = cinematic;
            fprintf(stderr, "[prime] current camera %04X, %u cinematic (first person %04X, ball %04X)\n",
                    current, cinematic, first_person, mem_r16(mem_r32(pair + 4) + 8));
        }
    }
    return current == first_person && cinematic == 0;
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

void aim_gun(uint32_t gun) {
    static const bool log = getenv("GCN_GUNLOG") != nullptr;
    // The desktop reads no vr.txt; it gets the defaults the headset would start from.
    if (!g_gun.loaded) {
        VrConfig d;
        config_defaults(d);
        config_loaded(d);
    }
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
