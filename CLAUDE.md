# Working notes for this repository

This is a static recompilation of *Metroid Prime* (GameCube), with a VR port as the goal. It
follows the structure and rules of `~/Source/bluestorm-recomp` (Wave Race: Blue Storm), from
which the recompiler and the GameCube runtime were taken.

**The repository must contain no game code or data.** Only original source (recompiler,
runtime, renderer) and metadata about code layout (`analysis/`). The disc image lives in
`rom/` and the generated C in `build/gen/`, both ignored by git. Never commit, quote at
length, or paste disassembly of the game into tracked files; a function name and address is
fine, its instructions are not.

`README.md` is the public landing page. It holds only what someone needs to build and play the
game. Keep it that way: no investigation records, measurements, debugging environment variables
or design rationale go there.

Those live in `docs/dev/`, and that is where to record anything non-obvious you learn, in the
file for its area:

- `docs/dev/status.md` — how far the game gets, what is known to be broken, and what the next
  milestones are.
- `docs/dev/diagnostics.md` — `MP_*` environment variables, scripted input, the benchmark, the
  guest call stack, and how `OSPanic` is reported.
- `docs/dev/graphics.md` — GL profile requirements and the shader cache.
- `docs/dev/audio.md` — the DSP: Metroid Prime uses MusyX, not AX, so the AX HLE inherited
  from Blue Storm does not apply.
- `docs/dev/vr.md` — the VR plan, once there is one.

When a belief is overturned, say what the belief was and what disproved it, so the next attempt
does not start from nothing.

Commit on `main` and push as work lands; no feature branches or PRs.
