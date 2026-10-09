# Status

Where the port stands, what is known to be broken, and what comes next. Dates are when the
observation was made; re-check anything old before building on it.

## What works (2026-10-07)

Built and run on an Apple Silicon Mac from the Rev 2 (v1.02) USA disc as a CISO:

- Boot through the SDK's OS init, the Retro Studios logo, the title screen and the main menu,
  including the THP videos behind them (fixed 2026-10-07: the HID2 read dropped the LCE bit,
  so the THP decoder refused every frame and the YUV planes stayed zero, which the YUV
  combiner shows as solid green).
- *New Game* → the intro text → the opening cutscene (Tallon IV from orbit, the approach to
  the Frigate Orpheon) → first-person gameplay on the frigate, with the combat visor HUD,
  world geometry, lighting and the planet outside all drawn correctly. The intro cinematic
  renders in full: the gunship, Samus's model and arm cannon, the frigate, the landing.
- `prime_bench` reports the guest running at the game's full 60 fps with room to spare.

Also built and run on Windows 11 ARM64 (2026-10-08), in a VMware VM, with the same clang, CMake
and Ninja setup as Blue Storm and no source changes: `.\build.ps1`, then
`build\prime.exe --replay=routes/new-game` plays through the menus and the intro into the
frigate, rendering as on the Mac, hairlines included. The VM has no OpenGL driver (only GDI
Generic 1.1, and its VMware SVGA adapter has no D3D12 either), so it renders through Mesa's
llvmpipe: MSYS2's `mingw-w64-clang-aarch64-mesa` 26.2.4 with its DLL dependencies copied
beside `prime.exe`, and `GALLIUM_DRIVER=llvmpipe`. mesa-dist-win's MSVC release ships no
ARM64 build. That software path runs at about 22 fps (5,400 frames in 240 s), against 60 on
real hardware; it is for checking the port, not for playing. (Since the virtual clock, a
host that slow runs the game in slow motion rather than dropping frames.)

Reached without a controller by replaying a recorded route (exactly, since 2026-10-08:
see "Replay is deterministic" below):

```
./build/prime --replay=routes/new-game
```

Every run records its input to `saves/inputs/<timestamp>/`; see `routes/README.md` and
`gcn-recomp/docs/diagnostics.md`.

The recompiler and runtime moved to the `gcn-recomp` submodule on 2026-10-07; this repository
now holds only the symbol file, the HLE tables, routes and notes.

## Replay is deterministic (2026-10-08)

Replays drifted by hundreds of frames over a few minutes. Two things in this game made that
inevitable under a wall-clock time base: the simulation step is the measured interval
between frames (`UpdateTicks` reads `OSGetTime`), and the frame wait is a loop in
`CGraphics::EndScene` calling `OSYieldThread` until the retrace callback flips a flag, so
any host hitch changed both the step and the number of ticks before the next frame. The
shared runtime now runs the guest on a virtual clock that advances with the guest's own
execution (`gcn-recomp/docs/diagnostics.md`, "The clock"), and the input log is keyed by
pad poll rather than presented frame, so a replay follows the recording instruction for
instruction. Prime's two wait loops are listed in `recomp/idle.txt` (the SDK scheduler's
spin and the `EndScene` yield loop); the runtime skips guest time to the next event there
instead of iterating, which is also what keeps the host from burning a core per frame.
Logs recorded before this (version 1 in their header, `routes/new-game` among them) still
play, approximately.

## Known broken

- **Main menu text is doubled and striped.** Each menu entry is drawn as a sharp white copy
  plus a dimmer copy offset a few pixels right and down, and "MAIN MENU" has horizontal
  banding; the "A Select / B Back" prompts are clean. The font texture itself decodes
  correctly (`GCN_TEXDUMP`, texture 1610, CI4 256x128), and the "[ PRESS START ]" texture
  (CMPR 256x32) does too, so the fault is in the draw, not the decode. The menu frame has
  about thirty single-draw states all on the one font texture. It may partly be the game's
  own drop-shadow and slide-in animation; a Dolphin reference capture was attempted and
  did not get as far as booting (see `diagnostics.md`), so this is unconfirmed.
- ~~Missing gunship and Samus in the intro~~ Fixed 2026-10-08. Prime streams CPU-skinned
  vertices through the write-gather pipe redirected into a vertex buffer; the PI FIFO end
  register dropped the 64 MB bit the SDK sets for that, so the stream wrapped to address 0.
  Also fixed on the way: ARAM addresses wrapped, so the SDK's size probe found expansion ARAM
  that is not there; and a partial gather line prepended to the redirected stream.
- **Thin orange lines across the frigate exterior and hangar.** Still open. They are hairline
  triangles in the frigate's geometry whose two near vertices sit a few float ulps apart
  (the game's own vertex data, as `GCN_VTXLOG` shows), plus particle quads with one corner
  displaced. Snapping vertices to a sixteenth-pixel grid in the shader, as the hardware's
  rasteriser does, did not remove them, so it is not sub-pixel precision; the displaced
  corners point at a vertex-stream fault not yet found. The draw log's sliver finder
  (`GCN_SLIVER_RATIO=0.03 GCN_DRAWLOG=<frame>`) lists them. Draining the write-gather
  buffer at more points (sync, pointer reads, idle) was tried as a cause and broke the
  frame protocol instead; see the note in `gcn-recomp/runtime/gx/fifo.cpp`.
- ~~Crash in `CGameAllocator::Alloc` about twelve minutes into the frigate~~ Fixed
  2026-10-08, the same day it was seen. Heap corruption, found by the allocator: a free
  block's header had been zeroed. Four replays of the run's log could not reproduce it
  (the game's timestep comes from the time base, so no replay followed the same route
  under the host clock), which led to the virtual clock below; the second occurrence,
  on a checked build (`src/heap_check.cpp`, see `diagnostics.md`), was reported within a
  frame of the write and its log replayed to the same instruction. The watch replay named
  the writer: the SDK's `GXRestoreWriteGatherPipe`, as emulated. Prime streams each
  CPU-skinned model's vertices through the write-gather pipe redirected into a heap block
  sized exactly for them; the SDK then pads the pipe with 31 zero bytes, waits for it to
  report empty and rewrites WPAR before repointing the PI FIFO. The runtime wrote those
  pending bytes out as a partial line at the PI register write, straight over the next
  block's header. On the hardware the WPAR write empties the buffer, and now it does
  here too (`gp_reset` in `gcn-recomp/runtime/gx/fifo.cpp`). The previous belief, from
  the missing-models fix, was that the pending line should be written out before the
  pointers move; it should be discarded, and the models render identically either way
  (bit-identical frame dumps over 200 seconds).
- ~~Surfaces drawn black after going through a door~~ Fixed 2026-10-09. Reproduced from
  a headset session's input log on the desktop: after a door in the Chozo Ruins, the next
  room's walls were black. Their first TEV stage samples a lightmap that the new room had
  loaded into memory an EFB copy's buffer once occupied. The texture cache still mapped that
  address to the copy (a GPU texture, never written to RAM), and every lookup kept the copy
  from expiring, so the walls were drawn with a stale screen copy. The cache now fingerprints
  a copy's destination and drops the copy when the RAM there changes ("Textures" in
  `gcn-recomp/docs/diagnostics.md`). Frames before the room change are byte-identical.
- ~~Crash in water rendering on the Quest, about eighteen minutes in~~ Fixed 2026-10-08,
  not yet re-played to the same spot. SIGSEGV in `UpdatePatchWithNormals`
  (`CFluidPlaneCPU`, under `CScriptWater::Render`) at guest `0xE0004020`. The patch's
  45x45 height field sits in the 16KB locked cache, and the read was one row past it. The
  runtime committed exactly 16KB at `0xE0000000`; it now commits 256KB, as Dolphin maps.
- ~~Save stations replenish energy but never offer to save~~ Fixed 2026-10-08. The card
  itself was fine: the directory held the save made at *New Game*, and the in-game
  attempt never sent the card a single command. The gate is in the save station's script
  message handler (`CScriptSpecialFunction::AcceptScriptMsg`, the `SaveStation` case):
  it only defers the state transition to the save screen when the card serial the game
  recorded at start (`CGameState`, the 64-bit value at +0x210) is nonzero; otherwise it
  sends the script the no-save state, which plays the "energy replenished" message on its
  own. The serial is `CARDGetSerialNo`, the XOR of the card header's 32 serial bytes, and
  the runtime formatted its card with a format time of zero, which makes those bytes all
  zero. The card is now formatted with a real serial (`write_serial` in
  `gcn-recomp/runtime/hw/memcard.cpp`, scrambled the way the CARD library's `VerifyID`
  checks) and a card found with a zero serial gets one when loaded, keeping its files.
  The old `saves/memcard_a.raw` therefore still works. Not yet confirmed at a save station
  in play; the mount and file read of the existing save were checked by replay.
- **Grey letterbox bars on the gunship close-up.** The cinematic's bars should be black; in
  the close-up of the ship's underside they come out mid-grey. Not investigated.
- **Retro's textures are stored bottom-up.** A dumped texture appears vertically flipped;
  the models' UVs undo it, so this is not a bug. Do not "fix" it in the decoder.
- ~~No audio~~ Fixed 2026-10-08. The belief that Prime runs a MusyX ucode the runtime does
  not emulate was wrong: the ucode is Nintendo's AX (hash 4E8A8B21, the one Dolphin's HLE
  handles), and MusyX is the CPU-side driver. The AX HLE had the length of one command
  wrong and skipped the others MusyX uses, so every command list was abandoned at its
  second command. See `audio.md`.
- **NES Metroid (the Fusion-link bonus) cannot run.** It is a REL module (`NESemuP.rel`)
  loaded at runtime; the recompiler only translates `main.dol`, so entering it will end in
  `call_indirect: no function at …`.
- The `[OS] UART:` lines drop their first character and some digits (`rotecting stack`,
  `nitializing renerer`). Cosmetic; the game writes them through its own byte-at-a-time path
  and `OSReport` sees them split. Not investigated.

## Started, not yet played

- Quest 3 (2026-10-08). The OpenXR app is installed on a Quest 3, on the headset frontend
  now shared in `gcn-recomp/android/`, but has not been played in the headset yet. The
  benchmark ran on it at 164% of the game's 60 fps over the menus. Stereo is chosen from
  the game's camera manager (first-person camera current, no cinematic camera), with the
  left thumbstick click as an override. See `vr.md`.

## Next milestones

1. A play-through of the frigate to the crash on Tallon IV, watching for faults; listen for
   what the AX HLE still gets wrong (ITD, the polyphase resampler).
2. VR: a steady 60 fps in stereo on the Quest 3. The render thread's share was cut on the
   Mac on 2026-10-09 (the flat pass trimmed, uniforms shadowed); the GPU's share, two eyes
   at `eye_scale` 1.4 with 4x MSAA, is the part still to measure down (`vr.md`, "The render
   thread in stereo").
