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
  world geometry, lighting and the planet outside all drawn correctly.
- `prime_bench` reports the guest running at the game's full 60 fps with room to spare.

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
- Windows and Quest builds. The CMake file still carries the Windows SDL path from Blue
  Storm but nothing here has been built there.

## Next milestones

1. MusyX DSP HLE, enough to drive the movie player and produce sound.
2. Title and menu videos.
3. A play-through of the frigate to the crash on Tallon IV, watching for faults.
4. VR.
