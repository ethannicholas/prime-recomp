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
  `gcn_add_game(... ANDROID_PACKAGE com.ethannicholas.prime)` builds `libprime.so` (the app) and
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
(`/sdcard/Android/data/com.ethannicholas.prime/files/game.ciso`, then under `com.example.prime`),
unpaced under the virtual clock:
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

## The theater panel as a stereo pair (2026-10-09)

The menus, the map, the ball and the cinematics are drawn by the game as 3D scenes, so
theater shows them like a 3D film rather than a photograph: `theater_stereo 1` in Prime's
defaults has the flat frame drawn twice a game frame, each perspective draw seen from half
the viewer's eye separation to one side of the game's camera, and the two images hung on
the same panel as left- and right-eye quad layers. The mechanism is in
`gcn-recomp/docs/graphics.md` ("The theater panel as a stereo pair"): the panel is a
window, so what the game's frustum shows as wide as the panel sits on it, nearer things
stand in front, and a point at infinity has the eyes' full separation. Not yet seen in the
headset; checked on the desktop (`GCN_THEATER_STEREO=0.064`, dumps in `_l`/`_r` pairs,
the shift between them measured per block of the frame, in pixels of 1280, where the
eyes' separation on the 3.2 m panel is 25.6):

- **Intro cinematic** (frame 5000): the planet and the stars at 25, the ship's hull
  sweeping past at -14 to -23, in front of the panel. As a film would have it.
- **Play** (frame 10000, which the headset shows in stereo, so this is the manual
  override): the hangar at 21-25, the arm cannon at -40 and beyond, well in front.
- **Pause screen** (10750): the frame and the lists at 22-23, Samus at 9-11. The draw log
  says why: the GUI is modelled 17-21 units out, Samus 3.2-3.9, all in the full depth band.
  So the frame reads far and large, with Samus standing in front of it; whether that is
  right is for the headset, and `theater_depth` (below) is the dial.
- **Map** (11750): the map's own frame, legend and text at 0, on the panel; the rooms at
  23-24, behind it. The game draws the map's GUI in its HUD band (0 to 1/512) and the rooms
  with no band at all, so `panel_band 1/512` in the defaults puts the HUD layer on the
  panel, like subtitles, and leaves the rooms their depth. The pause screen's GUI is not in
  that band, so it keeps the game's own depth.

`theater_depth` scales the separation (1 is true to the game's scale, less flattens towards
the panel), for judging in the headset. What the first session should look at: whether the
pause frame's distance and size feel right, and the cost -- a second flat pass per frame,
on the Quest about 1.4 ms more of render thread in the cinematic (`prime_egl` with
`GCN_THEATER_STEREO=0.064` measures it).

## Culling to the eyes (2026-10-09)

Prime culls against its camera's frustum, tightly, so in the headset turning the head even
slightly showed things missing at the edges: actors, particles and the area's own geometry.
In stereo the game now culls to what the eyes see instead.

- **One constructor.** Every frustum the frame is culled with is built by
  `CFrustumPlanes::CFrustumPlanes(xf, fov, aspect, near, bool, far)` (0x80345CEC): in
  `CStateManager::SetupViewForDraw` (what `DrawWorld` hands each actor's `AddToRenderer`,
  and the renderer's clipping planes for the area octree), `PreRender` (each actor's
  `PreRender`) and `ResetViewAfterDraw`. Patches at those three calls
  (`recomp/patches.txt`) run the constructor and then `cull_to_eyes` (`src/vr_prime.cpp`),
  which rebuilds each plane slot for slot: the frustum's count (5, or 6 with a far plane)
  and the order of its planes stay the game's. Its other callers -- a light's view in
  `CWorldShadow`, the orbit and targeting markers, the scan indicators -- are left alone;
  the scan indicators may want the same treatment once the scan visor is looked at.
  Position-based visibility (the area PVS, which areas are loaded and open) does not
  depend on where the camera looks and needs nothing.
- **The frontend publishes the eyes** (`vr::eye_views`, beside `vr::hand_pose`): each eye's
  pose in the same frame as the controllers and the tangents of its field's edges, while
  the eyes are drawn. `prime_egl` publishes its own, turned by `--eye-yaw`/`--eye-pitch`.
- **One frustum for both eyes**: the left eye's orientation, the union of both fields
  (each eye's corner rays turned into it), its apex between the eyes and every plane moved
  out by half their separation, which contains each eye's frustum. The far plane keeps the
  game's distance, measured from the eyes; the near plane passes through them.
- **The margin.** The guest culls a frame or two before it is shown, against the
  eyes last published, and the head turns in between: each side is widened by
  `cull_margin_deg` (vr.txt, default 10, judged nowhere yet; at 200 degrees a second, a
  quick turn, 10 degrees is 50 ms). `cull_to_eyes 0` turns it all off.
- **The planes point out.** `CFrustumPlanes` is a count, then planes of (n, d) with a point
  *outside* when n.p >= d (`PointInFrustumPlanes`). The first build read that test the other
  way round, took the normals for inward ones and wrote every plane inside out: in the
  headset most of the world went missing. `GCN_CULLLOG=1` on `prime_egl` showed it at once
  -- the camera facing -y, the game's first plane with its normal along +y and passing just
  by the camera, which only an outward near plane does. On the frigate `SetupViewForDraw`
  and `PreRender` build five planes, the near plane first, then the sides; no far plane.
  Checked on the headset with `prime_egl --eye --eyes=2` at `--eye-yaw=0` and `45`, frame
  10500 of the new-game route: the hangar whole straight ahead, and at 45 degrees the deck
  and the far structures out to the edge of the eye.
- **On the desktop**, `GCN_CULL_HEAD="yaw pitch"` (degrees left and up) stands in for a
  head with a Quest-like field, so the flat picture shows the world culled to where that
  head looks; `GCN_CULLLOG=1` prints, every 600 frustums, the camera and each plane as the game built it
  and as rebuilt (both inward), and the field culled to.

## The Chozo Ruins (2026-10-09)

Walking back into the main plaza of the Chozo Ruins was very rough in the headset, before
the culling change as well as after. The run's input log (the headset's
`inputs/20261009-104850`, 6,200 frames from the save) replays on the device tools; copied
to `/data/local/tmp/prime/chozo` and run as `GCN_REPLAY=chozo`. Measured with
`prime_egl --eye --eyes=2 --eye-size=2352x2464 --msaa=4 --fast`, `GCN_EYE_GPU=1` and
`GCN_FRAMETIME=1`, matched frame by frame. The heaviest stretch is frames 2400-3600, about
800 draws and 70,000-100,000 vertices a frame.

- **The render thread was 10-12 ms a stereo frame**, and 5-7 ms of it was the vertex upload,
  `glBufferData` of twelve megabytes: each vertex had room for eight texture coordinates,
  and Prime's use about two (in this room 0-2 for 90% of vertices, never more than 5). The
  renderer now uploads only what each draw uses (see `gcn-recomp/docs/graphics.md`), and
  the upload is about 0.5 ms; the render thread about 4-5 ms. Frames byte-identical.
- **The GPU is 10-12 ms** a frame for both eyes, and did not move with the vertices: it is
  not vertex fetch. With the compositor that is close to the 13.9 ms of a 72 Hz frame.
  `eye_scale` is the lever there.
- **The pace of the pipeline, unpaced, is 12-14 ms a frame here**, and did not move with
  the render thread either: it is the guest thread and the front end, the game's own work.
  `prime_bench` (no renderer) manages 74-98 fps over the same frames.
- **Culling to the eyes costs little**: 5-10% more draws and about 0.1-0.4 ms more of eye
  time against `cull_to_eyes 0`.

**The averages hid it.** That first look replayed a run that only walked through, and it
read the GPU from the eye timers' averages; the viewer saw a single-digit slideshow there.
A second run (`inputs/20261009-114624`, spinning around in the plaza) is far heavier --
up to 110,000 vertices, 16,000 GX draws and 1,500 draw calls an eye a frame -- and on it
the GPU is the limit: `gpu_busy_percentage` sat at 99% at the GPU's top clock (690 MHz)
through the heavy stretch, and the eyes took 14-18 ms a frame, more than a 72 Hz frame has
before the compositor takes its share. The app had also never asked for performance
levels, and the compositor's `VrApi FPS=` lines showed it at the runtime's default levels,
the GPU at 492-640 MHz. Two things not reproduced by the harness: the head (the input log
carries the pad, not the pose, so the harness holds the head still) and those clocks.

The GPU's time over frames 3600-4800 of that run, at 690 MHz, by eye size (multiples of the
runtime's 1680x1760) and MSAA:

| Eye scale | 4x MSAA | 2x MSAA | none |
|---|---|---|---|
| 1.4 | 16.5 ms | 10.9 ms | 8.3 ms |
| 1.3 | 13.1 ms | | |
| 1.2 | 12.0 ms | 8.0 ms | |
| 1.1 | 10.2 ms | | |
| 1.0 | 8.7 ms | | |
| 0.35 | | | 8.0 ms |

About 8 ms is the geometry, whatever the size: a tiled GPU bins every draw, and each eye's
1,500 draws are replayed for each tile. Multisampling at size is the rest: more samples
mean more tiles to replay them into. So Prime now defaults to `eye_scale 1.2` and `msaa 2`
(both still in vr.txt), and the frontend asks for the boost levels (`perf_boost`, default
on). The geometry floor is what multiview would halve.

The app now logs every second (`adb logcat -s prime`): game frames, the longest wait
between them, and the render thread's time per stereo frame; read them beside the
compositor's `VrApi FPS=` lines. Raise the log buffer first (`adb logcat -G 64M`) or a
session scrolls out of it.

The flat pass looked like a second culprit at first, 5-8 ms a frame. It was not: its
`[eye-rt]` time includes the batch's texture and vertex uploads, and `GCN_EYELOG=1` showed
the trim leaving out all 852 of the scene's draws in a frame of the plaza.

## The second room (2026-10-09)

The next room past the save station (`inputs/20261009-122345`, a few seconds in it) gave
40-48 game frames a second in the headset. This time the app's own log said so (`compositor
.. game .. frames`, one line a second) and the compositor's `VrApi FPS=` lines beside it
said why not the GPU: `GPU%` 0.35-0.41 at 545 MHz. (The boost levels were granted, `cpu 0,
gpu 0`, and still showed GPU level 4.) It is the CPU.

The room is about 20,000 GX draws and 115,000 vertices a frame, nearly all 4-8-vertex fans
and strips out of the room's display lists. `prime_bench` alone, with no renderer, managed
50-58 fps there, so the guest pipeline itself was short of 60. `GCN_STALLS=1` (new) showed
where the guest's time went: 4.5 ms of every frame waiting at draw-sync token reads.

- **The tokens are the skinned models'.** `CSkinnedModel::PostDrawFunc` sets one after
  each skinned model; `Skinning::AddSkinnedRef`, `CSkinnedModel::EnsureAllocation` and
  `TickAllocations` read it back to recycle the skinning buffer -- two reads per model,
  about 80 a frame here, against about one a frame on the frigate. Each read waited for
  the front end to decode everything drawn before it, and the creatures are drawn after
  the world, so each waited behind the world's 20,000 fans.
- **What was changed**, all in gcn-recomp's front end (`docs/graphics.md`, "Threads"): the
  wait moved from the token's issue to its read; the guest hands commands over 4 KB at a
  time instead of 64 KB; the threads wake each other only when asleep; the transform keeps
  a draw's plan while the registers are unchanged; and Prime turns on draw-sync lag
  (`src/gx_prime.cpp`), which shows the game a GPU a frame behind. With the lag no token
  read waits at all, and no read spun (`token reads .. lagged .. spun 0`).

The stereo harness at the headset's settings (`prime_egl --eye --eyes=2
--eye-size=2016x2112 --msaa=2 --fast`, top clocks), frames 2400-3200 of the run:

| Build | Frame interval | Token waits |
|---|---|---|
| before | 21-24 ms | 5-8 ms |
| token read lazy, 4 KB hand-off | 17.7-19 ms | 4.5 ms |
| + plan cache, fewer wake-ups | 17.7 ms | 4.5 ms |
| + draw-sync lag | 11.2-11.6 ms | 0.01 ms |

`prime_bench` over the same stretch went from 55 to 73 fps before the lag. Frames of this
run are byte-identical throughout. Two frames of the earlier Chozo run are not: a few
hundred pixels on the HUD map and the edges of the visor and the cannon moved, by up to 61
of 255. Both runs of each build agree, and with the front end inline (`GCN_GX_SYNC=1`) the
new build matches the old exactly, so it is the front end reading guest memory the game
rewrites each frame at a different moment, not a wrong result. Those are the same pixels as
the HUD map's shimmer, below.

What is left in the room is the game's own work, about 13 ms of the guest thread a frame,
much of it CPU skinning (`fn_80355298`, `fn_803553B4`, paired-single loads), and the
front end's 8 ms of decoding, now mostly in parallel with it.

## The HUD map's shimmer (2026-10-09)

The map at the top right of the HUD shimmered in the headset, parts of it dropping out as if
on the edge of a depth test. They were. The map is a perspective model about 15-18 units out
in view space (draws 418 on, frame 2400 of the Chozo run, `GCN_DRAWLOG` with
`GCN_EYE_FULLFLAT=1`), in the 0-1/512 band with the visor frame and the rest of the HUD. An
eye mapped its own depth into that band, and with the eye's near plane at 0.05 the 24-bit
buffer could only tell surfaces apart about 0.18 units apart at that distance; the map's
faces and the outlines drawn on them are far closer. Six consecutive frames showed it
breaking into stripes in two. The renderer now writes the game's own depth inside a
foreground layer (`docs/graphics.md`, "Depth bands in an eye"), and the same six frames
are solid; the rest of the frame, visor and cannon included, does not change.

## The map screen (2026-10-09)

The map screen (Z, the right grip) ran at 41 fps in the headset, the compositor's own rate
included: `VrApi FPS=41/72`, `GPU%` 0.98, the app's GPU time 20-25 ms a frame. It is shown
in theater, as a stereo pair, at `theater_scale` 3. Replayed on the headset
(`inputs/20261009-121114`, map open from frame 7651 to 8170; `prime_egl --scale=3` with
`GCN_THEATER_STEREO=0.064`): 10,000 GX draws and 7,046 state applications a pass against
470 in play, and the render thread 75% inside the Adreno driver. `GCN_APPLYSTATS=1` showed
what changed between draws: the cull mode 5,633 times, the TEV colours 2,734, the point size
2,646, the program 16. The map draws each room's translucent box and its outlines in turn,
and the renderer turned culling off for every line and back on for the next face. It no
longer touches culling for lines or points, nor the point size for anything but points
(`docs/graphics.md`); frames are byte-identical.

| Map screen, pair | Render thread a frame |
|---|---|
| 3x, before | 21.2 ms |
| 3x, now | 14.7 ms |
| 2x, now | 11.7 ms |

At 3x the GPU is still 99% busy over the map in the harness (top clock, unpaced), so in the
headset it should be about 60 rather than 41, with little margin; `theater_scale 2` in
`vr.txt` buys the margin at the cost of the panel's supersampling. What is left is the
draw calls themselves -- 7,000 a pass, each with its own colours -- and the pair doubles
them.

**The map's lag.** Turning the map, its sound started at once and the picture half a second
to a second later. The sound follows the guest; the picture was behind it by every frame
queued between the guest and the screen, and with the renderer the slow stage every queue
was full: eight frames in the render queue, two in the transform's, and the front end's
bounded only at 8 MB, dozens of map frames. The queues now hold one frame each
(`docs/graphics.md`, "Threads"): `GCN_STALLS` shows the guest 3 frames ahead of the screen
in the map screen at worst (6 with two each; frames-in-flight was not measured before), at
the same frame rate, and frames are unchanged.

## Loading, the acid's fog, and black HUD text (2026-10-09)

- **"Not responding" at start.** After a change to the shaders the app rebuilt all 6,000
  cached programs before starting the game -- 25 s -- on its main thread, answering neither
  Android nor the runtime: the runtime's waiting room, then Android's dialog. The cache is
  now built in slices between frames with a progress bar on the panel
  (`gcn-recomp/android/openxr_main.cpp`, `build_shaders_responsively`).
- **Static on the acid.** Yellow lines across the water in the Chozo Ruins' acid rooms, and
  specks along the waterline, in the flat view as well as the eyes. Not the water surface
  (`GCN_DRAW_SKIP` of it left them) but a fog volume after it: two depth copies of the lower
  half of the screen, read as IA8 indirect textures to index a ramp. The copies were in the
  wrong layout for that; fixed in gcn-recomp (`docs/graphics.md`, "Texture lifetimes and
  EFB copies"). At internal scale 2 the frame is clean; at 1, which stereo uses for its
  copies, faint lines remained every few dozen rows, and so did the specks.

  **Every few dozen rows is every chunk** (2026-10-09, from the code, not yet from the
  headset): the fog volume is drawn in horizontal chunks of the screen
  (`CCubeRenderer::DrawFogVolume`), each depth-copied and read back by a quad whose
  texture coordinates run from the first texel's centre to the last's over exactly the
  chunk's pixels. At internal scale 1 the last row's coordinate sits within 1/256 of a
  texel edge, where a GPU's subtexel rounding can read the next texel -- a different depth
  byte, and a wrong ramp index along that row. The renderer now snaps a depth copy's
  coordinate to the centre of the texel GX would read (`u_texsnap` in
  `gcn-recomp/runtime/gx/shadergen.cpp`), which has the frigate's frames unchanged; the acid
  rooms are not reachable on this machine (the Chozo input logs are on the headset), so the
  next run there should look. `stereo_scale 2` in `vr.txt` is the other lever, since the
  copies were clean at scale 2.
- **Black HUD text.** The energy digits, the warning ("Damage") and the map's room title
  became black rectangles as a session went on. Reproduced by replaying
  `inputs/20261009-140323` in stereo: the room title from frame 4200 and every "Damage" from
  5700. The renderer had deleted textures the front end still held as sent; the front end
  now decides alone when a texture dies, and the same replay draws all of them.

## The beetle room again: the clocks, the copies, the transform (2026-10-09)

A minute in the second room (`inputs/20261009-151339`, on the device as
`/data/local/tmp/prime/s5`) ran at 46-53 game frames a second with the sound breaking
up. Three things learned from the session's logcat before replaying anything:

- **The GPU is not the limit there**: the compositor's `VrApi FPS=` lines had `GPU%`
  0.57-0.69 and `CPU%` 0.95.
- **The CPU boost is on a timer.** The app asks for `XR_PERF_SETTINGS_LEVEL_BOOST`, and
  the OS's clock governor grants CPU level 8, 2361 MHz -- for 47 seconds. Then
  (`adb logcat -s crcs`): `Clock levels changed: CPU 8 -> 4 (CPU boost ran for too long
  or was requested too frequently. The boost must be disabled by the app.)`, and the
  session ran its remaining twenty seconds at 1920 MHz. The GPU's boost request is
  answered "based on Utilization" and never granted: 545 MHz throughout. So the steady
  state is 1920/545 MHz, and the headset's own log is the only honest reading of it.
- **The harness runs faster than the app.** `prime_egl` over `adb shell` runs the big
  cores at 2361 MHz and the GPU at 690 MHz under load (`scaling_cur_freq`, `gpuclk`).
  Every harness figure below is at those clocks; the app's CPU side is about 1.23 times
  slower, its GPU 1.27. The app logs the perf-settings notices now, beside the
  compositor's lines.

The replay in the harness (`--eye --eyes=2 --eye-size=2016x2112 --msaa=2 --fast`,
`GCN_FRAMETIME=1 GCN_STALLS=1 GCN_EYE_GPU=1`), frames 2200-3150, about 20,000 GX draws
and 115,000 vertices a frame, mean milliseconds per frame:

| Stage | Before | Texgen matrices cached | + copies keep their ids |
|---|---|---|---|
| guest thread | 12.2 | 11.7 | 11.5 |
| front end | 8.1 | 8.0 | 8.0 |
| transform (two workers, wall) | 7.2 | 6.5 | 6.5 |
| flat pass (render thread) | 5.1 | 4.6 | 2.9 |
| each eye (render thread) | 2.9 | 2.9 | 2.9 |
| render thread, stereo frame | 11.0 | 10.4 | 8.6 |

Frames byte-identical throughout. The GPU is 8.5-9.7 ms a stereo frame at 690 MHz. The
profile (`GCN_PROFILE`, by thread): the two transform workers 34% of all CPU time
(`transform_vertex` 25% on its own), the front end 24% (`draw_impl`, the vertex decode),
the render thread 23% (58% of it inside the Adreno driver), the guest 19% and flat --
`PSMTXROMultS16VecArrayGathered` and its stores about 12% of the guest thread, the
gather-pipe-to-parser path about the same, the rest the game's own code spread thin.

- **The flat pass was mostly copies.** This room makes four *depth* copies of the lower
  part of the screen each frame (the acid's fog volume), the first after the whole world
  has been drawn, so the trim can only leave out the 645 draws after the last of them.
  But the 4.6 ms was not those 800 draws: the game copies three targets of different
  sizes through one scratch buffer every frame (`GCN_COPYLOG`: a 128x128 reflection, a
  320x76 depth copy, a 4x76 sliver, all to `0047AD40`, and two more to `0049DD40`), the
  texture cache keyed copies by address, so each replaced the last and every one was new
  the next frame -- five GL textures allocated and freed a frame. Fixed in gcn-recomp
  (`docs/graphics.md`, "A copy keeps its id"); the copies are 0.2 ms of the pass now
  (`[rt]` prints their share), and the 800 draws the rest, at the same 2.5 µs a call as
  the eyes'.
- **What the transform is asked for** (`GCN_XFSTATS=1`, new): 115k vertices, 95% with a
  normal, 30% lit by 2.5 lights on average, 1.9 texgens per vertex and every texgen
  post-transformed. The texgen and post matrices were read through the snapshot's page
  table per vertex; cached per draw now, 9% off the transform. What is left is the
  arithmetic itself, at about 100 ns a vertex over two threads.

Where that leaves the room at the headset's 1920 MHz, scaling: the guest about 14 ms,
the render thread about 10.5, the front end 10, each transform worker 8 -- against a
16.7 ms frame, on four fast cores that also run the compositor. The guest sets the pace
and has no hot spot; the render thread is 3,500 draw calls a stereo frame and the eyes
are two thirds of them.

Next, in order:

1. **Multiview** (`GL_OVR_multiview2` with `GL_OVR_multiview_multisampled_render_to_texture`):
   both eyes from one set of draw calls, which takes an eye pass (2.9 ms here) off the
   render thread and the second eye's binning off the GPU. The vertex shader's `u_proj`,
   `u_view` and `u_crop` become pairs indexed by `gl_ViewID_OVR`, the eye target a
   two-layer array, and the app's eye swapchain one with `arraySize 2`; the shader cache
   rebuilds once. The eye grabs (a visor effect's whole-frame copy) need the per-eye
   path, so a frame that grabs falls back to it.
2. ~~The clock when the guest is slow.~~ Done the same day, in gcn-recomp ("The clock"
   in its `docs/diagnostics.md`). Under the virtual clock a guest that cannot keep real
   time ran the game in slow motion and starved the sound, which is what the dropouts
   were; the hardware would drop frames instead, its DSP keeping real time and the
   game's own `UpdateTicks` taking a longer step. Now virtual time jumps to the host's
   once it is 4 ms behind, each jump written to the input log at its back-edge count,
   and a replay makes exactly those jumps and none of its own. Checked on the headset
   with the harness: the session's log replays byte-identically as before; a run paced
   at twice real time (`GCN_TIMESCALE=2`, so the host falls behind in this room) made
   811 jumps, replaying its log (`GCN_INPUT_LOG` records one) reproduces its frames
   exactly, and the original log over the same frames does not. Logs are version 3
   (`jump` lines); older ones replay as they did. Not yet heard in the headset: what to
   listen for is that a heavy room drops frames with the sound whole, rather than slowing
   down and crackling.
3. The guest's skinning store path (`PSMTXROMultS16VecArrayGathered`, a bit-exact HLE
   in `src/`) and the gather-pipe path, each about a tenth of the guest thread.

## The scan visor follows the head (2026-10-09)

The scan visor was the one thing left that needed the free look: its window (the magnified
rectangle) and its target zone sat at the centre of the game's camera, so in the headset
an object could only be scanned by turning the camera onto it with R. Now, in stereo, both
follow the head. The mechanism is written out in `src/vr_prime.cpp` ("The scan visor follows
the head"); in short:

- **The game projects the head's direction itself.** The eyes' mean forward direction, in
  the camera's frame, goes through the first-person camera's own perspective (vertical
  field of view at +0x16C of `CGameCamera`, aspect at +0x178) to a pixel of the game's
  screen. That one point is written each frame into the tweaks the scan zone is read from
  -- `CTweakPlayer`'s box centre and ideal point for zone 1, which is what the Wii version
  does with its pointer -- so `CPlayer::FindOrbitableObjects` finds what the head is on, and
  the scan indicators and the lock follow. The zone's size is the game's: 252 by 88 pixels.
- **The window is moved by two patches in `CPlayerVisor::DrawScanEffect`**: the EFB copy it
  magnifies is taken around the point instead of the viewport's centre, and the ortho
  projection the window and its frame are drawn with is set again shifted by the point's
  offset, right after the game sets it. The point is clamped so the window stays inside the
  screen (and so inside the visor): looking past the edge pins it there.
- **Locking on no longer turns the body or the camera in the scan visor.** The game turns
  both to face an orbit target (`CPlayer::UpdateOrbitOrientation` snaps the body's yaw;
  `CFirstPersonCamera::UpdateTransform` looks at the orbit point), which in the headset
  would move the world under a viewer who is already facing the target, and take the zone,
  which follows the head, off it. While the scan follows the head, the four reads of the
  orbit state in those two functions are answered "no orbit" in the scan visor; the state
  itself, which the scanning checks, is untouched. Circling a target with L and the stick
  in the scan visor still moves the body; it just does not turn it.
- **Only while the eyes are published** (stereo), so theater and the desktop keep the game's
  own scan; `scan_follows_head 0` in `vr.txt` turns it off.

Checked on the desktop with the stand-in head (`GCN_CULL_HEAD`, which the scan reads too)
and scripted presses on the new-game route, frames dumped around 10500 in the hangar:

```
GCN_CULL_HEAD="20 8" GCN_STEREOLOG=1 GCN_SCANLOG=1 GCN_INPUT="10350:LEFT:10" \
    GCN_DUMP_RANGE=10300-10800 ./build/prime --replay=routes/new-game --no-input-log \
    --hidden --fast --dump-dir=frames --dump-every=50
```

With the head 20 degrees left and 8 up, the window and its magnified contents sat at
(163, 288) of the 640x448 screen, where the log put the zone, and the scan indicators stayed
on their objects; without `GCN_CULL_HEAD` the frame is the game's own, window and zone at
the centre. With the head 3 degrees right, L held on the console light at the centre of the
hangar (`GCN_INPUT="10350:LEFT:10,10450:L:200"`), the scan ran to "Scan complete" with the
window on the light, and the camera's forward vector in the log settled on the body's
heading, where the game's own scan turned it onto the light instead (the light sits a
degree to the right of centre; both logs show the turn, or its absence, over the 50 frames
after the press).

Not yet seen in the headset. What to look at there: whether the window keeps up with the
head (it is placed a frame or two behind, like the cannon); whether its magnified copy is
sharp enough -- the copy is taken from the flat pass at `stereo_scale`, 1 by default, so a
170-pixel window holds about 100 source pixels; `stereo_scale 2` doubles that at the cost
of the flat pass, which in the scan visor draws the whole world for the copy; and whether
pinning the window at the edge of the visor reads right. The data dots that fly from the
window to the scan panes still start from the screen's centre.

## The controllers, Prime's way (2026-10-09)

`vr::GameHooks::map_pad` (new in `gcn-recomp/runtime/vr_game.h`) lets a game rearrange the
pad the headset frontend built from the Touch controllers, before the game reads it and
before the input log records it, so replays carry the result. Prime's (`map_pad` in
`src/vr_prime.cpp`):

- The left controller's lower button (X) fires missiles and its upper one (Y) morphs, the
  other way round from the frontend's X-to-X mapping.
- The right trigger is a second A -- firing, with the cannon in that hand -- except on the
  pause and map screens, where it stays R for the pause screen's tabs and the map's zoom.
  Those screens are when the game is in play but not drawing its world (`prime_world_drawn`,
  the same answer the view decision reads), so the rule needs no new state. The free look
  that R gave is no longer needed now that the scan visor follows the head.

## The package (2026-10-09)

The app's package is `com.ethannicholas.prime` (it was `com.example.prime`, which showed
under Unknown Sources). A package is an app to Android, so the new one installs beside
the old and starts with an empty data directory: the disc image, the memory card, `vr.txt`
and the shader cache have to be copied across once, then the old app removed:

```
adb shell cp -r /sdcard/Android/data/com.example.prime/files/. /sdcard/Android/data/com.ethannicholas.prime/files/
adb uninstall com.example.prime
```

(`package-apk.ps1 -Install` pushes the disc image on its own, but not the card.)

## The helmet, the HUD's distance, the scan window, the charge shot's warp (2026-10-09)

Four things from a headset session, each replayed in the harness (`inputs/20261009-165000`,
a couple of charged shots; `20261009-163518`, fifteen minutes with the scan visor and the
acid), and the renderer given what it needed in gcn-recomp (`docs/graphics.md`, "Depth
bands in an eye": a HUD layer and a draw filter for the eyes).

- **The helmet is left out of the eyes.** Prime draws the inside of Samus's helmet as
  geometry in the HUD's band: the dark arc across the top with its three blue lamps, the
  brackets at the bottom corners, and the struts with the yellow lamps either side. On a
  television it is the picture's edge; in a headset it hung in the middle of the view. Its
  draws are known by their textures -- the helmet model's materials, draws 419-439 of frame
  2697 of the charge-shot session, textures 1881-1891 (`GCN_DRAWLOG`, then `GCN_TEXDUMP`
  to look at them) -- named by content hash, which is the same in every run where an id is
  not (`kHelmetTextures` in `src/vr_prime.cpp`; `GCN_TEXLOG` prints the hashes). The arc
  and the corner brackets go; `hide_helmet 0` in `vr.txt` keeps them. The side struts with
  the lamps are not in that list yet: they were not asked about.
- **The HUD at arm's length.** The HUD is modelled 16-21 units out in view space and at
  the foreground's half scale stood eight to ten metres off, a billboard. It has a scale of
  its own now (`hud_band 0.02`, `hud_band_scale 0.1` in Prime's defaults, both in
  `vr.txt`): a tenth of its distance, 1.6-2.1 m, the same angular size, the cannon where it
  was. Smaller is nearer. The first attempt put the band at 1/512 and moved only the map
  and two scraps: the HUD proper -- the energy bar, the radar, the selectors, the visor's
  outline (draws 96-154 of frame 2697, textures 1864-1879: the digits, the font, the
  radar, the icons) -- is in a band of its own, 1/512 to 1/64, and only the map and the
  scraps are below 1/512. Found by rendering the frame with the head 60 cm to the side
  (`--eye-pos`, new): what is near moves, and the HUD had not; then the eye's draw log
  (`GCN_DRAWLOG` now names the layer the eye gave each draw) said which band it was in. A
  single viewpoint cannot show the change at all, since the scale is about the camera.
- **The scan window sat above where the head looked**, its bottom edge on the gaze. The
  window is an orthographic draw, so the eyes paint it on the HUD frame, which is
  `hud_scale` of the *headset's* vertical field tall; the window had been placed through
  the game camera's 55-degree projection, and the two agree only when the frame happens to
  match the camera. Now the window's point is found through the frame itself (its distance,
  scale, height and pitch from `vr.txt`, the field from the eyes' tangents, the same way the
  frontend sizes it -- `head_screen_point(..., true)`), while the scan zone and the window's
  copy keep the camera's projection, since those are about where objects land on the game's
  screen. Checked in the harness with a Quest-like field (`--eye-fov=43,52`, new) and the
  eye pitched 15 degrees down: the previous build's window had its bottom on the gaze, the
  new one is centred on it.

  Two harness findings on the way: `prime_egl` only asked the game's `wants_stereo` hook
  when `GCN_STEREOLOG` was set, and that hook is where the camera manager is found, so the
  scan patches had nothing to work from in the harness (fixed: asked every frame, as the
  app asks); and Android's `grep` has no `\|`, so a device-side search with it finds
  nothing -- use `grep -E`.
- **The charged shot's warp is left out of the eyes.** The game copies a 192x192 square of
  the frame around the shot and draws it back over the same square orthographically through
  an indirect texture (draw 74 of frame 2697: `t7` the copy, `t1` the warp). On the HUD
  frame that was a distorted square of the flat view pasted in the air. There is no depth
  to give it, so the eye filter drops any ortho draw with an indirect stage sampling a copy
  of part of the frame; the scan visor's window is an ortho draw of a partial copy too, but
  has no indirect stage. `hide_flat_warps 0` keeps them.

- **The scan window's copy at twice the texels.** `stereo_scale 2` is now Prime's default:
  the EFB the eyes' copies come from is 1280x896 instead of 640x448, so the window's
  magnified copy has four times the source pixels. Measured in the beetle room
  (`vr.txt` with `stereo_scale 2` beside `prime_egl`, `GCN_EYE_GPU=1`): the GPU 8.1-8.7 ms
  a stereo frame either way, the render thread the same -- the flat pass is still a small
  target. It also thins the acid's lines (below).

**The acid's static, as far as it got.** Frame 23000 of the long session (the ball in the
acid, `GCN_DRAWLOG=23000 GCN_DRAWLOG_VERBOSE=1`): the lines are made by draws 101 and 102,
which sample the depth copy (`t7`, read back as IA8) straight into TEV stages whose alpha
combiner is in *compare* mode (`aenv` bias 3) -- the 16-bit depth from two bytes against
the volume's own -- with no indirect stage at all, so the texel-centre snap for indirect
lookups does not reach them. At internal scale 1 two bright lines cross the pool and dotted
diagonals run up the wall; at scale 2 one faint line. The shader's compare is integer
(`shadergen.cpp`, `mode` 2 and 3), so the next place to look is what reaches it: the
copy's bytes (`z * 16777215 + 0.5` of a float depth, where the hardware's is the
rasteriser's own 24-bit value) and the swap table that routes the copy's I and A into the
compare's G and R.

## Still to look at
- **The HUD** is drawn as geometry hanging in front of the camera (the visor frame, the
  energy bar, the radar). Which draws those are is readable from their position matrix, as
  with Blue Storm's countdown rig (`PixelState::view_space`); how the renderer's HUD frame
  treats them is the first thing to look at in the headset.
- **Comfort.** The game turns the camera with the stick and pitches it with free aim, on
  top of the viewer's own head.
- **Visor effects** (thermal, X-ray) and the EFB copies behind them are the renderer's
  hard part, as the water was for Blue Storm; the scan visor is above.
