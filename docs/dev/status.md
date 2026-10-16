# Status

Where the port stands, what is known to be broken now, and what comes next. Only the
current state belongs here: a fixed problem leaves this file, and how it was found and
fixed lives in the file for its area (`vr.md`, `graphics.md`, `audio.md`, gcn-recomp's
`docs/`) and in the history.

## Where it stands (2026-10-16)

- **Played in the headset.** The Quest 3 app is the main way the game is played: sessions
  of an hour and more, from the frigate through the Chozo Ruins, Magmoor Caverns and
  Phendrana Drifts, in stereo, with the arm cannon in the right hand, the scan visor and
  the camera following the head, and the menus, map and morph ball on a stereo theater
  panel. `vr.md` covers how each of those works.
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

- **NES Metroid** (the Fusion-link bonus) cannot run: it is a REL module (`NESemuP.rel`)
  loaded at runtime, and the recompiler translates only `main.dol`.

Seen early on the desktop and not re-checked since, so possibly gone:

- Main menu text doubled and striped (2026-10-07).
- Thin orange hairlines across the frigate's exterior and hangar (2026-10-08).
- Grey rather than black letterbox bars on the intro's gunship close-up (2026-10-07).
- The `[OS] UART:` log lines drop characters (`rotecting stack`); cosmetic.

## Next milestones

1. VR: a steady frame rate in stereo on the Quest 3 in the heaviest rooms (`vr.md`,
   "Performance" and "The Chozo Ruins").
2. Play on through the game in the headset and fix what turns up.
