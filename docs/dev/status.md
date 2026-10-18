# Status

Where the port stands, what is known to be broken now, and what comes next. Only the
current state belongs here: a fixed problem leaves this file, and how it was found and
fixed lives in the file for its area (`vr.md`, `graphics.md`, `audio.md`, gcn-recomp's
`docs/`) and in the history.

## Where it stands (2026-10-18)

- **Complete: beaten from start to finish in the headset.** The game was played through to
  the credits on the Quest 3 in sessions of an hour and more, ending with the Metroid
  Prime fight in the Impact Crater (`inputs/20261018-090925`), with no known significant
  bugs left. It is played in stereo, with the arm cannon in the right hand, the scan visor
  and the camera following the head, and the menus, map and morph ball on a stereo
  theater panel. `vr.md` covers how each of those works.
- **Desktop builds** run on Apple Silicon Macs and Windows (x64 and ARM64; a VM without an
  OpenGL driver renders through Mesa's llvmpipe, slowly). Sound is the game's own, through
  the AX HLE (`audio.md`).
- **Saving and dying** work: save stations write to the memory card, and a death's console
  reset starts the game again in a new process.
- **Replays are exact.** The guest runs on a virtual clock and the input log, including the
  headset's poses, is keyed by pad poll, so a recorded session replays instruction for
  instruction (`gcn-recomp/docs/diagnostics.md`), and says so if it parts from its
  recording. Sessions recorded before 2026-10-16 replay faithfully only with the VR hooks
  as they were then, and with `GCN_SCANLOG` on if it was on when recording (`vr.md`, "An
  hour in the headset").

## Known broken

- **Crackle in the ending's burning-temple shot**: the streamed music's buffers are played
  twice when that CPU-heavy cutscene runs at 36-41 frames a second (`vr.md`, "The
  play-through's frame rate").
- **Judder in the headset**: the game's 60 frames on the display's 72 Hz, one frame in five
  held twice. A few CPU-heavy stretches dip to 45-50 frames a second, now without breaking
  the sound.
- **NES Metroid** (the Fusion-link bonus) cannot run: it is a REL module (`NESemuP.rel`)
  loaded at runtime, and the recompiler translates only `main.dol`.

## Next milestones

1. VR: the guest paced on the display's frame timing, for an even cadence; the ending
   cutscene's crackle (`vr.md`, "The play-through's frame rate").
