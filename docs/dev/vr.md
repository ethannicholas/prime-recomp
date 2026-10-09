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

**The camera is not enough (2026-10-09).** In the headset the view went to stereo on the
world's name shown on black between worlds ("Chozo Ruins"), and stayed there on the pause
and map screens. All three keep the first-person camera current with no cinematic camera:
the pause and map screens draw their GUI over a blurred copy of the last frame, and between
worlds the new world's `CStateManager`, camera manager and cameras already exist while
`CMFGameLoader` draws the transition. What marks them is that the world is not drawn, and
the game's own decision is read rather than guessed at: `CMFGame::Draw` asks
`CInGameGuiManager::GetIsGameDraw` before drawing the world (the pause screen's blur turns
it off once it is up), and `CMFGameLoader::Draw` draws only the transition. Two patches
(`recomp/patches.txt`, 0x80024888 and 0x80023D10) pass that answer to `prime_world_drawn`
(`src/vr_prime.cpp`), and stereo is *first-person camera current, no cinematic camera, world
drawn*. It is a state the guest thread rewrites every frame, not a per-frame event, so the
render thread reading it a frame early or late can at worst move a transition by a frame;
a count of world draws was considered and rejected because the guest runs ahead of the
render thread by a varying fraction of a frame, which would have read as the world missing
a frame.

On the `new-game` route (`GCN_STEREOLOG=1`): the manager appears at frame 2280 with the
world not drawn (the loader), the cinematic count goes to 1 and then the world is drawn
(`CMFGame` taking over, the intro cinematic), and the first stereo frame is 9243, when the
count drops to 0. That also explains the stretch of stereo before the cinematic the earlier
notes had not looked at: it was the loader, and is gone.
Checked on the desktop with scripted presses on top of the route (`GCN_INPUT=
"10500:START:10,11100:START:10,11700:Z:10,12300:Z:10"` with `GCN_STEREOLOG=1` and frame
dumps): theater from the first frame of the pause screen (the game stops drawing the world
as soon as the blur is asked for, not when it is up), stereo again 50 frames after the
unpause press, once the blur has gone; theater on the map two frames after Z, stereo 40
frames after the Z that leaves it, with the map still zooming back into the HUD over the
world for that last stretch. The world's name between worlds is not on this route; it is
the loader, the same case as the frigate's load.

One trap in checking this: run paced. Unpaced (`--fast`) the guest is cheap while the game
is paused and runs about 100 frames ahead of the render thread, so the hook, reading the
guest's current state, said the world was drawn 95 frames before the dumps showed the map
leaving (a draw log of such a frame had only the map's 286 draws, at 2 and 28 units, and no
world). Paced, the answer and the frames agree. On the headset the guest is paced.

## First session in the headset (2026-10-08)

Played to the first room. Three faults, all addressed the same day:

- **The arm cannon read as about twice its size.** Not the eye separation: the cannon is
  modelled about 0.4-1.0 in front of the camera, and the visor frame hangs 2.6-4.6 units
  out. A flat picture shows only their angular size; two eyes see the distance. The world was
  not reported as wrong, so `units_per_metre` stays 1 and those layers are scaled towards the
  eye instead: same angular size, half the distance and half the size
  (`foreground_scale 0.5`). They are recognised by the depth band Prime confines them to,
  below.

  These notes first said the cannon was modelled a unit long, three units ahead and one to
  the right, as draws 4-7. Those draws are part of the visor: when the cannon was moved to
  the controller (below), draws 4-7 stayed put, and the draws that moved were 783-792, in
  the 1/32-1/8 band. The scale was judged by eye in the headset and stands.
- **The planet stood in front of nearer things.** Prime gives each layer its own slice of
  the depth buffer through the viewport's z range (`CGraphics::SetDepthRange`); the bands seen
  in one frame of play are sky 0.999-1 (10 draws), world 0.125-1 (740), the arm cannon at
  1/32-1/8 (10), 1/512-1/64 (54) not yet identified, and 0-1/512 (181), which holds the visor
  frame and the HUD. In the intro cinematic a 1/64-1/32 band appears as well.
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

With the CPU side clear, rendering is what is left, measured with `prime_egl` on the same
route:

- **Theater** (`--scale=3`, the `theater_scale` the app uses for the intro cinematic) holds 60
  fps paced through the whole intro; the render thread's time per frame is 1.4 ms at the
  median, 11 ms at the 99th percentile.
- **Stereo** (`--eye --eyes=2 --eye-size=2352x2464 --msaa=4`, the app's eye targets on a
  Quest 3, with `GCN_EYE_GPU=1`) costs about 13 ms of GPU and 10 ms of render-thread time
  per game frame in first-person play on the frigate, and runs there at about 67 fps
  unpaced. The heaviest stretch of the intro would take 18-20 ms, but the intro is a
  cinematic and is shown in theater. Profiled, 72% of the render thread in stereo is inside
  the Adreno driver: each frame is replayed three times (a flat pass that makes the EFB
  copies the game samples, then each eye), so draw calls and state changes dominate.
  Multiview (`GL_OVR_multiview2`, both eyes from one set of calls) is the lever there if
  heavier rooms turn out not to hold 60; `eye_scale` 1.2 is the cheap one.

## The arm cannon in the right hand (2026-10-09)

In stereo the cannon follows the right controller, and so do the shots. The first build put
it behind the viewer and to the left, though it turned with the controller; the placement
was fixed the same day (below) and checked on the desktop, not yet in the headset.

- **The frontend publishes the controllers.** `vr::hand_pose` (`gcn-recomp/runtime/vr_game.h`)
  gives each controller's aim pose in the eyes' frame, converted the way the eyes are. It is
  published only while the eyes are drawn, so the game falls back to its own aim on the theater
  panel and on the desktop.
- **The game is told, not the renderer.** A patch in `CPlayerGun::Update` (0x800412AC,
  `recomp/patches.txt`) runs `prime_aim_gun` (`src/vr_prime.cpp`) just before the game computes
  `mGunWorldXf = mXf * mGunLocalXf * bob`. The hook sets `mXf`, the gun's base transform that
  `CPlayer` would otherwise aim at the cursor, from the first-person camera and the controller.
  The game's own idle animation, bob and recoil stay on top, and the muzzle moves with the model.
  Moving the drawn model in the renderer would have left the shots going straight ahead.
- **Shots go where the cannon points.** This revision fires along `mAssistAimXf`'s rotation,
  from the muzzle (`UpdateNormalShotCycle`, `FireSecondary`), not along the gun or at the
  cursor as the decompilation's older `FirePrimary` does. The hook sets that rotation too,
  except while locked on, so lock-on still lands.
- **Where it sits.** At rest `mXf` is 0.25 right, 0.30 ahead and 0.35 below the eye, and the
  cannon's body is 0.37-1.00 ahead (draws 783-792 of frame 9600 on the new-game route), its
  centre 0.38 in front of `mXf`. The renderer draws the near bands at `foreground_scale`. So
  `mXf` is put at `(hand + offset) / foreground_scale`, with the offset chosen to put the
  visible cannon's centre 5 cm ahead of the controller's aim point. `gun_x`, `gun_y`, `gun_z` (metres, the
  controller's frame) and `gun_pitch_deg` in `vr.txt` move it from there; `gun_follows_hand 0`
  turns it off.
- **Smoothing.** Placed straight from the controller, the cannon shook with the tracking. The
  hand is averaged over the last `gun_smoothing` game frames (default 3, judged in the
  headset; 1 is off), which costs about a frame of lag.
- **On the desktop**, `GCN_GUN_HAND="x y z yaw pitch"` holds a controller still (with
  `GCN_STEREOLOG=1`, which is what finds the camera there), and `GCN_GUNLOG=1` prints where the
  game itself puts `mXf`. The desktop reads no `vr.txt`, so it uses the defaults the headset
  starts from; the flat picture shows the cannon at twice the distance and size it is seen at.

  The first placement took draws 4-7 for the cannon and put `mXf` 2.94 behind it, which left
  the cannon 1.5 m behind the viewer. The desktop missed it because it then ran without the
  headset's configuration (a scale of 1 and no offset), so it showed only the turn. With the
  defaults applied, a draw log of the stand-in hand showed draws 783-792 behind the camera
  and to the left, as in the headset, and draws 4-7 unmoved.

Expected rough edges, to judge in the headset:

- In the world the gun is at twice the distance it is seen at, so shots start about that far
  out along the line from the eye to the muzzle: about a metre, against the game's own 0.4-1.0.
- The camera's transform is read during the gun's update. If the camera moves later in the
  frame, the cannon lags a frame behind a stick turn, as the game's own does.
- The aim pose on a Touch controller points along the ring, tilted from the grip;
  `gun_pitch_deg` corrects that if holding it feels wrong.
- The grapple arm is placed from `mXf` (`UpdateLeftArmTransform`), so it follows the right
  controller too; the left controller is published but nothing reads it yet.

## The render thread in stereo (2026-10-09)

Done on the Mac, where there is no headset but the stereo path runs the same way (`prime
--replay=routes/new-game --eye --hidden --fast`, one eye at the window's size, with
`GCN_FRAMETIME=1` for the `[rt]`/`[eye-rt]` lines and `GCN_APPLYSTATS=1` for what each pass
sends to GL). Frames 10000-21000 of the route are first-person play on the frigate, about
1,000 draws and 65,000 vertices a frame, and nearly every draw changes state: the pass that
draws them makes about 1,000 state applications, 170 of them program switches.

What a stereo frame cost the render thread before, on that window, mean per frame:

| Pass | Before | Flat pass trimmed | + uniform shadows |
|---|---|---|---|
| flat pass (makes the EFB copies) | 2.93 ms | 0.76 ms | 0.78 ms |
| one eye | 2.31 ms | 2.17 ms | 2.03 ms |

- **The flat pass drew the whole frame to make three 32- and 128-pixel copies.** In stereo
  the flat pass exists only for the EFB copies the eye passes sample (the full-screen ones
  an eye re-grabs from itself, the spray's grabs and the game's own effect textures), and
  in play on the frigate the last copy anything reads is the third of three small clearing
  copies near the start of the frame. The renderer now stops the flat pass after that copy
  (`flat_pass_trim` in `gcn-recomp/runtime/gx/render_gl.cpp`; `GCN_EYE_FULLFLAT=1` draws
  it all again): about 1,000 of 1,010 draws a frame left out, 8 state applications instead
  of 1,000. On the Quest the flat pass was one of the three replays of the frame that put
  72% of the render thread in the driver, so this should take roughly a third of that
  10 ms off; not yet measured there.
- **A program switch re-uploaded every uniform group.** The eye pass's 165 program switches
  each sent the projection, viewport, point size, TEV registers, alpha reference, indirect,
  fog, screen and ripple uniforms whether or not they had changed, about 2,000 uniform
  uploads a pass. Each program now keeps a shadow of what it last received, invalidated as
  a whole when the internal scale changes and for the view-dependent groups when the view
  does (a new eye, the morph, the flat view after an eye); the same pass now sends about
  280. Texture and sampler binds are tracked separately, so a draw that changes texture but
  not sampler rebinds one, not two: 1,016 texture binds and 739 sampler binds a pass where
  there were 1,016 of each, and 600 texture-size uploads where there were 711.
- **The depth buffer is dropped after each eye** on ES (`glInvalidateFramebuffer`, under
  `GCN_GL_ES`; `GCN_EYE_KEEPDEPTH=1` keeps it). Each eye target is 2352x2464 at 4x MSAA
  and nothing reads its depth back, so a tiled GPU need not write it out. Not testable
  on the Mac, and not compile-checked here (no NDK): the first Android build should
  confirm it.

Eye frames from 10000 on are byte-identical across all of these. Two things to know
before trusting a comparison of earlier ones:

- **The boot video and the title screen vary run to run**, with or without the trim and
  with or without `GCN_GX_SYNC=1`: the frames before about 2800 on this route differ
  between any two runs of the same build (different video frames in the window, a
  different blink of the title's text). The belief going in was that the threaded front
  end explained it, since it reads textures when it reaches a draw, and that putting it
  back inline would make the dumps exact; it did not, so the video's timing is somewhere
  else. The intro cinematic and play are exact run to run.
- **The trim is not quite exact through the cinematic** (frames 5250-8700 of the route): a
  handful of pixels a frame, up to 17, off by up to 3 of 255, the same ones every run.
  The eye only ever shows the cinematic on the Mac, since the headset shows it in
  theater, but it means something the full flat pass leaves behind still reaches the eye
  pass. What it is not: the uniform shadows (the apply path before them shows the same
  pixels), the copies (never left out), the texture uploads (done before any draw). Not
  found yet; `GCN_EYE_FULLFLAT=1` is the control.

What is left is mostly the GPU. The 13 ms a frame of GPU time on the Quest is two eyes of
2352x2464 at 4x MSAA, 1.3 times the pixels the panels show, and the render thread's 10 ms
is now expected to be under 7. The levers, in order:

- **`eye_scale`**: 1.4 to 1.2 or 1.1 keeps 4x MSAA and cuts the pixels by 27% or 38%.
  The number to watch is `GCN_EYE_GPU=1` with `prime_egl --eye --eyes=2
  --eye-size=WxH --msaa=4` on the device.
- **Multiview** (`GL_OVR_multiview2`): both eyes from one set of GL calls would halve what
  is left of the render thread, and it is the standard answer on this GPU. It needs the
  per-eye projection and view as a uniform array indexed by `gl_ViewID_OVR` in every
  vertex shader, and the eye targets as one array texture; the shader cache's 3,500
  programs would all change. Cannot be validated on the Mac.
- **CMPR textures as S3TC** instead of decoding to RGBA8, if the Adreno exposes
  `GL_EXT_texture_compression_s3tc` (check `glGetString(GL_EXTENSIONS)` there): a
  quarter of the upload and of the bandwidth per sample, if texture bandwidth turns out
  to matter once the pixels are fewer.

## Leaving stereo is a cut (2026-10-09)

Every exit from stereo is noticed only once the game is already drawing something that is
wrong in stereo: the pause screen's blur, the ball camera, a cinematic camera, the loader's
transition. Folding the world back onto the panel over `transition_s` only showed more of
it, so for Prime the way back snaps: `transition_out_s 0` in the defaults
(`config_defaults` in `src/vr_prime.cpp`; a negative value, the shared default, keeps
`transition_s` both ways). Entering stereo still folds out over `transition_s`.

## Still to look at

- **A stereo panel for the 2D views.** The menus, the map and the morph ball are shown flat
  on the theater panel. They could be shown like a 3D film instead: the game's own framing,
  but each eye's image rendered from a viewpoint half an interpupillary distance to its
  side, so the ball sits in its room at its distance and the map's rooms have depth. The
  vertices reach the renderer in the game camera's view space with each draw's own
  projection, so the change is per draw: translate the view by ∓δ (δ = half the IPD in
  game units) and shift the projection's x-from-z term by ±P00·δ/D so that things D game
  units out have no parallax (D = the panel's distance, so the screen is the window);
  orthographic draws, the 2D elements, are left alone and sit on the screen. Two ways to
  present it: the eye path at morph 0 with that per-eye change in `morph_chain`, through the
  projection layer, which costs two full eye passes; or the flat pass run twice with the
  shear, into two theater swapchains, submitted as two `XrCompositionLayerQuad`s with
  `eyeVisibility` LEFT and RIGHT -- about twice theater's 1.4 ms, and it keeps the quad's
  reprojection, so it is the one to try. Two things to decide in the headset: the near
  layers (the visor frame at 2.6-4.6 units, the ball's HUD) would stand in front of the
  screen, so the foreground band may want pinning to the panel or the parallax clamping to
  the IPD; and the cinematics, which are shown in theater for cost, would pay the second
  pass too.
- **The HUD** is drawn as geometry hanging in front of the camera (the visor frame, the
  energy bar, the radar). Which draws those are is readable from their position matrix, as
  with Blue Storm's countdown rig (`PixelState::view_space`); how the renderer's HUD frame
  treats them is the first thing to look at in the headset.
- **Comfort.** The game turns the camera with the stick and pitches it with free aim, on
  top of the viewer's own head.
- **Visor effects** (scan, thermal, X-ray) and the EFB copies behind them are the
  renderer's hard part, as the water was for Blue Storm.
