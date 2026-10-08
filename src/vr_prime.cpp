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
#include <cstdio>
#include <cstdlib>

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

bool wants_stereo(const gx::Batch&) {
    static const bool log = getenv("GCN_STEREOLOG") != nullptr;
    static uint32_t pair = 0, frames_since_search = 1000;
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

void config_defaults(VrConfig& c) {
    // Prime's world units are taken to be metres. A guess, to be judged from inside the
    // headset like Blue Storm's 50 was.
    c.units_per_metre = 1.0f;
    // Close enough to see the arm cannon, far enough to keep depth precision for rooms
    // that are a few hundred metres across.
    c.near_m = 0.05f;
    c.far_m = 1000.0f;
}

const bool installed = [] {
    vr::GameHooks h;
    h.config_defaults = config_defaults;
    h.wants_stereo = wants_stereo;
    vr::set_game_hooks(h);
    return true;
}();

}  // namespace
