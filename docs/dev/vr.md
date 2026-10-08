# VR

Target: Meta Quest 3 through OpenXR, on the headset frontend in `gcn-recomp/android/`.

## Where it stands (2026-10-08)

Installed on a Quest 3 and not yet played in it: launched over adb with nobody wearing the
headset, Horizon OS holds a 6DoF app at its launch check
(`Requires6dofModeLaunchInterceptor` in logcat), so the first real run has to be started
from inside the headset.

- **The frontend is shared.** Blue Storm's Android work -- the OpenXR NativeActivity app,
  the headless EGL harness, the AAudio device and APK packaging without Gradle -- moved into
  `gcn-recomp/android/` and `gcn-recomp/tools/package-apk.ps1`, with everything Wave
  Race-specific taken out and put behind `vr::GameHooks` (`gcn-recomp/runtime/vr_game.h`).
  `gcn_add_game(... ANDROID_PACKAGE com.example.prime)` builds `libprime.so` (the app) and
  `prime_egl` (the harness) on an Android build. Blue Storm still builds its own copies until
  it moves over.
- **`src/vr_prime.cpp`** holds Prime's answers: `units_per_metre 1` (world units taken to be
  metres -- a guess until judged in the headset), near 0.05 m, far 1000 m, and the stereo
  decision below. The left thumbstick click switches between theater and stereo by hand,
  overriding the game until its answer next changes.
- **Stereo itself is the renderer's existing eye path** (`render_execute_eye`): vertices
  reach the renderer in the game camera's view space, so each eye is that view re-projected
  with the headset's per-eye projection and head pose, and the game's 2D elements are
  painted on a frame hung in front of the camera. See `gcn-recomp/docs/graphics.md` and Blue
  Storm's `docs/dev/vr.md` for how that path separates off-screen passes, EFB copies and
  the HUD.

## The CPU is not the limit (2026-10-08)

`prime_bench` on the Quest 3, run over adb against the image the app uses
(`/sdcard/Android/data/com.example.prime/files/game.ciso`), unpaced under the virtual clock:
**98 fps steady state, 164% of the game's 60** (6,302 frames in 60 s). With no input the
benchmark sits on the title screen and menus, so this is not yet a gameplay figure; the
frigate needs measuring the same way with a route (`GCN_INPUT` or a replay in the env).
Under the virtual clock a host that cannot keep up plays the game in slow motion rather
than dropping frames, which is why this was measured before anything was rendered.

## When to be in stereo

Stereo for first-person play; theater for menus, cinematics and the morph ball. Read from
the game's camera manager (`wants_stereo` in `src/vr_prime.cpp`, where the layout is
written out):

- The current camera's id is the first field of `CCameraManager`. **It does not move for a
  cinematic**: on the `new-game` route it was the first-person camera's from the moment the
  frigate loaded (frame 2280) through the whole intro cinematic. A cinematic shows instead in
  the count of cinematic cameras in use, the word at +0x8, which `GetCurrentCamera` and
  `IsInCinematicCamera` both read. It went to 1 as the intro cinematic began (frame 2713) and
  2 at frame 5890. So stereo is *current is the first-person camera and that count is 0*.
  The count dropping back to 0 when play begins (around frame 9000) has not been watched
  yet: the run that would have shown it was cut short. If it does not, the left thumbstick
  click still gets into stereo.
- Finding the manager. There is no global pointer to it, and the first attempt assumed it
  and the cameras were each heap blocks of their own, which found nothing: the manager is
  built in a static arena, `sAllocSpace` (0x8045D614 on the frigate), and the first-person
  camera sits inside a larger heap block. What holds is the pair of pointers to the
  first-person and ball cameras at +0x88 (revision 2's offset; one field later than earlier
  revisions), each camera recognised by its vtable, with the ball camera -- which *is* a
  heap block -- still allocated, so a previous world's manager is not mistaken for a live
  one.
- `GCN_STEREOLOG=1` on the desktop (or `prime_egl`) prints the manager when found, every
  change of camera or cinematic count, and the answer, numbered like the frame dumps:

  ```
  GCN_STEREOLOG=1 GCN_DUMP_RANGE=2000-10000 ./build/prime --replay=routes/new-game \
      --hidden --fast --dump-dir=frames --dump-every=150
  ```

  The search reads all of RAM, at most every two seconds while there is no manager; on this
  VM's software renderer the route reaches gameplay in about 12 minutes.

## First session in the headset (2026-10-08)

Played to the first room. Three faults, all addressed the same day:

- **The arm cannon read as about twice its size.** Not the eye separation: the cannon is
  modelled a unit long three units in front of the camera and a unit to the right (draws 4-7
  of the first frame of play on the frigate, `GCN_DRAWLOG`), and the visor frame hangs 2.6-4.6
  units out. A flat picture shows only their angular size; two eyes see the distance. The
  world was not reported as wrong, so `units_per_metre` stays 1 and those layers are scaled
  towards the eye instead: same angular size, half the distance and half the size
  (`foreground_scale 0.5`). They are recognised by the depth band Prime confines them to,
  below.
- **The planet stood in front of nearer things.** Prime gives each layer its own slice of
  the depth buffer through the viewport's z range (`CGraphics::SetDepthRange`); the bands seen
  in one frame of play are sky 0.999-1 (10 draws), world 0.125-1 (740), layers at 1/32-1/8
  (10) and 1/512-1/64 (54) not yet identified, and 0-1/512 (181), which holds the visor
  frame, the arm cannon and the HUD. In the intro cinematic a 1/64-1/32 band appears as well.
  The eye path
  ignored the viewport's z, so the sky, modelled about 58 units out, was depth-tested there
  and hid everything further away. The eye now honours the bands, and draws the sky at
  infinity so its disparity agrees. See "Depth bands in an eye" in
  `gcn-recomp/docs/graphics.md`.
- **No D-pad**, which the visors need: holding the left grip turns the left thumbstick into
  one.

## Performance (2026-10-08)

The intro cinematic was choppy, with the sound breaking up. Measured with `prime_bench`
replaying `routes/new-game` on the headset (`GCN_REPLAY`), unpaced: the heaviest stretch of
the cinematic ran at 38-50 fps on the CPU alone -- some 115,000 vertices and 21,000 draws a
frame -- so the guest fell behind real time, and under the virtual clock that is slow motion
and starved audio rather than dropped frames.

`simpleperf` cannot record on a Quest (the shell is refused perf events), so
`gcn-recomp/runtime/host_profile.cpp` samples with a profiling timer instead (`GCN_PROFILE`).
Over the cinematic, all of it on the guest thread: the GX front end's transform and lighting
about 35%, the recompiled game's code with its loads and stores most of the rest, and the
paired-single quantised loads and stores about 10%. Two changes, both with byte-identical
frames against the build before them (13 frames from 4000 to 10000 compared with `cmp`):

- The vertex transform moved off the guest thread: draws are decoded as they arrive and
  transformed after the frame is submitted, on two worker threads, from snapshots of the XF
  state each draw saw.
- The float paired-single formats are handled inline in the generated code, and the
  integer ones look their scale up instead of calling `ldexpf`.

That left the heaviest stretch at 57-68 fps unpaced, only just enough. A second round the
same day, measured the same way (`GCN_REPLAY` through the intro, the 28-46 s window of the
benchmark, mean and minimum frames a second; the same build varies by about 3 fps between
runs, so each was run at least twice):

| Change | Mean | Minimum |
|---|---|---|
| after the first round | 61.8 | 55.9 |
| vertex decoding by format-specialised readers; matrix bookkeeping cached | 62.1 | 55.9 |
| XF snapshots copied a sixteen-word block at a time instead of a region | 62.7 | 55.9 |
| `-march=armv8.2-a -mtune=cortex-a78c` | 63.1 | 57.9 |
| stores to the write-gather pipe appended inline, not through the MMIO decode | 65.4 | 61.0 |
| prefetching each draw's array elements a few vertices ahead | 61.4 | 57.8 |
| (prefetching taken out again) | | |
| **the GX front end on a thread of its own** | **102-104** | **68-70** |

All with frames byte-identical to the build before them. The first three barely moved the
number because the decode is bound by memory latency -- the hottest lines were the loads
from the game's vertex arrays, scattered through RAM -- and the core already overlapped
those misses: asking for them early only added work. What was left on the guest thread was
about half GX front end and half the game's own code (Retro's CPU skinning,
`PSMTXROMultS16VecArrayGathered`, stores every vertex through the gather pipe, which is what
the inline store helps). Moving the front end to its own thread, with the guest keeping the
frame protocol (see "Threads" in `gcn-recomp/docs/graphics.md`), halved the guest thread's
work; the four threads now share a frame about evenly.

## Still to look at

- **A short stretch of stereo before the intro cinematic.** On the `new-game` route the hook
  answers stereo from frame 2280 (the camera manager appearing, first-person camera current)
  to 2721 (the first cinematic camera). What is on screen then has not been looked at.

- **The pause and map screens** keep the first-person camera current; whether they want
  theater, and what marks them, is open.
- **The HUD** is drawn as geometry hanging in front of the camera (the visor frame, the
  energy bar, the radar). Which draws those are is readable from their position matrix, as
  with Blue Storm's countdown rig (`PixelState::view_space`); how the renderer's HUD frame
  treats them is the first thing to look at in the headset.
- **Comfort.** The game turns the camera with the stick and pitches it with free aim, on
  top of the viewer's own head.
- **Visor effects** (scan, thermal, X-ray) and the EFB copies behind them are the
  renderer's hard part, as the water was for Blue Storm.
