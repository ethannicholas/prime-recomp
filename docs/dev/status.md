# Status

Where the port stands, what is known to be broken, and what comes next. Dates are when the
observation was made; re-check anything old before building on it.

## What works (2026-10-07)

Built and run on an Apple Silicon Mac from the Rev 2 (v1.02) USA disc as a CISO:

- Boot through the SDK's OS init, the Retro Studios logo, the title screen and the main menu.
- *New Game* → the intro text → the opening cutscene (Tallon IV from orbit, the approach to
  the Frigate Orpheon) → first-person gameplay on the frigate, with the combat visor HUD,
  world geometry, lighting and the planet outside all drawn correctly.
- `prime_bench` reports the guest running at the game's full 60 fps with room to spare.

Reached without a controller by scripted input (see `diagnostics.md`):

```
MP_INPUT="1300:START:10,1700:A:10,2100:A:10,2500:A:10,2900:A:10" ./build/prime
```

## Known broken

- **Title screen and main menu backgrounds are solid green, and menu text is garbled.** The
  backgrounds are THP videos (`Video/00_first_start.thp`, `01_startloop.thp`, …, read off the
  disc's FST). Solid green `(0,135,0)` is exactly what the SDK's YUV→RGB TEV produces from
  all-zero Y/U/V planes, so the decoded frames are never written, or written with zeros. One
  dumped frame (frames2/frame_01600 in the first session) had its lower half full of random
  noise, which looks like a partly-written frame buffer. Leading hypothesis: the game's movie
  player paces video on audio playback, and the audio never plays (below), so decoding never
  advances. Test that before suspecting the CPU translation of the decoder.
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
