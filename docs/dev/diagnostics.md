# Diagnostics

The tooling -- input logging and replay, scripted input, frame dumps, the benchmark, texture
dumps, draw logs, the crash dumps -- is the shared runtime's and is documented in
[`gcn-recomp/docs/diagnostics.md`](../../gcn-recomp/docs/diagnostics.md). This file holds
only what is specific to Metroid Prime.

## Routes

Every run records an input log under `saves/inputs/<timestamp>/`; `--replay=<dir>` plays one
back. Keep logs that reach interesting places in `routes/` with a name that says where they
go (they are small text files plus a memory card snapshot).

The scripted route to gameplay from a fresh boot (no save on the memory card):

```
GCN_INPUT="1300:START:10,1700:A:10,2100:A:10,2500:A:10,2900:A:10"
```

Title (press Start), *Start*, *New Game*, confirm, and through the intro text; the cutscene
runs on its own and gameplay begins around frame 9000. Scripted presses count presented
frames, which under the virtual clock are as repeatable as anything else, but a recorded log
(keyed by pad poll) is the exact form; record one by replaying a script and keep that.

## Idle loops

`recomp/idle.txt` names the backward branches where this game only waits for an interrupt:
the SDK scheduler's spin in `SelectThread` and the frame wait in `CGraphics::EndScene`
(a loop around `OSYieldThread`). The runtime jumps guest time to the next event there.
If a new wait loop turns up -- the symptom is the host at 100% of a core and `--sample`
showing the main thread in the same function frame after frame -- find its back-edge in the
generated C and add it.

## Naming guest code

`analysis/symbols.txt` names every function, so `--sample`, the crash dumps and
`GCN_COUNT` report readable names such as `THPVideoDecode` or
`Update__12CMoviePlayerFf`. The same file (and the decomp project it came from) is the
quickest way to find where to look for a given subsystem.

## Panics

The game's `OSPanic` is replaced (`recomp/hle.txt`) so that a panic prints the message and a
guest backtrace and exits, instead of spinning in `PPCHalt` looking like a hang.
