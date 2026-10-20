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
#include "hw/pad.h"
#include "input_log.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

uint32_t gx_frames_submitted();  // gx/xf.cpp: frames the guest has finished

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

// The guest thread's own copy, for the hooks that steer the game (the cannon, the scan, the
// camera's turn). It must not be the render thread's: that one is found when the render
// thread happens to search, every 120 of its frames, and until then the hooks left the game
// to itself -- so what they did over the first frames of a world depended on how fast the
// host drew, and a replay in the harness parted from a session recorded in the headset four
// seconds in (20261015-194904, the scan zone at frame 4811). This one is a function of guest
// memory alone: checked at every use and searched for again whenever it does not check out,
// at most once in 60 failed calls, which are only made in play, where the pair is there to find.
// The logs read g_guest_pair as it stands, so that turning one on does not move a search.
uint32_t g_guest_pair = 0;
uint32_t guest_camera_pair() {
    static uint32_t failed = 0;
    if (g_guest_pair && live_pair(g_guest_pair)) return g_guest_pair;
    g_guest_pair = 0;
    if (failed++ % 60) return 0;
    g_guest_pair = search();
    if (g_guest_pair) failed = 0;
    return g_guest_pair;
}

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
// The map screen is flat.
//
// The panel shows the game's flat screens as a stereo pair (theater_stereo), but the map
// screen reads badly that way, so it goes up flat, one image for both eyes. CAutoMapper
// (revision 2's layout, from IsInMapperState): the current state at +0x1BC and the one it
// is moving to at +0x1C0, each an EAutoMapperState -- 0 the HUD's minimap, 1 the map
// screen, 2 the universe map. CAutoMapper::Draw runs every frame the map or the minimap is
// shown, and a patch there (recomp/patches.txt) says where the automapper is and when it
// last drew. The pause screen leaves it on the minimap, so it keeps its pair. Only an
// automapper that drew in the last few frames is read: a cinematic draws no minimap, and
// across an elevator ride the one recorded was the previous world's, freed, whose memory
// read as the map and turned the ride's second half flat.
constexpr uint32_t kMapperState = 0x1BC, kMapperNextState = 0x1C0;
std::atomic<uint32_t> g_automapper{0}, g_automapper_frame{0};

bool wants_panel_pair(const gx::Batch&) {
    const uint32_t m = g_automapper.load(std::memory_order_relaxed);
    if (!g_camera_pair.load(std::memory_order_relaxed) || !in_ram(m) || (m & 3)) return true;
    if (gx_frames_submitted() - g_automapper_frame.load(std::memory_order_relaxed) > 4) return true;
    return mem_r32(m + kMapperState) == 0 && mem_r32(m + kMapperNextState) == 0;
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
    // gamepad_gun_x/y/z: with a gamepad in stereo, how far the game's own cannon is moved,
    // in units in the camera's frame (right, forward, up) before foreground_scale.
    float pad_offset[3] = {};
    bool loaded = false;
};
GunConfig g_gun;

// Culling to the eyes, below.
struct CullConfig {
    float margin_rad = 10.0f * 3.14159265f / 180.0f;  // cull_margin_deg
    bool on = true;                                    // cull_to_eyes
};
CullConfig g_cull;
bool g_scan_on = true;  // scan_follows_head
bool g_follow_on = true;  // camera_follows_head
bool g_lock_follows_gaze = true;  // lock_on_follows_gaze
bool g_pad_classic = false;       // gamepad_classic: the GameCube's controls, not Remastered's
float g_pad_pitch_rate = 2.6f;    // gamepad_look_deg_per_s, as radians a second at full tilt
struct FollowConfig {
    float reach = 0.32f;     // camera_follow_reach
    float out_rate = 8.0f;   // camera_follow_rate
    float back_rate = 4.0f;  // camera_return_rate
};
FollowConfig g_follow;
// The frame the eyes paint the game's 2D elements on (gx::render_hud_frame), as the
// headset frontend builds it from vr.txt: the scan window is placed through it.
struct HudFrame { float dist, scale, height, pitch_rad; };
HudFrame g_hud_frame = {4.0f, 0.5f, 0.0f, 0.0f};
bool g_hide_helmet = true;      // hide_helmet
bool g_hide_flat_warps = true;  // hide_flat_warps
bool g_hide_scan_tint = true;   // hide_scan_tint
bool g_hide_scan_magnifier = true;  // hide_scan_magnifier
bool g_visor_film_on_head = true;  // visor_film_on_head
bool g_visor_drops_on_head = true;  // visor_drops_on_head
bool g_cannon_cutaway = true;      // cannon_cutaway
bool g_screen_filter_on_head = true;  // screen_filter_on_head
bool g_gaze_field_centre = true;  // scan_gaze_field: the gaze is the centre of the visible field, not the eye's axis
float g_gaze_pitch_rad = 0.0f;    // scan_gaze_pitch_deg: and this much further up
bool g_scan_icons_true = true;    // scan_icons_true_distance
bool g_scan_window_faces = true;  // scan_window_faces
bool g_reticle_true_scale = true; // reticle_true_distance

// gun_x, gun_y and gun_z move the cannon from there, in metres in the controller's frame.
void config_loaded(const VrConfig& c) {
    g_gun.scale = c.foreground_scale > 0.0f ? c.foreground_scale : 1.0f;
    const float u = c.units_per_metre;
    const float nudge[3] = {c.get("gun_x", 0.0f), c.get("gun_y", 0.0f), c.get("gun_z", 0.0f) - kGunAhead};
    for (int i = 0; i < 3; i++) g_gun.offset[i] = u * nudge[i] - g_gun.scale * kCannonFromXf[i];
    g_gun.pitch_rad = c.get("gun_pitch_deg", 0.0f) * 3.14159265f / 180.0f;
    g_gun.on = c.get("gun_follows_hand", 1.0f) != 0.0f;
    // Metres as the viewer sees them, x right, y up, z back: 8 inches right and 8 down,
    // out of the face, where the console's place for it put it in the headset.
    const float pad[3] = {c.get("gamepad_gun_x", 0.2032f), c.get("gamepad_gun_y", -0.2032f),
                          c.get("gamepad_gun_z", 0.0f)};
    g_gun.pad_offset[0] = u * pad[0] / g_gun.scale;
    g_gun.pad_offset[1] = -u * pad[2] / g_gun.scale;
    g_gun.pad_offset[2] = u * pad[1] / g_gun.scale;
    const int n = (int)c.get("gun_smoothing", 3.0f);
    g_gun.smoothing = n < 1 ? 1 : n > kMaxSmoothing ? kMaxSmoothing : n;
    g_gun.loaded = true;
    g_cull.margin_rad = c.get("cull_margin_deg", 10.0f) * 3.14159265f / 180.0f;
    g_cull.on = c.get("cull_to_eyes", 1.0f) != 0.0f;
    g_scan_on = c.get("scan_follows_head", 1.0f) != 0.0f;
    g_follow_on = c.get("camera_follows_head", 1.0f) != 0.0f;
    g_lock_follows_gaze = c.get("lock_on_follows_gaze", 1.0f) != 0.0f;
    g_pad_classic = c.get("gamepad_classic", 0.0f) != 0.0f;
    g_pad_pitch_rate = c.get("gamepad_look_deg_per_s", 150.0f) * 3.14159265f / 180.0f;
    g_follow.reach = c.get("camera_follow_reach", 0.32f);
    g_follow.out_rate = c.get("camera_follow_rate", 8.0f);
    g_follow.back_rate = c.get("camera_return_rate", 4.0f);
    g_hud_frame = {c.hud_distance_m * u, c.hud_scale, c.hud_height_m * u, c.hud_pitch_deg * 3.14159265f / 180.0f};
    g_hide_helmet = c.get("hide_helmet", 1.0f) != 0.0f;
    g_hide_flat_warps = c.get("hide_flat_warps", 1.0f) != 0.0f;
    g_hide_scan_tint = c.get("hide_scan_tint", 1.0f) != 0.0f;
    g_hide_scan_magnifier = c.get("hide_scan_magnifier", 1.0f) != 0.0f;
    g_visor_film_on_head = c.get("visor_film_on_head", 1.0f) != 0.0f;
    g_visor_drops_on_head = c.get("visor_drops_on_head", 1.0f) != 0.0f;
    g_cannon_cutaway = c.get("cannon_cutaway", 1.0f) != 0.0f;
    g_screen_filter_on_head = c.get("screen_filter_on_head", 1.0f) != 0.0f;
    g_gaze_field_centre = c.get("scan_gaze_field", 1.0f) != 0.0f;
    g_gaze_pitch_rad = c.get("scan_gaze_pitch_deg", 0.0f) * 3.14159265f / 180.0f;
    g_scan_icons_true = c.get("scan_icons_true_distance", 1.0f) != 0.0f;
    g_scan_window_faces = c.get("scan_window_faces", 1.0f) != 0.0f;
    g_reticle_true_scale = c.get("reticle_true_distance", 1.0f) != 0.0f;
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

// `v` turned by the unit quaternion `q`, or by its inverse.
void qrot(const float q[4], const float v[3], float out[3], bool inverse = false) {
    const float x = inverse ? -q[0] : q[0], y = inverse ? -q[1] : q[1], z = inverse ? -q[2] : q[2], w = q[3];
    const float t[3] = {2 * (y * v[2] - z * v[1]), 2 * (z * v[0] - x * v[2]), 2 * (x * v[1] - y * v[0])};
    out[0] = v[0] + w * t[0] + (y * t[2] - z * t[1]);
    out[1] = v[1] + w * t[1] + (z * t[0] - x * t[2]);
    out[2] = v[2] + w * t[2] + (x * t[1] - y * t[0]);
}

// The product a * b of two quaternions: b's turn, then a's.
void qmul(const float a[4], const float b[4], float out[4]) {
    out[0] = a[3] * b[0] + b[3] * a[0] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] + b[3] * a[1] + a[2] * b[0] - a[0] * b[2];
    out[2] = a[3] * b[2] + b[3] * a[2] + a[0] * b[1] - a[1] * b[0];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// How far the first-person camera is turned after the head (below, "The camera follows the
// head"): yaw left about the eyes' up, then pitch up about the turned right, and the same as
// a quaternion in the eyes' frame. The camera's transform has it in from the patch in
// CFirstPersonCamera::Think until the game next rebuilds it, and the poses the frontend
// publishes are in the frame before it: a hook that sets them against the camera takes the
// turn off them first (unturn_pose).
struct CameraTurn {
    float yaw = 0.0f, pitch = 0.0f;
    float yaw_rate = 0.0f, pitch_rate = 0.0f;  // radians a second
    float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
};
CameraTurn g_turn;

// A pose in the frame the frontend publishes, into the turned camera's.
void unturn_pose(float pos[3], float rot[4]) {
    float p[3], r[4];
    const float inv[4] = {-g_turn.q[0], -g_turn.q[1], -g_turn.q[2], g_turn.q[3]};
    qrot(g_turn.q, pos, p, true);
    qmul(inv, rot, r);
    memcpy(pos, p, sizeof(p));
    memcpy(rot, r, sizeof(r));
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
bool stand_in_eyes(vr::EyeView ev[2]);

// Moves the game's own cannon, where it put it this frame, by gamepad_gun_* in the camera's
// frame. The game builds mXf afresh every frame before the hook, so this does not add up.
void offset_rest(uint32_t gun, const float cam[12]) {
    for (int i = 0; i < 3; i++) {
        const uint32_t t = gun + kGunXf + 16 * i + 12;
        wr_f(t, rd_f(t) + cam[4 * i] * g_gun.pad_offset[0] + cam[4 * i + 1] * g_gun.pad_offset[1] +
                    cam[4 * i + 2] * g_gun.pad_offset[2]);
    }
}

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
    const uint32_t pair = guest_camera_pair();
    if (!pair) return;
    const uint32_t camera = mem_r32(pair);
    if (!is_camera(camera, kFirstPersonCameraVtable)) return;
    float cam[12];
    read_xf(camera + kActorXf, cam);

    // A controller that loses its pose -- out of the headset's cameras, or the system menu
    // taking the controllers over -- leaves the cannon where it last was in the hand. Left
    // to the game, it went back to where it sits on a television, low and a few tens of
    // centimetres out, where the headset's wider field looks into the open back of the
    // model (the session 20261014-225445 ended on such a pose).
    static vr::HandPose last_hand;
    static bool have_last = false;
    vr::HandPose hand;
    // With a gamepad the cannon is the game's, moved out of the face (gamepad_gun_*) while
    // the eyes are drawn; no controller's pose, current or last, places it.
    const bool pad = vr::gamepad_play();
    const bool tracked = g_gun.on && !pad && (vr::hand_pose(vr::kRightHand, &hand) || stand_in_hand(&hand));
    smooth_hand(&hand, tracked);
    if (tracked) {
        last_hand = hand;
        have_last = true;
    } else if (g_gun.on && !pad && have_last) {
        hand = last_hand;
    } else {
        vr::EyeView ev[2];
        if (pad && (vr::eye_views(ev) || stand_in_eyes(ev))) offset_rest(gun, cam);
        if (log) log_rest(gun, cam);
        return;
    }
    unturn_pose(hand.pos, hand.rot);

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

// The eyes -- the headset's, or GCN_CULL_HEAD's -- in the turned camera's frame.
// The last eyes it gave, for the logs: a read of the eyes is input, counted and logged
// (input_pose), so a log that read them again would make a session recorded with it on
// replay apart from one without.
vr::EyeView g_last_eyes[2];
uint32_t g_last_eyes_frame = ~0u - 8;
bool camera_eyes(vr::EyeView ev[2]) {
    if (!(vr::eye_views(ev) || stand_in_eyes(ev))) return false;
    g_last_eyes_frame = gx_frames_submitted();
    g_last_eyes[0] = ev[0];
    g_last_eyes[1] = ev[1];
    for (int e = 0; e < 2; e++) unturn_pose(ev[e].pos, ev[e].rot);
    return true;
}

void cull_to_eyes(CPU* c) {
    const uint32_t frustum = c->r[3], xf = c->r[4];
    fn_80345CEC(c);
    ensure_config();
    vr::EyeView ev[2];
    if (!g_cull.on || !camera_eyes(ev)) return;
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

// ---------------------------------------------------------------------------
// The scan visor follows the head.
//
// Prime's scan visor looks straight down the camera. The window that magnifies the view
// (CPlayerVisor::DrawScanEffect) sits at the centre of the screen, and a scannable object
// is found by projecting it with the first-person camera and asking whether it lands in
// the scan zone, a box of screen pixels around that same centre
// (CPlayer::FindOrbitableObjects, CPlayer::WithinOrbitScreenBox, the box from CTweakPlayer).
// On the GameCube only the free look moves the camera, which in the headset turns the world
// under the viewer. The Wii version moved the window and the zone to the pointer
// (CTweakPlayer::GetOrbitZoneCentreX there reads the cursor); here they move to where the
// head looks, projected through the game's camera, so that looking at an object scans it.
//
//  - Once a frame (a patch at CPlayer::UpdateOrbitZone, 0x8017E96C, recomp/patches.txt) the
//    head's direction in the camera's frame is projected to screen pixels and written to
//    the scan zone's centre and ideal point in the tweaks: CTweakPlayer's
//    mOrbitScreenBoxCenterX/Y and mOrbitZoneIdealX/Y for zone 1, which the generated code
//    of WithinOrbitScreenBox reads at +0x1B8 and +0x1C0 plus four bytes a zone. The zone's
//    size stays the game's, and the game's own values are put back when the eyes go away.
//  - DrawScanEffect copies the EFB around the centre of the viewport (GXSetTexCopySrc at
//    0x80113998) and draws the window with an ortho projection centred on the screen
//    (gpRender->SetViewportOrtho(true, -1, 1), the virtual call at 0x801139EC). One patch
//    moves the copy to the head's point; another, once the ortho is set, sets it again
//    through CGraphics::SetOrtho shifted by the point's offset from the centre, so the
//    window, its frame and the copy inside follow. The point is kept far enough inside the
//    screen for the window to fit, so looking past the edge of the visor pins it there.
//  - Locking on (L) in the scan visor turns the body to face the target
//    (CPlayer::UpdateOrbitOrientation) and the camera too
//    (CFirstPersonCamera::UpdateTransform, which looks at the orbit point). The viewer is
//    already facing it, and the turn would move the world under them, and the zone, which
//    follows the head, off the target. While the scan follows the head, the reads of
//    CPlayer::mOrbitState (+0x314) in both are answered "no orbit" in the scan visor; the
//    game's own copy of the state, which the scanning itself checks, is untouched.
//
// Which visor is up is CPlayer::mOrbitZoneMode (+0x340), which UpdateOrbitZone sets to 1 in
// the scan visor. The first-person camera's field of view (vertical, degrees) and aspect
// are at +0x16C and +0x178 of CGameCamera, the viewport is CGraphics::mViewport. All of
// this is only while the eyes are published -- stereo in the headset, GCN_CULL_HEAD on the
// desktop -- and scan_follows_head 0 in vr.txt turns it off; otherwise the scan is the
// game's own. GCN_SCANLOG=1 prints the point as it moves.

constexpr uint32_t kTweakPlayer = 0x805A9D78;  // gpTweakPlayer
constexpr uint32_t kScanZoneFields[4] = {0x1BC, 0x1C4, 0x1CC, 0x1D4};  // centre x, y, ideal x, y of zone 1
constexpr uint32_t kViewport = 0x803EE928;     // CGraphics::mViewport: left, top, width, height
constexpr uint32_t kCameraFov = 0x16C, kCameraAspect = 0x178;
constexpr uint32_t kPlayerOrbitState = 0x314, kPlayerOrbitZoneMode = 0x340;
constexpr uint32_t kPlayerMorphState = 0x308;  // CPlayer::mMorphBallState, 0 unmorphed
uint32_t g_player;  // the CPlayer the movement hooks last saw (turn_input), for turn_camera
// The scan window at its idle size: 169 by 152 pixels (DrawScanEffect's 169.218 and
// 152.218 at a window scale of 1).
constexpr float kWindowHalfW = 85.0f, kWindowHalfH = 77.0f;

extern "C" void fn_80379B4C(CPU* c);  // GXSetTexCopySrc
extern "C" void fn_8030CFA4(CPU* c);  // CGraphics::SetOrtho(left, right, top, bottom, near, far)

// The gaze. An eye's pose points along its optical axis, but a headset's field is not
// centred on that axis: a Quest 3 sees 44 degrees above it and 55 below, so the centre of
// what the viewer sees -- what reads as "straight ahead" -- is some 10 degrees below the
// axis. A window placed on the axis sat half its height above where the viewer looked (the
// headset's own log put the game's zone where the maths said, and the viewer disagreed). So
// the gaze is the direction of the field's centre, the mean of the four edge tangents,
// which is the axis itself on a symmetric field; scan_gaze_field 0 in vr.txt takes the axis,
// and scan_gaze_pitch_deg nudges either. A unit vector, in the eyes' frame.
void gaze(const vr::EyeView ev[2], float d[3]) {
    d[0] = d[1] = d[2] = 0.0f;
    for (int e = 0; e < 2; e++) {
        float ahead[3] = {0, 0, -1};
        if (g_gaze_field_centre) {
            ahead[0] = 0.5f * (ev[e].tan_left + ev[e].tan_right);
            ahead[1] = 0.5f * (ev[e].tan_up + ev[e].tan_down);
        }
        if (g_gaze_pitch_rad != 0.0f) {
            const float cp = cosf(g_gaze_pitch_rad), sp = sinf(g_gaze_pitch_rad);
            const float y = ahead[1] * cp - ahead[2] * sp, z = ahead[1] * sp + ahead[2] * cp;
            ahead[1] = y; ahead[2] = z;
        }
        float f[3];
        qrot(ev[e].rot, ahead, f);
        const float len = sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        for (int i = 0; i < 3; i++) d[i] += f[i] / (len > 0.0f ? len : 1.0f);
    }
    const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    for (int i = 0; i < 3; i++) d[i] = len > 0.0f ? d[i] / len : (i == 2 ? -1.0f : 0.0f);
}

// How far the scan window's centre can go from the screen's, in normalised device
// coordinates, and still fit the window on the screen: the region the head moves it in.
// False without a viewport.
bool window_reach(float lim[2]) {
    const float W = (float)(int32_t)mem_r32(kViewport + 8), H = (float)(int32_t)mem_r32(kViewport + 12);
    if (!(W > 0.0f) || !(H > 0.0f)) return false;
    lim[0] = 1.0f - 2.0f * kWindowHalfW / W;
    lim[1] = 1.0f - 2.0f * kWindowHalfH / H;
    return true;
}

// The first-person camera's projection: the tangents of half its picture's width and height.
bool camera_tangents(uint32_t camera, float k[2]) {
    const float t = tanf(rd_f(camera + kCameraFov) * 0.5f * 3.14159265f / 180.0f);
    const float aspect = rd_f(camera + kCameraAspect);
    if (!(t > 0.0f) || !(aspect > 0.0f)) return false;
    k[0] = aspect * t;
    k[1] = t;
    return true;
}

// A direction in the camera's frame (the eyes' axes) through that projection, to normalised
// device coordinates; one behind the camera lands far off the side it leans to.
void project(const float d[3], const float k[2], float n[2]) {
    if (d[2] < -1e-3f) {
        n[0] = d[0] / -d[2] / k[0];
        n[1] = d[1] / -d[2] / k[1];
    } else {
        n[0] = d[0] * 1e3f;
        n[1] = d[1] * 1e3f;
    }
}

// Where the head looks, as a point on the game's screen, in pixels of the viewport with y
// up (the convention of CPlayer's screen tests), kept far enough from the edges for the
// window to fit. False when the scan is the game's own.
//
// Two mappings, because the eyes draw the game's two kinds of draw differently:
//  - `on_hud_frame` false: the eyes' mean forward direction in the camera's frame, through
//    the first-person camera's projection. Where a world object the head looks at lands on
//    the game's screen, which is what the scan zone tests and what the window's copy should
//    be taken around.
//  - `on_hud_frame` true: the same direction, through the frame the eyes paint the game's
//    2D elements on (gx::render_hud_frame: `hud_scale` of the headset's field of view tall,
//    `hud_distance_m` ahead, lifted and tilted by `hud_height_m` and `hud_pitch_deg`). The
//    window is an orthographic draw and lands on that frame, whose angular size is the
//    headset's and not the game camera's 55 degrees, so a window placed through the camera
//    sat off the gaze by the ratio of the two -- above it on a Quest 3, whose field is the
//    larger. Through the frame, the window is centred where the eyes look by construction.
//    The frame's height takes the larger of the eyes' up and down tangents, as the
//    frontend does.
bool head_screen_point(float out[2], bool on_hud_frame = false) {
    ensure_config();
    if (!g_scan_on || vr::gamepad_play()) return false;
    vr::EyeView ev[2];
    if (!camera_eyes(ev)) return false;
    const uint32_t pair = guest_camera_pair();
    if (!pair) return false;
    const uint32_t camera = mem_r32(pair);
    if (!is_camera(camera, kFirstPersonCameraVtable)) return false;
    float d[3];
    gaze(ev, d);
    const float W = (float)(int32_t)mem_r32(kViewport + 8), H = (float)(int32_t)mem_r32(kViewport + 12);
    if (!(W > 0.0f) || !(H > 0.0f)) return false;
    // Normalised device coordinates; a head turned past the edge of the view pins to it.
    float nx, ny;
    if (on_hud_frame) {
        float tan_half = 0.0f;
        for (int e = 0; e < 2; e++) tan_half = fmaxf(tan_half, fmaxf(fabsf(ev[e].tan_up), fabsf(ev[e].tan_down)));
        const HudFrame& f = g_hud_frame;
        const float half_h = f.dist * tan_half * f.scale, half_w = half_h * 4.0f / 3.0f;
        if (!(half_h > 0.0f)) return false;
        // The frame: centre C, its up leaned back by the pitch, its normal with it. The
        // gaze ray from the camera meets its plane at t.
        const float c = cosf(f.pitch_rad), sn = sinf(f.pitch_rad);
        const float C[3] = {0.0f, f.height, -f.dist}, up[3] = {0.0f, c, -sn}, n[3] = {0.0f, sn, c};
        const float nd = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
        if (nd > -1e-4f) {
            nx = d[0] * 1e3f;
            ny = d[1] * 1e3f;
        } else {
            const float tt = (n[0] * C[0] + n[1] * C[1] + n[2] * C[2]) / nd;
            const float P[3] = {tt * d[0] - C[0], tt * d[1] - C[1], tt * d[2] - C[2]};
            nx = P[0] / half_w;
            ny = (P[0] * up[0] + P[1] * up[1] + P[2] * up[2]) / half_h;
        }
    } else {
        float k[2], n[2];
        if (!camera_tangents(camera, k)) return false;
        project(d, k, n);
        nx = n[0];
        ny = n[1];
    }
    float lim[2];
    window_reach(lim);
    nx = fmaxf(-lim[0], fminf(lim[0], nx));
    ny = fmaxf(-lim[1], fminf(lim[1], ny));
    out[0] = 0.5f * W * (1.0f + nx);
    out[1] = 0.5f * H * (1.0f + ny);
    return true;
}

// The patch at CPlayer::UpdateOrbitZone: the scan zone's centre, or the game's own.
float g_last_zone[2];  // the zone's point as last placed, for scan_ortho's log
void scan_frame(uint32_t player) {
    static const bool log = getenv("GCN_SCANLOG") != nullptr;
    static bool have_original = false, moved = false;
    static uint32_t original[4];
    const uint32_t tweak = mem_r32(kTweakPlayer);
    if (!in_ram(tweak)) return;
    if (!have_original) {
        for (int i = 0; i < 4; i++) original[i] = mem_r32(tweak + kScanZoneFields[i]);
        have_original = true;
        if (log)
            fprintf(stderr, "[prime] scan zone: the game's centre %u %u, ideal %u %u, half extents %u %u\n",
                    original[0], original[1], original[2], original[3], mem_r32(tweak + 0x1AC), mem_r32(tweak + 0x1B4));
    }
    if (log) {
        // Every 50 frames: where the camera looks and the orbit state, for checking that
        // locking on does not turn it.
        static uint32_t calls = 0;
        const uint32_t pair = g_guest_pair;
        if (calls++ % 50 == 0 && pair && is_camera(mem_r32(pair), kFirstPersonCameraVtable)) {
            float cam[12];
            read_xf(mem_r32(pair) + kActorXf, cam);
            fprintf(stderr, "[prime] scan frame %u: camera forward %.4f %.4f %.4f, orbit state %u\n", calls - 1, cam[1],
                    cam[5], cam[9], mem_r32(player + kPlayerOrbitState));
        }
    }
    float p[2];
    if (head_screen_point(p)) {
        g_last_zone[0] = p[0];
        g_last_zone[1] = p[1];
        const uint32_t v[4] = {(uint32_t)p[0], (uint32_t)p[1], (uint32_t)p[0], (uint32_t)p[1]};
        for (int i = 0; i < 4; i++) mem_w32(tweak + kScanZoneFields[i], v[i]);
        if (log) {
            static uint32_t last_x = ~0u, last_y = ~0u;
            if (v[0] != last_x || v[1] != last_y) {
                last_x = v[0];
                last_y = v[1];
                fprintf(stderr, "[prime] scan zone at %u %u\n", v[0], v[1]);
            }
        }
        moved = true;
    } else if (moved) {
        for (int i = 0; i < 4; i++) mem_w32(tweak + kScanZoneFields[i], original[i]);
        moved = false;
        if (log) fprintf(stderr, "[prime] scan zone: the game's again\n");
    }
}

// In place of DrawScanEffect's call of GXSetTexCopySrc(x, y, width, height): the copy
// centred on the head's point, inside the viewport. The EFB's y runs down.
void scan_copy_src(CPU* c) {
    float p[2];
    if (head_screen_point(p)) {
        const int32_t left = (int32_t)mem_r32(kViewport), top = (int32_t)mem_r32(kViewport + 4);
        const int32_t W = (int32_t)mem_r32(kViewport + 8), H = (int32_t)mem_r32(kViewport + 12);
        const int32_t w = (int32_t)(c->r[5] & 0xFFFF), h = (int32_t)(c->r[6] & 0xFFFF);
        int32_t x = (int32_t)(left + p[0] - 0.5f * w), y = (int32_t)(top + (H - p[1]) - 0.5f * h);
        x = x < left ? left : x > left + W - w ? left + W - w : x;
        y = y < top ? top : y > top + H - h ? top + H - h : y;
        c->r[3] = (uint32_t)(x & ~1) & 0xFFFF;  // the copy's origin is taken in pairs of pixels
        c->r[4] = (uint32_t)(y & ~1) & 0xFFFF;
    }
    c->lr = 0x8011399Cu;
    fn_80379B4C(c);
}

// Right after DrawScanEffect's SetViewportOrtho(true, -1, 1): the same ortho again, shifted
// so that what the game draws about the origin lands on the head's point. SetViewportOrtho
// centred the viewport's pixel bounds and passed them to SetOrtho as (left, right, bottom,
// top), y up; the generated code's volatile registers are free here, between a call's
// return and the next instruction's use of a saved one.
void scan_ortho(CPU* c) {
    float p[2];
    if (!head_screen_point(p, true)) return;
    // GCN_SCANLOG=1: the window's point beside the zone's, with the eyes' field and the
    // frame they were mapped through, whenever the window moves by more than a few pixels.
    // Read from a headset session (gcn_env.txt in the app's files directory turns it on)
    // this says what the headset is actually giving the placement to work with.
    static const bool log = getenv("GCN_SCANLOG") != nullptr;
    if (log) {
        static float last[2] = {-1e9f, -1e9f};
        if (fabsf(p[0] - last[0]) > 4.0f || fabsf(p[1] - last[1]) > 4.0f) {
            last[0] = p[0]; last[1] = p[1];
            // What the hooks already read: the zone's point as scan_frame last placed it,
            // the eyes as they were last given. Reading either again would be a read of
            // the eyes (camera_eyes).
            const float z[2] = {g_last_zone[0], g_last_zone[1]};
            const vr::EyeView* ev = g_last_eyes;
            float tan_half = 0.0f;
            for (int e = 0; e < 2; e++) tan_half = fmaxf(tan_half, fmaxf(fabsf(ev[e].tan_up), fabsf(ev[e].tan_down)));
            fprintf(stderr, "[prime] scan window at %.0f %.0f (zone %.0f %.0f; frame %.2f x %.2f tall at %.2f, height %.2f, pitch %.1f; eyes tan up %.3f down %.3f)\n",
                    p[0], p[1], z[0], z[1], g_hud_frame.scale, tan_half, g_hud_frame.dist, g_hud_frame.height,
                    g_hud_frame.pitch_rad * 180.0f / 3.14159265f, ev[0].tan_up, ev[0].tan_down);
        }
    }
    const int32_t left = (int32_t)mem_r32(kViewport), top = (int32_t)mem_r32(kViewport + 4);
    const int32_t W = (int32_t)mem_r32(kViewport + 8), H = (int32_t)mem_r32(kViewport + 12);
    const float ox = p[0] - 0.5f * W, oy = p[1] - 0.5f * H;
    const float args[6] = {(float)(left - W / 2) - ox, (float)(left + W / 2) - ox, (float)(top + H / 2) - oy,
                           (float)(top - H / 2) - oy, -1.0f, 1.0f};
    for (int i = 0; i < 6; i++) {
        c->f[1 + i].d = args[i];
        c->ps1[1 + i] = args[i];
    }
    c->lr = 0x801139F0u;
    fn_8030CFA4(c);
}

// GCN_ORBITLOG: every damageable trigger's lock-on state (CScriptDamageableTrigger, vtable
// 0x803E0DC8), logged when it changes: the active bit (+0x30, 0x80), its own flags (+0x310:
// 0x08 can orbit, 0x40 dead), its health (+0x17C) and whether it has the Orbit material
// (bit 41, 0x200 in the word at +0x68), with its distance and elevation from the camera.
void damageable_log(uint32_t mgr, const float* cam) {
    const uint32_t list = mem_r32(mgr + 0x810);
    if (!in_ram(list)) return;
    static uint32_t last[1024];
    for (uint32_t slot = 0; slot < 1024; slot++) {
        const uint32_t obj = mem_r32(list + 4 + 8 * slot);
        if (!in_ram(obj) || (obj & 3) || mem_r32(obj) != 0x803E0DC8u) {
            last[slot] = 0;
            continue;
        }
        const uint32_t s = 0x80000000u | (uint32_t)mem_r8(obj + 0x30) << 16 | (uint32_t)mem_r8(obj + 0x310) << 8 |
                           ((mem_r32(obj + 0x68) >> 9) & 1) | (rd_f(obj + 0x17C) > 0.0f ? 2 : 0);
        if (s == last[slot]) continue;
        last[slot] = s;
        float d[3];
        for (int j = 0; j < 3; j++) d[j] = rd_f(obj + kActorXf + 16 * j + 12) - cam[4 * j + 3];
        const float h = sqrtf(d[0] * d[0] + d[1] * d[1]);
        fprintf(stderr, "[prime] frame %u: damageable %04X active %u flags %02X hp %.1f orbit %u, %.1f m, %.1f up\n",
                gx_frames_submitted(), mem_r16(obj + 8), mem_r8(obj + 0x30) >> 7, mem_r8(obj + 0x310), rd_f(obj + 0x17C),
                s & 1, sqrtf(h * h + d[2] * d[2]), atan2f(d[2], h) * 57.2958f);
    }
}

// GCN_ORBITLOG=1: what the lock-on has to choose from, once a frame from UpdateOrbitZone (r3
// the player, r4 the state manager). CPlayer keeps three lists of TUniqueIds, each an
// rstl::vector (size at +4, items at +0xC): the orbitable objects near enough
// (+0x354), those of them inside the visor's zone (+0x364) and outside it (+0x374); the
// lists are last frame's. For each near one: its vtable (symbols.txt names the class),
// where its origin lands on the game's screen against the zone's box (CTweakPlayer: centre
// at +0x1B8, half extents at +0x1A8, four bytes a zone, the combat visor's zone 0), and its
// elevation from the camera. An object is looked up in the state manager's list of all
// objects (+0x810: entries of eight bytes from +4, the entity first, by the id's low ten
// bits). Logged whenever the lists or the lock change, and every 30 frames while any is near.
void orbit_log(uint32_t player, uint32_t mgr) {
    static const bool log = getenv("GCN_ORBITLOG") != nullptr;
    if (!log || !in_ram(player) || !in_ram(mgr)) return;
    const uint32_t tweak = mem_r32(kTweakPlayer), list = mem_r32(mgr + 0x810);
    const uint32_t pair = g_guest_pair;
    if (!in_ram(tweak) || !in_ram(list) || !pair) return;
    const uint32_t camera = mem_r32(pair);
    float k[2];
    if (!is_camera(camera, kFirstPersonCameraVtable) || !camera_tangents(camera, k)) return;
    const uint32_t zone = mem_r32(player + kPlayerOrbitZoneMode), state = mem_r32(player + kPlayerOrbitState);
    const uint32_t n[3] = {mem_r32(player + 0x358), mem_r32(player + 0x368), mem_r32(player + 0x378)};
    const uint16_t target = mem_r16(player + 0x320);
    static uint32_t last_n[3] = {~0u, ~0u, ~0u}, last_state = ~0u, frames = 0;
    static uint16_t last_target = 0;
    const bool changed = memcmp(n, last_n, sizeof(n)) || state != last_state || target != last_target;
    memcpy(last_n, n, sizeof(n));
    last_state = state;
    last_target = target;
    {
        float c0[12];
        read_xf(camera + kActorXf, c0);
        damageable_log(mgr, c0);
        // The camera manager's fluid counter (+0x74): non-zero while the camera is in water.
        static uint32_t last_fluid = ~0u;
        const uint32_t cm = mem_r32(mgr + 0x870);
        const uint32_t fl = in_ram(cm) ? mem_r32(cm + 0x74) : 0;
        if (fl != last_fluid) {
            last_fluid = fl;
            fprintf(stderr, "[prime] frame %u: camera in fluid %u\n", gx_frames_submitted(), fl);
        }
    }
    if (!changed && !(n[0] && ++frames % 30 == 0)) return;
    if (n[0] > 64) return;
    const float W = (float)(int32_t)mem_r32(kViewport + 8), H = (float)(int32_t)mem_r32(kViewport + 12);
    const int32_t cx = (int32_t)mem_r32(tweak + 0x1B8 + 4 * zone), cy = (int32_t)mem_r32(tweak + 0x1C0 + 4 * zone);
    const int32_t hx = (int32_t)mem_r32(tweak + 0x1A8 + 4 * zone), hy = (int32_t)mem_r32(tweak + 0x1B0 + 4 * zone);
    fprintf(stderr, "[prime] frame %u: orbit state %u target %04X, zone %u box %d %d +- %d %d, %u near, %u in, %u out; turn %.1f left %.1f up\n",
            gx_frames_submitted(), state, target, zone, cx, cy, hx, hy, n[0], n[1], n[2], g_turn.yaw * 57.2958f,
            g_turn.pitch * 57.2958f);
    float cam[12];
    read_xf(camera + kActorXf, cam);
    const uint32_t items = mem_r32(player + 0x360);
    for (uint32_t i = 0; i < n[0] && in_ram(items); i++) {
        const uint16_t id = mem_r16(items + 2 * i);
        const uint32_t obj = mem_r32(list + 4 + 8 * (id & 0x3FF));
        if (!in_ram(obj) || mem_r16(obj + 8) != id) continue;
        float d[3], v[3];
        for (int j = 0; j < 3; j++) d[j] = rd_f(obj + kActorXf + 16 * j + 12) - cam[4 * j + 3];
        for (int j = 0; j < 3; j++) v[j] = d[0] * cam[j] + d[1] * cam[4 + j] + d[2] * cam[8 + j];  // right, fwd, up
        const float sx = v[1] > 1e-3f ? 0.5f * W * (1.0f + v[0] / v[1] / k[0]) : -1.0f;
        const float sy = v[1] > 1e-3f ? 0.5f * H * (1.0f + v[2] / v[1] / k[1]) : -1.0f;
        const float elev = atan2f(d[2], sqrtf(d[0] * d[0] + d[1] * d[1])) * 57.2958f;
        fprintf(stderr, "[prime]   %04X vt %08X at %.0f %.0f on screen, %.1f m, %.1f up\n", id, mem_r32(obj), sx, sy,
                sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), elev);
    }
    // Locking on (state 2) with nothing near: every object of the state manager's list with
    // a transform within 60 m and more than 15 degrees above the camera, whatever it is,
    // with its material bits (+0x68, as ValidateObjectForMode reads them) and the flag bytes
    // ValidateOrbitTargetId tests (+0x30, +0xF2, +0xF3), and the tweak's angle limits above
    // and below the horizon (+0x188, +0x18C), which that test holds the eye's line to it to.
    if (state == 2 && n[0] == 0 && changed) {
        fprintf(stderr, "[prime]   nothing near; orbit angle limits %.1f up %.1f down\n", rd_f(tweak + 0x188), rd_f(tweak + 0x18C));
        for (uint32_t slot = 0; slot < 1024; slot++) {
            const uint32_t obj = mem_r32(list + 4 + 8 * slot);
            if (!in_ram(obj) || (obj & 3)) continue;
            const uint32_t vt = mem_r32(obj);
            if (vt < 0x803D0000u || vt >= 0x80400000u) continue;
            float d[3];
            bool ok = true;
            for (int j = 0; j < 3; j++) {
                d[j] = rd_f(obj + kActorXf + 16 * j + 12) - cam[4 * j + 3];
                ok = ok && fabsf(d[j]) < 60.0f;
            }
            if (!ok) continue;
            const float h = sqrtf(d[0] * d[0] + d[1] * d[1]), elev = atan2f(d[2], h) * 57.2958f;
            if (elev < 15.0f) continue;
            fprintf(stderr, "[prime]   above: %04X vt %08X %.1f m, %.1f up, mat %08X %08X, flags %02X %02X %02X\n", mem_r16(obj + 8), vt,
                    sqrtf(h * h + d[2] * d[2]), elev, mem_r32(obj + 0x68), mem_r32(obj + 0x6C), mem_r8(obj + 0x30),
                    mem_r8(obj + 0xF2), mem_r8(obj + 0xF3));
        }
    }
}

// The reads of CPlayer::mOrbitState in UpdateOrbitOrientation and the first-person camera:
// no orbit, while a scan that follows the head is up.
uint32_t orbit_state_for_turning(uint32_t player) {
    const uint32_t s = mem_r32(player + kPlayerOrbitState);
    if (s >= 1 && s <= 4 && mem_r32(player + kPlayerOrbitZoneMode) == 1) {
        float p[2];
        if (head_screen_point(p)) return 0;
    }
    return s;
}

// ---------------------------------------------------------------------------
// The camera follows the head.
//
// The game's camera is the body's: the stick turns it, the free look tilts it, and the head
// looks around from it. What the game does with what is on its screen -- locking on (its
// orbit zones are boxes of screen pixels), scanning, the HUD's markers -- stops at that
// screen's edges, though the viewer can see and shoot well past them: an enemy overhead
// could be hit but not locked on to. So the camera follows the head, past the region the
// scan window can follow it in on the game's screen (window_reach, through the first-person
// camera's own projection: about 28 degrees either side and 19 up or down).
//
// Where the camera is headed is a function of the gaze alone: straight ahead while the gaze
// is within camera_follow_reach (a fraction, 0.32: 10 degrees either side, 6 up or down) of
// that region, and past it, turned just far enough to bring the gaze back to that fraction
// of it -- so looking back pulls the camera home again. (0.8 at first; worn, it was asked to
// start following a fifth sooner, 0.64, and then at half that.) It gets there on a critically damped spring, quicker following the gaze
// out (camera_follow_rate, 8 a second: a steady head turn is followed a quarter of a second
// behind) than going back (camera_return_rate, 4), so that it has some weight and eases in
// and out of its moves. The first version kept the gaze exactly on the region's edge,
// moving with the head and staying where it was left; worn, that read as weightless and as
// stuck, and the spring's lag is what the fraction leaves room for. Yaw and pitch are worked out
// separately, the yaw's region widened by the gaze's pitch (a yaw turns the view less far
// the nearer it looks to straight up).
//
// The turn goes onto the camera's transform once CFirstPersonCamera::Think has stored it (the
// patch at 0x8000E570, recomp/patches.txt), stepped by Think's own time step. UpdateTransform
// builds that transform afresh every frame from the player and its own gun-follow transform
// without reading the last one back, so the turn does not accumulate. The frame drawn from it
// carries the turn (gx::set_view_turn) and the eyes undo it: the world stays where the viewer
// saw it, while what the game places about its camera -- the HUD frame, the scan window, the
// cannon -- turns with the camera. The poses the other hooks read are taken into the turned
// camera's frame before use (unturn_pose).
//
// The turn is said as the world's view is set up for drawing (the cull patch in
// CStateManager::SetupViewForDraw), and none for a view from another camera. gcn-recomp
// pins it to that point of the command stream; the first version had it applied on the
// guest thread to whichever batch the front end, up to a frame behind, was building, and
// in the headset the world, and only the world, jumped by a frame's turn every so often
// while the camera moved.
//
// Locking on points the camera at the target, as the game always has, outside the scan
// visor (which keeps both the body and the camera off the lock: "The scan visor follows the
// head"); the turn goes on top. For a while the camera alone was kept off the lock in every
// visor, so that it would not pull the world round to the target. But the body's lock-on
// steering (CPlayer::UpdateOrbitOrientation) and the camera's go together: with only the
// body's, holding L in the combat visor turned the body at full speed for as long as it was
// held (frames 6717-7139 of the session 20261015-123810), turn or no turn. Where the player
// steers by the camera's forward, it is given the forward with the turn taken off
// (unturned_forward, below).
//
// Only while the eyes are published -- stereo in the headset, GCN_CULL_HEAD on the desktop;
// without them the turn is none, which the panel needs (the morph between it and the eyes
// would turn it too) and costs nothing in the eyes, which undo whatever turn a frame
// carries. camera_follows_head 0 in vr.txt turns it off. GCN_TURNLOG=1 prints the turn as it
// changes.

float wrap_angle(float a) {
    while (a > 3.14159265f) a -= 6.2831853f;
    while (a < -3.14159265f) a += 6.2831853f;
    return a;
}

// How far past `half` either way `a` is: what the camera has to turn by to bring it back.
float past(float a, float half) { return a > half ? a - half : a < -half ? a + half : 0.0f; }

// The spring's rate for an axis of the turn at `x` headed for `to`: the quicker one while the
// target is further from the centre than the turn.
float spring_rate(float x, float to) { return fabsf(to) > fabsf(x) ? g_follow.out_rate : g_follow.back_rate; }

// One axis of the turn, `x` with its rate `v`, `dt` seconds towards `to` on a critically
// damped spring of rate `w`.
void spring(float& x, float& v, float to, float w, float dt) {
    const int n = (int)ceilf(dt / 0.004f);
    const float h = n > 0 ? dt / (float)n : 0.0f;
    for (int i = 0; i < n; i++) {
        v += h * (w * w * (to - x) - 2.0f * w * v);
        x += h * v;
    }
}

bool dual_sticks(float v[3]);

// The patch in CFirstPersonCamera::Think, `dt` its time step: the turn, onto the transform
// just stored.
void turn_camera(uint32_t camera, float dt) {
    ensure_config();
    if (!is_camera(camera, kFirstPersonCameraVtable)) return;
    vr::EyeView ev[2];
    float k[2], lim[2];
    const bool eyes = vr::eye_views(ev) || stand_in_eyes(ev);
    static const bool tlog = getenv("GCN_TURNLOG") != nullptr;
    if (tlog) {
        // The game's own camera, before the turn: its heading and pitch, when either moves by
        // more than 3 degrees in a frame.
        float cam[12];
        read_xf(camera + kActorXf, cam);
        const float h = atan2f(cam[1], cam[5]) * 57.2958f, p = asinf(fmaxf(-1.0f, fminf(1.0f, cam[9]))) * 57.2958f;
        static float last_h = 0.0f, last_p = 0.0f;
        const float dh = fabsf(wrap_angle((h - last_h) / 57.2958f)) * 57.2958f;
        if (dh > 3.0f || fabsf(p - last_p) > 3.0f)
            fprintf(stderr, "[prime] frame %u: the game's camera heading %.1f pitch %.1f (from %.1f %.1f)\n", gx_frames_submitted(), h,
                    p, last_h, last_p);
        last_h = h;
        last_p = p;
        static uint32_t calls = 0;
        // Every 15 frames: the gaze, the game's own heading and the turn.
        if (calls++ % 15 == 0) {
            float g[3] = {0, 0, -1};
            if (eyes) gaze(ev, g);
            fprintf(stderr, "[prime] frame %u: gaze %.1f left %.1f up, the game's heading %.1f, turn %.1f left %.1f up\n",
                    gx_frames_submitted(), atan2f(-g[0], -g[2]) * 57.2958f, asinf(fmaxf(-1.0f, fminf(1.0f, g[1]))) * 57.2958f,
                    h, g_turn.yaw * 57.2958f, g_turn.pitch * 57.2958f);
        }
    }
    // With a gamepad the camera is the game's: the head only looks around it. With the
    // dual-stick controls the right stick tilts it (see map_gamepad), level while locked on
    // or in the ball.
    float sticks[3];
    if (dual_sticks(sticks)) {
        g_turn.yaw = g_turn.yaw_rate = g_turn.pitch_rate = 0.0f;
        const bool level = g_player && (mem_r32(g_player + kPlayerOrbitState) != 0 || mem_r32(g_player + kPlayerMorphState) != 0);
        const float step = fmaxf(0.0f, fminf(0.1f, dt));
        if (level) g_turn.pitch *= expf(-10.0f * step);
        else g_turn.pitch = fmaxf(-1.2f, fminf(1.2f, g_turn.pitch + sticks[2] * g_pad_pitch_rate * step));
    } else if (!g_follow_on || !eyes || vr::gamepad_play()) {
        g_turn.yaw = g_turn.pitch = g_turn.yaw_rate = g_turn.pitch_rate = 0.0f;
    } else if (camera_tangents(camera, k) && window_reach(lim)) {
        float g[3];
        gaze(ev, g);
        // The gaze's yaw (left) and pitch (up), and the region about the centre, as angles.
        const float gp = asinf(fmaxf(-1.0f, fminf(1.0f, g[1]))), gy = atan2f(-g[0], -g[2]);
        const float half_p = atanf(g_follow.reach * lim[1] * k[1]);
        const float half_y = fminf(3.0f, atanf(g_follow.reach * lim[0] * k[0]) / fmaxf(cosf(gp), 0.25f));
        const float to_pitch = fmaxf(-1.45f, fminf(1.45f, past(gp, half_p)));
        const float to_yaw = past(gy, half_y);
        const float step = fmaxf(0.0f, fminf(0.1f, dt));
        // The yaw measured from where it is, so that it goes the short way round.
        float yaw_off = 0.0f;
        spring(yaw_off, g_turn.yaw_rate, wrap_angle(to_yaw - g_turn.yaw), spring_rate(g_turn.yaw, to_yaw), step);
        g_turn.yaw = wrap_angle(g_turn.yaw + yaw_off);
        spring(g_turn.pitch, g_turn.pitch_rate, to_pitch, spring_rate(g_turn.pitch, to_pitch), step);
    }
    // Yaw about y, then pitch about the turned x.
    const float cy = cosf(0.5f * g_turn.yaw), sy = sinf(0.5f * g_turn.yaw);
    const float cp = cosf(0.5f * g_turn.pitch), sp = sinf(0.5f * g_turn.pitch);
    g_turn.q[0] = cy * sp;
    g_turn.q[1] = sy * cp;
    g_turn.q[2] = -sy * sp;
    g_turn.q[3] = cy * cp;

    static const bool log = getenv("GCN_TURNLOG") != nullptr;
    if (log) {
        static float last[2] = {0.0f, 0.0f};
        if (fabsf(g_turn.yaw - last[0]) + fabsf(g_turn.pitch - last[1]) > 0.5f * 3.14159265f / 180.0f) {
            last[0] = g_turn.yaw;
            last[1] = g_turn.pitch;
            fprintf(stderr, "[prime] frame %u: camera turned %.1f left, %.1f up after the head\n", gx_frames_submitted(),
                    g_turn.yaw * 57.2958f, g_turn.pitch * 57.2958f);
        }
    }
    if (g_turn.yaw == 0.0f && g_turn.pitch == 0.0f) return;

    // The camera's right, forward and up, turned in its own frame, then into the world.
    float cam[12];
    read_xf(camera + kActorXf, cam);
    const float axes[3][3] = {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}};  // in the eyes' frame
    float turned[3][3];
    for (int j = 0; j < 3; j++) {
        float t[3], c[3];
        qrot(g_turn.q, axes[j], t);
        eye_to_camera(t, c);
        rotate(cam, c, turned[j]);
    }
    for (int j = 0; j < 3; j++)
        for (int i = 0; i < 3; i++) wr_f(camera + kActorXf + 16 * i + 4 * j, turned[j][i]);
}

// In place of the game's loads of the first-person camera's forward, where its player steers
// by it: `camera`'s forward with the follow turn taken off, x, y and z into `fx`, `fy`, `fz`.
// - CPlayer::UpdateOrbitOrientation: until a lock is established the body is turned to face
//   that forward, and the camera is the body's plus the turn. Given the turned one, the body
//   turned by the whole turn every frame, round and round while L was held -- 20 degrees a
//   frame at the end of the session 20261015-114835, circling a target with the camera
//   turned that far.
// - CPlayer::UpdateFreeLookState: tests the camera's level forward against the body's, to
//   end the free look; with the turn in it, that test would not pass while the head was
//   turned past the region.
void unturned_forward(CPU* c, uint32_t camera, int fx, int fy, int fz) {
    float cam[12], back[3], local[3], fwd[3];
    read_xf(camera + kActorXf, cam);
    const float ahead[3] = {0.0f, 0.0f, -1.0f};
    qrot(g_turn.q, ahead, back, true);
    eye_to_camera(back, local);
    rotate(cam, local, fwd);
    const int to[3] = {fx, fy, fz};
    for (int i = 0; i < 3; i++) {
        c->f[to[i]].d = fwd[i];
        c->ps1[to[i]] = fwd[i];
    }
}

// As the world's view is set up for drawing: the turn of the camera it is drawn from, none
// unless that is the first-person camera.
void publish_view_turn() {
    static const float none[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const uint32_t pair = guest_camera_pair();
    bool first_person = false;
    if (pair && is_camera(mem_r32(pair), kFirstPersonCameraVtable)) {
        const uint32_t mgr = pair - kCameraPairOffset;
        first_person = mem_r16(mgr) == mem_r16(mem_r32(pair) + 8) && mem_r32(mgr + kCinematicCount) == 0;
    }
    gx::set_view_turn(first_person ? g_turn.q : none);
}

// ---------------------------------------------------------------------------
// The Touch controllers, after the frontend's own mapping (vr::GameHooks::map_pad).
//
// The right trigger fires (A), as the cannon in that hand suggests, and the right grip is
// R, the free look (the frontend hands the grip over as Z). The right hand's A jumps (B)
// and its B fires missiles (Y); the left hand's X morphs (X) and its Y opens the map (Z).
// A and B are themselves wherever the game is not drawing its world -- the main menu, the
// pause and map screens -- which select with A and go back with B; and on the pause and
// map screens the trigger is R
// rather than A, for the pause screen's tabs and the map's zoom. Those screens are where
// the game is in play but not drawing its world (prime_world_drawn), which the view
// decision reads too.
void map_pad(PadState& p) {
    const bool a = p.buttons & PAD_A, b = p.buttons & PAD_B, y = p.buttons & PAD_Y, grip = p.buttons & PAD_Z;
    const bool trig = (p.buttons & PAD_R) || p.trig_r > 127;
    const bool in_play = g_camera_pair.load(std::memory_order_relaxed) != 0;
    const bool world = g_world_drawn.load(std::memory_order_relaxed) != 0;
    p.buttons &= (uint16_t)~(PAD_A | PAD_B | PAD_Y | PAD_Z | PAD_R);
    p.trig_r = 0;
    if (y) p.buttons |= PAD_Z;
    if (grip || (trig && in_play && !world)) { p.buttons |= PAD_R; p.trig_r = 255; }
    if (trig && !(in_play && !world)) p.buttons |= PAD_A;
    if (a) p.buttons |= (in_play && world) ? PAD_B : PAD_A;
    if (b) p.buttons |= (in_play && world) ? PAD_Y : PAD_B;
}

// ---------------------------------------------------------------------------
// A gamepad (vr::GameHooks::map_gamepad).
//
// By default Metroid Prime Remastered's dual-stick controls, which a modern pad's player
// expects: the left stick moves and strafes, the right stick turns and looks up and down,
// the right trigger fires, the left trigger locks on. gamepad_classic 1 in vr.txt gives the
// GameCube's instead, each button in its place on the pad (default_gamepad_layout) but
// with south jumping and west firing: by place the GameCube's A is south, and a modern
// pad's player reaches for south to jump.
//
// The dual-stick controls, by place (a Nintendo pad's names in brackets):
// - the right trigger (ZR) and east (A) fire, a bomb in the ball (the GameCube's A);
// - south (B) jumps, and boosts the ball (B);
// - west (Y) morphs and unmorphs (X);
// - the right shoulder (R) fires missiles, a power bomb in the ball (Y);
// - the left trigger (ZL) locks on, scans, grapples (L, analog);
// - the left shoulder (L) holds the spider ball (R);
// - the d-pad picks a beam, as the GameCube's C stick did, and a visor with north (X)
//   held, as its d-pad did;
// - start pauses (start), select opens the map (Z).
// Off the world (the menus, the pause and map screens) the sticks are the main and C
// sticks, east selects (A), south goes back (B), and the triggers and shoulders are L and R
// for the pause screen's tabs and the map's zoom.
//
// Moving, turning and looking are not the GameCube's: its stick turns the body and only
// strafes while locked on, and its free look springs back when let go. The pad's main
// stick is the left stick, which moves and steers the ball as the GameCube's does; what
// the game's movement does with it differs in three places, each a hook on the guest
// thread that reads the right stick and the left stick's x through input_pose
// (dual_sticks), so a replay has them:
// - the turn (CPlayer::TurnInput) is the right stick's x, not the main stick's;
// - outside a lock, the left stick's x pushes the body sideways (prime_strafe_force), with
//   the force the game works out for going forward (CPlayer::ComputeMovement), and
//   the turn no longer adds to the forward speed (CPlayer::ForwardInput);
// - the right stick's y tilts the camera up and down and leaves it there (turn_camera's
//   pitch, gamepad_look_deg_per_s at full tilt, 150), levelling again while locked on,
//   where the game aims, and in the ball.
// Only with a gamepad in hand: tracked controllers keep their own controls.

std::atomic<float> g_pad_strafe{0.0f}, g_pad_turn{0.0f}, g_pad_look{0.0f};

float dead_zone(float v, float dz) { return fabsf(v) < dz ? 0.0f : (v - copysignf(dz, v)) / (1.0f - dz); }

void map_gamepad(const vr::Gamepad& g, PadState& p) {
    using G = vr::Gamepad;
    if (g_pad_classic) {
        vr::Gamepad c = g;
        c.buttons &= ~(G::South | G::West);
        if (g.buttons & G::South) c.buttons |= G::West;
        if (g.buttons & G::West) c.buttons |= G::South;
        vr::default_gamepad_layout(c, p);
        g_pad_strafe = g_pad_turn = g_pad_look = 0.0f;
        return;
    }
    const bool in_play = g_camera_pair.load(std::memory_order_relaxed) != 0;
    const bool world = in_play && g_world_drawn.load(std::memory_order_relaxed) != 0;
    auto held = [&](uint32_t b) { return (g.buttons & b) != 0; };
    auto to_u8 = [](float f) { return (uint8_t)std::max(0, std::min(255, 128 + (int)(f * 100.0f))); };
    const float lx = dead_zone(g.lx, 0.12f), ly = dead_zone(g.ly, 0.12f);
    const float rx = dead_zone(g.rx, 0.1f), ry = dead_zone(g.ry, 0.1f);
    if (lx != 0.0f || ly != 0.0f) { p.stick_x = to_u8(lx); p.stick_y = to_u8(ly); }
    if (held(G::Start)) p.buttons |= PAD_START;
    if (held(G::Select)) p.buttons |= PAD_Z;
    if (held(G::East)) p.buttons |= PAD_A;
    if (held(G::South)) p.buttons |= PAD_B;
    if (!world) {
        if (held(G::West)) p.buttons |= PAD_X;
        if (held(G::North)) p.buttons |= PAD_Y;
        if (rx != 0.0f || ry != 0.0f) { p.cstick_x = to_u8(rx); p.cstick_y = to_u8(ry); }
        if (held(G::Up)) p.buttons |= PAD_UP;
        if (held(G::Down)) p.buttons |= PAD_DOWN;
        if (held(G::Left)) p.buttons |= PAD_LEFT;
        if (held(G::Right)) p.buttons |= PAD_RIGHT;
        const float l = std::max(g.lt, held(G::LeftShoulder) ? 1.0f : 0.0f);
        const float r = std::max(g.rt, held(G::RightShoulder) ? 1.0f : 0.0f);
        if (l > 0.03f) { p.trig_l = std::max(p.trig_l, (uint8_t)(l * 255.0f)); if (l > 0.9f) p.buttons |= PAD_L; }
        if (r > 0.03f) { p.trig_r = std::max(p.trig_r, (uint8_t)(r * 255.0f)); if (r > 0.9f) p.buttons |= PAD_R; }
        g_pad_strafe = g_pad_turn = g_pad_look = 0.0f;
        return;
    }
    if (g.rt > 0.4f) p.buttons |= PAD_A;
    if (held(G::West)) p.buttons |= PAD_X;
    if (held(G::RightShoulder)) p.buttons |= PAD_Y;
    if (held(G::LeftShoulder)) { p.buttons |= PAD_R; p.trig_r = 255; }
    if (g.lt > 0.03f) { p.trig_l = std::max(p.trig_l, (uint8_t)(g.lt * 255.0f)); if (g.lt > 0.5f) p.buttons |= PAD_L; }
    // The d-pad: a visor with north held, otherwise a beam, the C stick pushed its way.
    if (held(G::North)) {
        if (held(G::Up)) p.buttons |= PAD_UP;
        if (held(G::Down)) p.buttons |= PAD_DOWN;
        if (held(G::Left)) p.buttons |= PAD_LEFT;
        if (held(G::Right)) p.buttons |= PAD_RIGHT;
    } else {
        const float bx = held(G::Right) ? 1.0f : held(G::Left) ? -1.0f : 0.0f;
        const float by = held(G::Up) ? 1.0f : held(G::Down) ? -1.0f : 0.0f;
        if (bx != 0.0f || by != 0.0f) { p.cstick_x = to_u8(bx); p.cstick_y = to_u8(by); }
    }
    g_pad_strafe = lx;
    g_pad_turn = rx;
    g_pad_look = ry;
}

// The dual-stick controls' sticks as the guest reads them: the left stick's x (right
// positive), the right stick's x and y (right and up positive). False, with nothing in
// `v`, unless a gamepad is in hand with the dual-stick controls.
bool dual_sticks(float v[3]) {
    bool on = !g_pad_classic && vr::gamepad_play();
    v[0] = g_pad_strafe.load(std::memory_order_relaxed);
    v[1] = g_pad_turn.load(std::memory_order_relaxed);
    v[2] = g_pad_look.load(std::memory_order_relaxed);
    input_pose("dual_sticks", &on, v, 3);
    return on;
}

constexpr uint32_t kPlayerLookButtonHeld = 0x3ED;
constexpr uint32_t kPlayerXf = 0x34, kPlayerVelocity = 0x148, kPlayerMass = 0xF8;
constexpr uint32_t kPlayerMoveState = 0x2C0, kPlayerSurface = 0x2BC;  // on the ground (2): its surface
// CTweakPlayer's per-surface arrays: translational acceleration, friction, top speed.
constexpr uint32_t kTweakAccel = 0x4, kTweakFriction = 0x44, kTweakMaxSpeed = 0xA4;

// CPlayer::TurnInput's turn (left positive), the right stick's under the dual-stick
// controls.
float turn_input(uint32_t player, float turn) {
    g_player = player;
    float v[3];
    if (mem_r32(player + kPlayerMorphState) != 0 || !dual_sticks(v)) return turn;
    return -v[1];
}

// CPlayer::ForwardInput's turn, which lengthens the forward of a stick pushed diagonally:
// none under the dual-stick controls, where the turn is on the other stick.
double forward_turn(double turn) {
    float v[3];
    return dual_sticks(v) ? 0.0 : turn;
}

// CPlayer::ComputeMovement, outside a lock, with `force` the body-relative force it is about
// to apply: the left stick's x pushes it sideways, worked out as the game works out the
// forward push from `forward` (ForwardInput's), and together no faster than the stick's
// whole throw. `dt` the step.
void strafe_force(uint32_t player, uint32_t force, float forward, float dt) {
    float v[3];
    if (!dual_sticks(v) || v[0] == 0.0f || mem_r8(player + kPlayerLookButtonHeld) || dt <= 0.0f) return;
    float s = fmaxf(-1.0f, fminf(1.0f, v[0] / 0.8f));
    if (s * s + forward * forward > 1.0f) s = copysignf(sqrtf(fmaxf(0.0f, 1.0f - forward * forward)), s);
    int surface = mem_r32(player + kPlayerMoveState) == 2 ? (int)mem_r32(player + kPlayerSurface) : 4;
    if (surface < 0 || surface > 7) surface = 0;
    const uint32_t tweak = mem_r32(kTweakPlayer);
    const float accel = rd_f(tweak + kTweakAccel + 4 * surface);
    const float friction = rd_f(tweak + kTweakFriction + 4 * surface);
    const float top = rd_f(tweak + kTweakMaxSpeed + 4 * surface);
    if (accel <= 0.0f || top <= 0.0f) return;
    const float friction_speed = friction * rd_f(player + kPlayerMass) / (dt * accel) * top;
    const float want = s * (top - friction_speed) + copysignf(friction_speed, s);
    // The body's own sideways speed: the velocity against the transform's first column.
    float side = 0.0f;
    for (int i = 0; i < 3; i++) side += rd_f(player + kPlayerXf + 16 * i) * rd_f(player + kPlayerVelocity + 4 * i);
    const float push = fmaxf(-1.0f, fminf(1.0f, (want - side) / top)) * accel;
    wr_f(force, rd_f(force) + push);
}

// ---------------------------------------------------------------------------
// What the eyes leave out (vr::GameHooks::eye_filter).
//
// The helmet. Prime draws the inside of Samus's helmet as geometry in the HUD's depth band:
// the dark frame across the top with its three blue lamps, the brackets at the bottom
// corners. On a television it is the picture's edge; in a headset it is a frame hanging
// in the middle of the view with the room visible round it, and the HUD proper -- the
// energy bar, the radar, the visor and beam selectors -- reads better without it. Its
// draws are told by their textures, the eleven materials of the helmet model, named by
// content hash (TexData::hash, printed by GCN_TEXLOG and GCN_DRAWLOG), which is the same in
// every run where a texture id is not. hide_helmet 0 in vr.txt keeps it.
//
// Flat warps. A charged shot ripples the scene behind it: the game copies a square of the
// frame around the shot and draws it back over the same square, orthographically, through
// an indirect texture (GCN_DRAWLOG: a 192x192 copy at the screen's centre, then one
// six-vertex ortho draw with the copy in one texmap and the warp in another). The eyes
// paint an orthographic draw on the HUD frame, so in stereo that was a distorted square
// of the flat view pasted in the air in front of the shot. There is no depth to give it,
// so it is left out: an ortho draw with an indirect stage sampling a copy of part of the
// frame. The scan visor's window is also an ortho draw of a partial copy, but with no
// indirect stage. hide_flat_warps 0 in vr.txt keeps them.
constexpr uint64_t kHelmetTextures[] = {
    0x34ffe8714164577full, 0xd121d9738b265065ull, 0xe30eeb56d6384648ull, 0x2ce6f2dc1af5676bull,
    0xe3572b518ab343f7ull, 0x7565fd36059507f5ull, 0xa096affd391ad7e0ull, 0xdb4007eee5dd3abfull,
    0x676e9b2ec4da4750ull, 0x1aae04f0d8f5cbe1ull, 0x4c43c6505f2cef01ull,
};

// The scan visor's tint. Entering the scan visor the game lays a translucent grey quad
// over the whole screen (draw 72 of frame 2500 of the long session: orthographic,
// exactly the screen's 640x448, no texture, one TEV stage, blend mode 0x59, in the band
// 1/64-1/32 with the scan window's frame). On the HUD frame that is a grey rectangle
// hanging in the room, smaller than the view. hide_scan_tint 0 in vr.txt keeps it.
bool is_scan_tint(const gx::PixelState& st, const gx::EyeDrawFacts& f) {
    if (!f.ortho || f.samples_copy || f.indirect) return false;
    for (int i = 0; i < 8; i++)
        if (f.tex_hash[i]) return false;
    const uint32_t stages = ((st.bp[0x00] >> 10) & 15) + 1;
    return stages == 1 && (st.bp[0x41] & 0xFFFF) == 0x59 && f.band_lo >= 0.01f && f.band_hi <= 0.04f;
}

// Films on the visor. Steam, heat and the like are drawn over the whole screen as textured
// quads in the same band as the scan tint (draws 114-117 of frame 2280 of the session
// 20261014-173803, the hot steam on entering a heated room in Magmoor: orthographic, the
// whole viewport, four draws of one state). On the HUD frame they were a rectangle hanging
// at the HUD's distance with its edges in plain view. They are drawn across the whole of
// each eye instead, fixed to the head (gx::EyeScreen): any orthographic state whose draws
// cover the whole picture together (the steam is four quadrants) and which neither samples
// a copy of the frame (the scan window does) nor warps through an indirect stage. The first
// version asked for a whole-screen viewport instead, and caught the scan window's outline,
// which is drawn into one: it went across the eye and stood far nearer than the window it
// frames. The draws' box is not enough either: the dark bars the game draws as play begins
// after a cinematic are one mesh whose box is the whole picture, and across the eye they
// slid past the edges of the field as the view morphed into stereo and left a strip along
// the bottom. So the triangles must fill the picture too (clip_area: 4 is all of it once).
// visor_film_on_head 0 in vr.txt puts them back on the frame.
bool is_visor_film(const gx::EyeDrawFacts& f) {
    const float edge = 0.98f;
    return f.ortho && !f.samples_copy && !f.indirect && f.clip_x0 <= -edge && f.clip_x1 >= edge &&
           f.clip_y0 <= -edge && f.clip_y1 >= edge && f.clip_area >= 3.6f;
}

// Drops and splashes on the visor. What lands on the glass -- a parasite's goo, the rain
// on the Tallon surface -- is a CHUDBillboardEffect: a particle effect the game puts just
// in front of its camera every frame (PreRender) and draws in perspective with the world,
// so on the screen it stays on the same place of the picture. In the eyes the camera turns
// only once the gaze is well off to the side ("The camera follows the head"), so the
// effect stayed where the body faced as the head turned: the goo hung in the air rather
// than on the face, and the rain slid the other way across the view whenever the head
// moved. Its draws are drawn across the eye as the steam's films are (gx::EyeScreen),
// where the game's picture has them, fixed to the head. They are told by where they are
// drawn: the patches at 0x8013048C and 0x801304A0 tag CHUDBillboardEffect::Render's call of
// its generator's Render. visor_drops_on_head 0 in vr.txt leaves them in the world.
constexpr uint8_t kVisorDropTag = 2;

constexpr uint32_t kBillboardGen = 0xF8;     // CHUDBillboardEffect's generator
constexpr uint32_t kElementGenLive = 0x34;   // CElementGen's live particles; Render draws none at 0

// GCN_VISORDROPLOG=1: the frames in which the game draws something on the visor, each
// effect with its generator's live particles (a CElementGen's; an electric effect's reads
// as whatever is there), to find a frame to draw-log. `effect` the CHUDBillboardEffect,
// 0 at the end of its draw.
void visor_drops_drawn(uint32_t effect) {
    static const bool log = getenv("GCN_VISORDROPLOG") != nullptr;
    if (log && in_ram(effect)) {
        const uint32_t gen = mem_r32(effect + kBillboardGen);
        fprintf(stderr, "[prime] frame %u: visor effect %08X, %d live\n", gx_frames_submitted(), effect,
                in_ram(gen) ? (int)mem_r32(gen + kElementGenLive) : -1);
    }
    gx::set_draw_tag(effect ? kVisorDropTag : 0);
}

bool is_visor_drop(const gx::PixelState& st, const gx::EyeDrawFacts& f) { return st.tag == kVisorDropTag && !f.ortho; }

// The beam change. Switching beams, the cannon is wiped away and the new one wiped in
// behind a plane that moves along it (CPlayerGun::Render): the game copies the quarter of
// the frame the cannon is in once the world is drawn (CopyScreenTex, a 320x224 copy of the
// bottom right), draws the cannon, then puts the copy back over that quarter at the plane's
// depth with a GEQUAL test (DrawScreenTex: draw 62 of frame 2380 of the session
// 20261014-205240, orthographic, the cannon's band 1/32-1/8, depth tested and written), so
// whatever of the cannon is nearer than the plane is replaced by the room behind it. The
// eyes painted that quad on the HUD frame, a flat picture of the room's corner hanging in
// the air, and with no depth test it covered the cannon in patches. It is a cut-away
// (gx::EyeRestore): each eye grabs its own room where the game copies and puts it back
// across its field at the plane's depth. The scan window, the other ortho draw of a partial
// copy, is in the band in front of the cannon's (1/64-1/32). cannon_cutaway 0 in
// vr.txt paints it on the frame again.
bool is_cannon_cutaway(const gx::PixelState& st, const gx::EyeDrawFacts& f) {
    const bool depth_test = st.bp[0x40] & 1;
    return f.ortho && f.samples_copy && !f.samples_fullscreen_copy && !f.indirect && depth_test &&
           f.band_lo >= 0.03f && f.band_hi <= 0.13f;
}

// The thermal visor's filters. With the thermal visor the game draws the room, copies the
// whole frame as intensity (a 640x448 I4 copy) and draws it back over the whole screen in
// false colour (draw 98 of frame 2600 of the session 20261014-220552: orthographic, six
// vertices, the copy jittered by a noise texture through an indirect stage, tinted
// purple). Then it draws the hot things -- the cannon, the creatures -- with their heat in
// the frame's alpha, copies the frame again and draws that copy back read as CI4 through
// the heat palette, blended by the frame's alpha (draw 114), so only the hot things take
// the palette's colours. On the HUD frame both were a picture of the flat view, smaller
// than the eye's field: the purple rectangle hanging in the room, and the cannon black
// where the second filter's stale lookup met its heat. They are drawn across the whole
// of each eye (gx::EyeScreen), and the eye substitutes its own grab of the frame for each
// copy: any orthographic state whose draws together cover the picture and which samples a
// copy of the whole frame. screen_filter_on_head 0 in vr.txt puts them back on the frame.
bool is_screen_filter(const gx::EyeDrawFacts& f) {
    const float edge = 0.98f;
    return f.ortho && f.samples_fullscreen_copy && f.clip_x0 <= -edge && f.clip_x1 >= edge &&
           f.clip_y0 <= -edge && f.clip_y1 >= edge && f.clip_area >= 3.6f;
}

// Samus's face in the visor. A bright flash close by lights her face up in the glass
// (CSamusFaceReflection::Draw): a model drawn orthographically into the window of the
// visor, twice -- once for depth alone, then in colour with the depth test, so that only
// its nearest surface shows (draws 483 and 484 of frame 22290 of the session
// 20261015-194904: 2,388 vertices, one texture, in the band 1/64-1/32). The eyes painted it
// on the HUD frame, which has no depth test, so the eyeballs drawn after the face showed
// through it: two great eyes staring out of the dark. It is composed into the HUD
// (gx::EyeFlat), which keeps the game's depth for it. Told by its texture.
constexpr uint64_t kFaceTexture = 0x25b0a289fb233f6full;

bool is_face_reflection(const gx::EyeDrawFacts& f) {
    if (!f.ortho) return false;
    for (int i = 0; i < 8; i++)
        if (f.tex_hash[i] == kFaceTexture) return true;
    return false;
}

// The scan visor's markers. CPlayerVisor::DrawScanObjectIndicators draws a small model at
// each scannable object, in perspective, in the world's band with the depth test (draws
// 314-333 of frame 12300 of the session 20261017-092738: four draws a marker, the last of
// them textured), and then DrawScanEffect copies the screen around the scan zone and
// paints the magnified copy in its window over the same place. On the screen the copy
// holds the markers, magnified. In an eye the window, on the HUD frame, covered the
// eye's own markers, and the copy -- taken from the flat view around where the gaze
// lands on the game's screen, magnified, in a window the HUD frame makes larger than the
// game's -- held the flat view's, pushed out towards the window's edge or past it: the
// markers inside the window, the region the scan targets, were missing or cut off. Until
// 2026-10-19 they were drawn last (gx::EyeLateObject), over the window, and those inside
// it on the window's plane; now the eyes leave the magnified copy out (is_scan_magnifier,
// below) and nothing covers them, so they are drawn in order, where the game models them.
// The targeting reticle round what is being scanned, which DrawScanEffect draws next
// (CTargetingManager::Draw: draws 468-470 of frame 12300 of the session 20261017-162603,
// a red square at 6.97 m round a marker 6.8-7.2 m out) is in the band at exactly 1/8,
// inside the foreground layer the eyes bring halfway to the camera for the cannon: it
// would stand 3.5 m in front of its marker. So these draws are drawn at their modelled
// distance (gx::EyeTrueScale), the reticle with its marker. They are told by where they are
// drawn: the patches at 0x80112798 (in DrawScanObjectIndicators, once it has set its
// projection and view) and 0x80113834 (in DrawScanEffect, past the targeting draw) tag the
// draws between (gx::set_draw_tag); the perspective ones from the world's band on are
// these. The marker's texture does not tell them: its content hash is not the same from
// one session to the next (1b50237ceda41ddf, 7267e5ccc5008f17, e450cde4903e4cc6 in the three
// sessions of 2026-10-17). scan_icons_true_distance 0 in vr.txt brings the reticle forward
// with the cannon again.
constexpr uint8_t kScanMarkerTag = 1;

void scan_markers_drawn(bool begin) { gx::set_draw_tag(begin ? kScanMarkerTag : 0); }

bool is_scan_marker(const gx::PixelState& st, const gx::EyeDrawFacts& f) {
    return st.tag == kScanMarkerTag && !f.ortho && f.band_lo >= 0.12f;
}

// The scan window, moved to the gaze by translating its orthographic projection
// (scan_ortho): its draws are the ortho ones in the window's band (1/64-1/32) whose
// projection is translated off the screen's centre, by less than its half-width (draws
// 337-349 of that frame; the game's own orthos there are centred). On the HUD frame, flat and fixed to the camera, the window off the frame's
// centre was seen at a slant; turned to face the viewer (gx::EyeFacing), it is square on
// to the gaze wherever it goes. scan_window_faces 0 in vr.txt leaves it on the frame.
bool is_moved_scan_window(const gx::PixelState& st, const gx::EyeDrawFacts& f) {
    if (!f.ortho || f.band_lo < 0.01f || f.band_hi > 0.04f) return false;
    const float tx = fabsf(st.proj[1]), ty = fabsf(st.proj[3]);
    return (tx > 1e-4f || ty > 1e-4f) && tx < 0.99f && ty < 0.99f;
}

// The scan window's magnified picture. DrawScanEffect paints a copy of the screen around
// the scan zone, magnified, inside the window's outline (draw 337 of frame 12300 of the
// session 20261017-092738: orthographic, in the window's band 1/64-1/32, sampling a copy of
// part of the frame, no indirect stage). In an eye that copy is of the flat view, taken
// where the gaze lands on the game's screen and stretched over a window the HUD frame
// makes larger than the game's: blocky, and never quite what the eye sees behind it. The
// markers had to be drawn over it and the window turned to face the gaze (EyeFacing, below)
// to keep it usable, and it was still more trouble than help. It is left out of the eyes;
// the outline, the corners and the markers stay, framing the eye's own view of what is
// being scanned. hide_scan_magnifier 0 in vr.txt keeps it, and then it covers the markers
// inside it, which are no longer drawn over it (is_scan_marker, above).
bool is_scan_magnifier(const gx::EyeDrawFacts& f) {
    return f.ortho && f.samples_copy && !f.samples_fullscreen_copy && !f.indirect && f.band_lo >= 0.01f &&
           f.band_lo < 0.025f && f.band_hi <= 0.04f;
}

// The targeting reticles (CTargetingManager::Draw, the CCompoundTargetReticle). Holding L
// with nothing to lock on to draws one far out ahead, where the aim is: draw 273 of frame
// 17220 of that session, 792 vertices, 74-77 units out, through the first-person camera's
// own projection but in the HUD's band (1/512-1/64), so that nothing in the world hides
// it. The HUD layer's scale (hud_band_scale, a tenth) brought it to 7.5 m, in front of a
// world tens of metres off, and the eyes saw it double. A perspective draw in the HUD's
// band through the camera's own projection is something placed in the world -- the HUD
// proper has a projection of its own (a 65-degree field against the camera's 55) -- and
// is drawn at its modelled distance (gx::EyeTrueScale), in front of everything as before.
// The camera is the render thread's own find (g_camera_pair). reticle_true_distance 0 in
// vr.txt scales it with the HUD again.
bool is_world_marker_in_hud_band(const gx::PixelState& st, const gx::EyeDrawFacts& f) {
    if (f.ortho || f.band_hi > 0.02f) return false;
    const uint32_t pair = g_camera_pair.load(std::memory_order_relaxed);
    if (!pair) return false;
    const uint32_t camera = mem_r32(pair);
    if (!is_camera(camera, kFirstPersonCameraVtable)) return false;
    const float t = tanf(rd_f(camera + kCameraFov) * 0.5f * 3.14159265f / 180.0f);
    return t > 0.0f && fabsf(st.proj[2] * t - 1.0f) < 1e-3f;
}

int eye_filter(const gx::PixelState& st, const gx::EyeDrawFacts& f) {
    ensure_config();
    if (g_visor_drops_on_head && is_visor_drop(st, f)) return gx::EyeScreen;
    if (g_scan_icons_true && is_scan_marker(st, f)) return gx::EyeTrueScale;
    if (g_reticle_true_scale && is_world_marker_in_hud_band(st, f)) return gx::EyeTrueScale;
    if (g_screen_filter_on_head && is_screen_filter(f)) return gx::EyeScreen;
    if (g_cannon_cutaway && is_cannon_cutaway(st, f)) return gx::EyeRestore;
    if (g_hide_flat_warps && f.ortho && f.indirect && f.samples_copy && !f.samples_fullscreen_copy) return gx::EyeHideDraw;
    if (g_hide_scan_tint && is_scan_tint(st, f)) return gx::EyeHideDraw;
    if (g_hide_scan_magnifier && is_scan_magnifier(f)) return gx::EyeHideDraw;
    if (g_visor_film_on_head && is_visor_film(f)) return gx::EyeScreen;
    if (is_face_reflection(f)) return gx::EyeFlat;
    if (g_scan_window_faces && is_moved_scan_window(st, f)) return gx::EyeFacing;
    if (g_hide_helmet) {
        // The whole model goes with its textured materials: the untextured strips along
        // the top and bottom and the small block by the missile count are drawn with the
        // same position matrix (they bob with the walk as the helmet does), and have
        // nothing else to name them by.
        for (int i = 0; i < 8; i++) {
            if (!f.tex_hash[i]) continue;
            for (uint64_t h : kHelmetTextures)
                if (h && h == f.tex_hash[i]) return gx::EyeHideObject;
        }
    }
    return gx::EyeKeep;
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
    // The eyes' targets. At the frontend's 1.4x with 4x MSAA the GPU took 16-17 ms a frame
    // where the Chozo Ruins' plaza is busiest, more than a 72 Hz frame has; 1.2x with 2x
    // MSAA took 8 ms there (docs/dev/vr.md, "The Chozo Ruins").
    c.eye_scale = 1.2f;
    c.msaa = 2;
    // The EFB the eyes' copies are made from, at twice the hardware's resolution: the scan
    // visor's window magnifies a copy of it, and the acid's fog reads depth copies of it.
    // Measured free in the heaviest room (docs/dev/vr.md): the flat pass is still small.
    c.stereo_scale = 2;
    c.background_band = 0.99f;
    c.foreground_band = 0.5f;
    c.foreground_scale = 0.5f;
    // The HUD -- the band below 1/512 -- is modelled 16-21 units out in view space, which
    // at the foreground's half scale stood eight to ten metres away, a billboard rather
    // than a visor. Drawn at a twentieth of its distance, 0.8-1.05 m, it reads at arm's
    // length, the same angular size (a tenth until 2026-10-19, chosen in the headset);
    // hud_band_scale in vr.txt moves it (smaller is nearer).
    // Two bands hold it: 0-1/512 (the map, a few scraps) and 1/512-1/64 (the energy bar,
    // the radar, the selectors, the visor's outline), found with the eye's draw log
    // (GCN_DRAWLOG in an eye names each draw's layer). 1/64 as the float GX holds it comes
    // out a hair above 1/64, so the bound has slack; the cannon's band starts at 1/32.
    c.hud_band = 0.02f;
    c.hud_band_scale = 0.05f;
    // The thermal visor's innermost triangle (draw 115 of frame 2600 of the session
    // 20261014-220552: 243 vertices, flat, 2.95 units out, in the 1/512-1/64 band) is
    // modelled seven times nearer than the rest of its HUD, 14-27 units out, so at that
    // scale it stood 0.3 m from the eyes. A HUD draw nearer than 10 units is drawn with its
    // nearest point at 22 instead, at the same angular size: the far end of the visor's
    // nested triangles (14.2-21.9), the one it sits inside. hud_push_below 0 in vr.txt
    // turns it off.
    c.hud_push_below = 10.0f;
    c.hud_push_to = 22.0f;
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
    c.panel_band = 0.02f;  // both HUD bands, with the same slack as hud_band
}

const bool installed = [] {
    vr::GameHooks h;
    h.config_defaults = config_defaults;
    h.config_loaded = config_loaded;
    h.wants_stereo = wants_stereo;
    h.wants_panel_pair = wants_panel_pair;
    h.map_pad = map_pad;
    h.map_gamepad = map_gamepad;
    h.eye_filter = eye_filter;
    vr::set_game_hooks(h);
    return true;
}();

}  // namespace

// The dual-stick controls' patches in CPlayer's movement (recomp/patches.txt, map_gamepad).
extern "C" float prime_turn_input(uint32_t player, float turn) { return turn_input(player, turn); }
extern "C" double prime_forward_turn(double turn) { return forward_turn(turn); }
extern "C" void prime_strafe_force(CPU* c) { strafe_force(c->r[29], c->r[1] + 220, (float)c->f[31].d, (float)c->f[27].d); }

// The patch in CPlayerGun::Update (recomp/patches.txt); r28 is the gun.
extern "C" void prime_aim_gun(CPU* c) { aim_gun(c->r[28]); }

// The patches in CMFGame::Draw and CMFGameLoader::Draw (recomp/patches.txt): whether the
// game draws its world this frame.
extern "C" void prime_world_drawn(uint32_t drawn) { g_world_drawn.store(drawn, std::memory_order_relaxed); }

// The patch at CAutoMapper::Draw (recomp/patches.txt): where the automapper is.
extern "C" void prime_automapper(uint32_t self) {
    g_automapper.store(self, std::memory_order_relaxed);
    g_automapper_frame.store(gx_frames_submitted(), std::memory_order_relaxed);
}

// The patches at three calls of the CFrustumPlanes constructor (recomp/patches.txt), in
// place of the call.
extern "C" void prime_cull_frustum(CPU* c) {
    if (c->lr == 0x800474DCu) publish_view_turn();  // SetupViewForDraw's ("The camera follows the head")
    cull_to_eyes(c);
}

// GCN_FACELOG=1: the frames in which the game draws Samus's face reflected in the visor
// (CSamusFaceReflection::Draw, past its test of the hidden flag; recomp/patches.txt), to
// find one to draw-log.
extern "C" void prime_face_drawn(void) {
    static const bool log = getenv("GCN_FACELOG") != nullptr;
    if (log) fprintf(stderr, "[prime] frame %u: face reflection drawn\n", gx_frames_submitted());
}

// Locking on where the viewer looks. The game gathers what can be locked on to from a box
// in front of the first-person camera's gun-follow transform (CPlayer::UpdateOrbitableObjects:
// BuildNearListBox, 0x80180154, over GetFirstPersonCameraTransform): the target distance
// ahead, but only a few metres to either side and above and below. On a television the
// free look pitches that transform, so the box goes wherever the player looks. In the
// headset the head looks and the body stays level, and the follow turn goes onto the
// camera's own transform, not the gun-follow one, so the box stayed level with the body:
// whatever was more than about ten metres above or below it was never a candidate. The
// stalactites in Frost Cave, 25 to 85 degrees up and 25 to 55 metres away, could not be
// locked on to (session 20261015-194904, frames 121450-130700), though the game itself
// passed them (ValidateOrbitTargetId answered 0 for both). The box's transform is aimed
// along the gaze instead, from the same place: the eyes the hooks last read, at most a
// frame old (no read of its own, which would be input), and nothing without them.
// lock_on_follows_gaze 0 in vr.txt gives the game's box back.
uint32_t orbit_box_transform(uint32_t xf, uint32_t sp) {
    ensure_config();
    if (!g_lock_follows_gaze || vr::gamepad_play() || !in_ram(xf) || gx_frames_submitted() - g_last_eyes_frame > 1) return xf;
    float g[3], local[3], m[12], fwd[3];
    gaze(g_last_eyes, g);
    eye_to_camera(g, local);
    read_xf(xf, m);
    rotate(m, local, fwd);
    float right[3] = {fwd[1], -fwd[0], 0.0f};  // forward x world up
    const float len = sqrtf(right[0] * right[0] + right[1] * right[1]);
    if (len < 1e-3f) return xf;  // straight up or down: the game's box
    right[0] /= len;
    right[1] /= len;
    const float up[3] = {right[1] * fwd[2], -right[0] * fwd[2], right[0] * fwd[1] - right[1] * fwd[0]};
    // In the guest's stack well below its frame, read by BuildNearListBox before anything
    // else is called.
    const uint32_t out = (sp - 0x1000) & ~15u;
    for (int i = 0; i < 3; i++) {
        const uint32_t row = out + 16 * i;
        wr_f(row, right[i]);
        wr_f(row + 4, fwd[i]);
        wr_f(row + 8, up[i]);
        wr_f(row + 12, m[4 * i + 3]);
    }
    return out;
}

// The scan visor's patches (recomp/patches.txt): once a frame in CPlayer::UpdateOrbitZone,
// the two draws in CPlayerVisor::DrawScanEffect, and the orbit state as the body's and the
// camera's turning read it.
extern "C" void prime_scan_frame(CPU* c) {
    scan_frame(c->r[3]);
    orbit_log(c->r[3], c->r[4]);
}
extern "C" void prime_scan_copy_src(CPU* c) { scan_copy_src(c); }
extern "C" void prime_orbit_box(CPU* c) { c->r[3] = orbit_box_transform(c->r[3], c->r[1]); }
extern "C" void prime_scan_ortho(CPU* c) { scan_ortho(c); }
extern "C" void prime_scan_markers(uint32_t begin) { scan_markers_drawn(begin != 0); }
// The patches in CHUDBillboardEffect::Render (recomp/patches.txt).
extern "C" void prime_visor_drops(uint32_t effect) { visor_drops_drawn(effect); }
extern "C" uint32_t prime_orbit_state(uint32_t player) { return orbit_state_for_turning(player); }

// The patch in CFirstPersonCamera::Think (recomp/patches.txt): the camera follows the head.
extern "C" void prime_turn_camera(CPU* c) { turn_camera(c->r[31], (float)c->f[31].d); }
// The patches in CPlayer::UpdateOrbitOrientation (r3 the camera) and
// CPlayer::UpdateFreeLookState (r4) where they load the first-person camera's forward.
extern "C" void prime_orbit_facing(CPU* c) { unturned_forward(c, c->r[3], 0, 1, 2); }
extern "C" void prime_free_look_facing(CPU* c) { unturned_forward(c, c->r[4], 31, 30, 29); }
