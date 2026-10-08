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
real hardware; it is for checking the port, not for playing.

Reached without a controller by replaying a recorded route:

```
./build/prime --replay=routes/new-game
```

Every run records its input to `saves/inputs/<timestamp>/`; see `routes/README.md` and
`gcn-recomp/docs/diagnostics.md`.

The recompiler and runtime moved to the `gcn-recomp` submodule on 2026-10-07; this repository
now holds only the symbol file, the HLE tables, routes and notes.

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
- **Grey letterbox bars on the gunship close-up.** The cinematic's bars should be black; in
  the close-up of the ship's underside they come out mid-grey. Not investigated.
- **Retro's textures are stored bottom-up.** A dumped texture appears vertically flipped;
  the models' UVs undo it, so this is not a bug. Do not "fix" it in the decoder.
- **No audio.** Metroid Prime runs the MusyX DSP microcode, not AX. The DSP HLE inherited from
  Blue Storm boots whatever ucode it is given and then parses mails as AX, which happens to
  keep the game running but produces nothing. See `audio.md`.
- **NES Metroid (the Fusion-link bonus) cannot run.** It is a REL module (`NESemuP.rel`)
  loaded at runtime; the recompiler only translates `main.dol`, so entering it will end in
  `call_indirect: no function at …`.
- The `[OS] UART:` lines drop their first character and some digits (`rotecting stack`,
  `nitializing renerer`). Cosmetic; the game writes them through its own byte-at-a-time path
  and `OSReport` sees them split. Not investigated.

## Not started

- VR. See `vr.md` for the plan. Unlike Wave Race, Prime is first person: the game's camera
  is the player's head, so stereo is a per-eye projection and view on the existing GX
  transform, not a reconstruction of where the viewer should be.
- Quest build. Blue Storm's Android frontend (OpenXR NativeActivity, AAudio, APK packaging
  without Gradle) is the starting point.

## Next milestones

1. MusyX DSP HLE, enough to drive the movie player and produce sound.
2. Title and menu videos.
3. A play-through of the frigate to the crash on Tallon IV, watching for faults.
4. VR.
