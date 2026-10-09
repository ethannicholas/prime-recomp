// Metroid Prime's answers to the GX front end's questions (gcn-recomp/runtime/gx/gx.h).
//
// Draw-sync lag. Prime sets a draw-sync token after every skinned model it draws
// (CSkinnedModel::PostDrawFunc) and reads it back (Skinning::AddSkinnedRef,
// CSkinnedModel::EnsureAllocation and TickAllocations) to learn which parts of its skinning
// buffer the GPU has finished with -- two reads per model, about eighty a frame in a room
// full of creatures. Read exactly, each one waited for the front end to decode everything
// drawn before it, which by the time the creatures are drawn is the room's whole world:
// 4-5 ms of every frame in the Chozo Ruins, behind 20,000 small fans. Shown a GPU a frame
// behind, as the real hardware usually is, it never waits and never runs short of buffer
// (no read spun on the way into that room); frames are byte-identical either way.
// docs/dev/vr.md, "The second room".
#include "gx/gx.h"

namespace {

const bool installed = [] {
    gx::set_draw_sync_lag(true);
    return true;
}();

}  // namespace
