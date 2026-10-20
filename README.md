# prime-recomp

A static recompilation of *Metroid Prime* (Nintendo GameCube, 2002) to native code, for
macOS, Windows (including PCVR), the Meta Quest 3 and the Apple Vision Pro (standalone VR). The main goal of this
particular port was getting as close to 'native-feeling' VR as possible.

The game's PowerPC executable is translated ahead of time into C, then compiled and linked
against a runtime that provides virtual GameCube hardware (graphics, audio, DVD, controllers,
memory card). The result is a native program that runs without an emulator. The recompiler
and runtime are [gcn-recomp](https://github.com/ethannicholas/gcn-recomp).

> **This repository contains no game code or data.** You must supply your own disc image,
> legally dumped from your own copy of the game. The build process reads the executable out
> of your image and recompiles it locally. No game code or assets are distributed here.

## License

The code in this repository and in `gcn-recomp` is released under the [MIT License](LICENSE).

## Legal

- *Metroid Prime*, GameCube and all related names and marks are property of Nintendo.
  This project is not affiliated with or endorsed by Nintendo or Retro Studios.
- This repository contains only original source code (the recompiler, the hardware/runtime
  layer and the renderer) plus metadata describing code layout (function addresses and
  names in `analysis/`, from the [Metroid Prime decompilation
  project](https://github.com/PrimeDecomp/prime)'s CC0 configuration). It contains no
  copyrighted game code, assets, BIOS/IPL files or DSP microcode.
- You need a legally obtained copy of the game. Dump it from your own disc using your own
  GameCube/Wii (e.g. with [CleanRip](https://wiibrew.org/wiki/CleanRip)). Do not download
  game images, and do not share the image, the extracted executable, the generated C code
  (`build/gen/`), built binaries, or memory card files, since all of these contain or are
  derived from the game.

## Supported version

Only the North American Rev 2 release (v1.02, the *Player's Choice* disc) is supported:

| Title | Disc ID | `main.dol` SHA-1 |
|---|---|---|
| Metroid Prime (USA) (Rev 2) | `GM8E01` | `39a2f928159ad46491e9b1ef0a72613d91e0cc40` |

## Status

Complete. The whole game is playable from start to finish, with no known significant bugs.
`docs/dev/status.md` has further details.

## Building

### Requirements

**macOS (Apple Silicon)**

- Xcode (or the Command Line Tools), CMake 3.20+, Ninja, Python 3
- SDL2

With Homebrew:

```sh
brew install cmake ninja sdl2 python
```

**Windows (x64 or ARM64)**

- Clang, CMake 3.20+, Ninja, Python 3
- The Windows SDK and MSVC headers/libraries (the "Desktop development with C++" workload of
  Visual Studio or the standalone Build Tools): clang targets the MSVC ABI and uses them
- An OpenGL 3.3 driver (see [Graphics drivers](#graphics-drivers))

With [winget](https://learn.microsoft.com/windows/package-manager/):

```powershell
winget install Kitware.CMake Ninja-build.Ninja Python.Python.3.13 LLVM.LLVM Microsoft.VisualStudio.2022.BuildTools
```

SDL2 is built from source as part of the build, since there are no prebuilt ARM64 binaries.

### 1. Provide your game image

Dump your disc, then place the image in `rom/` as an uncompressed **`.iso`** or a **`.ciso`**:

```
rom/game.iso
```

If your dump is in another format (`.rvz`, `.gcz`), convert it first, for example with
Dolphin (right-click the game → *Convert File…* → *Uncompressed Disc Image* or *CISO*),
`dolphin-tool convert`, or [`nodtool`](https://github.com/encounter/nod):

```sh
cargo install --locked nodtool
nodtool convert "rom/Metroid Prime (USA) (Rev 2).rvz" rom/game.iso
```

The build uses the first `.iso` or `.ciso` it finds in `rom/`. To use an image elsewhere,
pass `-DGAME_ISO=/path/to/game.iso` when configuring.

### 2. Build

```sh
./build.sh          # macOS
```

```powershell
.\build.ps1         # Windows
```

The first build fetches the `gcn-recomp` submodule if a plain `git clone` left it empty
(`git clone --recursive` avoids that).

### 3. Run

```sh
./build/prime            # uses the configured image / rom/*.iso
./build/prime path/to/game.iso
```

On Windows the executable is `build\prime.exe`.

Run it from the repository root: the memory card is created in `saves/` relative to the
current directory. Every run also records the controller input to `saves/inputs/<timestamp>/`
so that a route can be played back with `--replay=<that directory>`; `--no-input-log` turns
that off.

### Graphics drivers

The renderer needs an OpenGL 3.3 core profile. Any current GPU driver provides it. A Windows
virtual machine or a GPU with no OpenGL driver offers only Windows' built-in OpenGL 1.1, and
the game stops with `OpenGL 1.1 is too old`. In that case, put Mesa's software renderer
beside `prime.exe`: `opengl32.dll`, `libgallium_wgl.dll` and the DLLs they load, from
[mesa-dist-win](https://github.com/pal1000/mesa-dist-win/releases) on x64 or MSYS2's
`mingw-w64-clang-aarch64-mesa` package on ARM64. Set `GALLIUM_DRIVER=llvmpipe` to skip its
attempt to find a Vulkan device first. It is slow, but it renders correctly.

## Controls (Windows / MacOS)

| GameCube | Keyboard | Controller |
|---|---|---|
| Control stick | Arrow keys | Left stick |
| C stick | – | Right stick |
| A / B / X / Y | X / Z / C / S | A / X / B / Y |
| L / R | Q / W | Triggers |
| Z | D | Right shoulder |
| D-pad | I / J / K / L | D-pad |
| Start | Return | Start |

Escape quits.

## Running on a Quest 3

The Android build and packaging scripts are PowerShell, so these steps are written for a
Windows PC.

### Requirements

On the PC, besides CMake, Ninja and Python as for the desktop build:

- [Android NDK](https://developer.android.com/ndk/downloads), unpacked to
  `%LOCALAPPDATA%\Android\Sdk\ndk\<version>\`
- [platform-tools](https://developer.android.com/tools/releases/platform-tools) (for `adb`),
  unpacked to `%LOCALAPPDATA%\Android\Sdk\platform-tools\`
- The Android SDK's `build-tools;34.0.0` and `platforms;android-34`, installed with
  `sdkmanager` from the
  [command-line tools](https://developer.android.com/studio#command-line-tools-only)
  (run `sdkmanager --licenses` first), so that they land under `%LOCALAPPDATA%\Android\Sdk\`
- A JDK, for signing the APK: `winget install Microsoft.OpenJDK.21`

On the headset, enable
[developer mode](https://developer.oculus.com/documentation/native/android/mobile-device-setup/),
connect it over USB, put it on, and accept the *Allow USB debugging* prompt. `adb devices`
should list it as `device`.

### Build and install

With your disc image in `rom/`:

```powershell
.\build-android.ps1          # cross-compiles libprime.so for arm64
.\package-apk.ps1 -Install   # packages the APK, installs it, and pushes the disc image
```

The disc image is not part of the APK. It is copied to the app's data directory on the
headset, `/sdcard/Android/data/com.ethannicholas.prime/files/`, which takes a few minutes the first
time. The memory card and the shader cache live there too, and survive reinstalling.

### Playing

The app appears under **Library → Unknown Sources → Metroid Prime Recomp**. The game will seamlessly switch
between full 3D and a (still stereoscopic) 2D "theater mode" as needed.

| GameCube | Touch controller |
|---|---|
| Control stick | Left thumbstick |
| C stick | Right thumbstick |
| A (fire) | Right trigger |
| B (jump) | A (right) |
| Y (missile) | B (right)  |
| X (morph ball) | X (left) |
| Z (map) | Y (left) |
| R (free look) | Right grip |
| L (lock-on) | Left trigger |
| Start | Menu (left) |
| D-pad (visors) | Hold left grip + left thumbstick |

On the map and menu screens, the controls change to better match the on-screen legend (e.g. the L
and R triggers become the GameCube's L and R triggers on the map screen).

### Tuning the VR view

Settings are read when the app starts, from `/sdcard/Android/data/com.ethannicholas.prime/files/vr.txt`,
so change them with `adb push` and restart the app. The file is optional: any setting it
leaves out keeps its default, and a misspelt name is silently ignored. One `key value` per
line; `#` starts a comment. The values below are the defaults.

**Size and distance.** "Nearer" settings keep things the same size in view: they change how
far away something feels, not how big it looks.

```
units_per_metre 1        # game units per real metre: higher makes the world smaller
hud_band_scale 0.05      # how near the HUD is -- energy, radar, visor and beam selectors:
                         #   a fraction of the distance the game models it at, so 0.05 is
                         #   about 0.8-1.05 m; smaller is nearer
hud_distance_m 4         # where the game's flat 2D elements hang, such as the scan
                         #   window's outline, in metres
hud_scale 0.5            #   and how much of the view's height they fill
hud_height_m 0           #   raised by this many metres
hud_pitch_deg 0          #   tilted by this many degrees (negative leans the top towards you)
foreground_scale 0.5     # how near the arm cannon is drawn, as a fraction of its distance
theater_depth 1          # depth of the flat screen's 3D picture (0 flattens it, 1 is true depth)
near_m 0.05              # nearest and farthest distances drawn, in metres
far_m 1000
```

**The arm cannon.**

```
gun_follows_hand 1       # the cannon follows the right controller (0: the game's own aim)
gun_x 0                  # move the cannon from the controller, in metres
gun_y 0                  #   (x right, y up, z back, along the controller)
gun_z 0
gun_pitch_deg 0          # tilt the cannon against the controller, in degrees
gun_smoothing 3          # frames the hand's movement is averaged over (1-16; 1 is none)
gamepad_gun_x 0.2032     # with a gamepad, how far the cannon is moved from where the game
gamepad_gun_y -0.2032    #   holds it, in metres as you see it (x right, y up, z back)
gamepad_gun_z 0
```

**Looking and aiming.**

```
camera_follows_head 1    # the game's camera turns after your head once you look far enough
                         #   off to the side, so the HUD, lock-on and scanning follow you
camera_follow_reach 0.32 #   how far you can look before it does (smaller: sooner)
camera_follow_rate 8     #   how quickly it follows, and how quickly it comes back to
camera_return_rate 4     #   straight ahead
lock_on_follows_gaze 1   # lock on to what you look at, even far above or below
scan_follows_head 1      # the scan visor's window and target follow your gaze
scan_window_faces 1      #   the window turns to face you wherever it is
scan_gaze_field 1        #   the gaze is the middle of what you can see rather than straight
                         #   ahead of your eyes
scan_gaze_pitch_deg 0    #   and this many degrees above it
gamepad_classic 0        # with a gamepad: 0 for Metroid Prime Remastered's dual-stick
                         #   controls, 1 for the GameCube's
gamepad_look_deg_per_s 150  # with a gamepad, how fast the right stick looks up and down
```

**What is drawn, and where.** Each of these is 1 (on) by default; 0 turns it off and shows
the game's original behaviour.

```
hide_helmet 1            # leave out the inside of Samus's helmet around the view
hide_scan_tint 1         # leave out the scan visor's grey tint
hide_scan_magnifier 1    # leave out the scan window's magnified picture
hide_flat_warps 1        # leave out the flat ripple a charged shot leaves
visor_film_on_head 1     # steam and other films on the visor cover your whole view
visor_drops_on_head 1    # goo and rain on the visor stay on your face
screen_filter_on_head 1  # the thermal visor's colours cover your whole view
cannon_cutaway 1         # the beam change's wipe is drawn in each eye
scan_icons_true_distance 1  # scan markers and the reticle round a target stand at the target
reticle_true_distance 1  # the lock-on reticle stands where you are aiming
cull_to_eyes 1           # the game draws what you can see, not only what its camera sees
cull_margin_deg 10       #   with this many degrees of margin beyond the edge of your view
```

**Performance and image quality.**

```
eye_scale 1.2            # resolution of each eye, times what the headset recommends
msaa 2                   # antialiasing samples per pixel in 3D
gpu_budget_ms 13         # the eyes draw fewer pixels when the GPU takes longer than this
                         #   many milliseconds a frame (0: always full size)
min_eye_res 0.5          #   but never fewer than this fraction of each side
display_hz 0             # the display's refresh rate (0: the headset's default)
perf_boost 1             # ask the headset for its highest CPU and GPU clocks
thread_hints 1           # tell the headset which of the app's threads matter most
theater_scale 3          # supersampling of the flat screen
stereo_scale 2           # resolution of the copies 3D takes from the flat picture, such as
                         #   the acid's fog
```

**Switching between the flat screen and 3D.** The game decides when to switch: 3D in first
person, the flat screen for the menus, the map, the Morph Ball and cutscenes.

```
transition_s 1           # seconds the change into 3D takes
transition_out_s 0       # seconds the change back takes (0: a cut)
theater_stereo 1         # the flat screen is shown as a 3D picture (0: flat)
```

The remaining settings decide how the game's layers are told apart in 3D (`background_band`
0.99, `foreground_band` 0.5, `hud_band` 0.02, `panel_band` 0.02, `hud_push_below` 10,
`hud_push_to` 22, `view_space_world` 0, `offset_x`/`offset_y`/`offset_z` 0); changing them
breaks the picture rather than tuning it. [docs/dev/vr.md](docs/dev/vr.md) and
[gcn-recomp/docs/graphics.md](gcn-recomp/docs/graphics.md) describe them.

## Running on an Apple Vision Pro

New, and not yet confirmed on a device: see [docs/dev/vr.md](docs/dev/vr.md#the-vision-pro).

### Requirements

- A Mac with Xcode 26 (with the visionOS SDK), CMake, and Python 3.
- An Apple developer account signed in to Xcode. The build signs with the team in
  `$GCN_APPLE_TEAM`.
- The Vision Pro paired with the Mac (Xcode → Window → Devices and Simulators) and in
  developer mode (Settings → Privacy & Security → Developer Mode).

### Build and install

With your disc image in `rom/`:

```sh
GCN_APPLE_TEAM=<your team id> ./build-visionos.sh --install --push-image
```

The first build also builds [ANGLE](https://chromium.googlesource.com/angle/angle) (OpenGL
ES on Metal) from WebKit's sources, which takes a few minutes. `--push-image` copies the disc
image into the app's Documents folder. You can instead choose it from the Files app in the
app's launcher window. The memory card, the shader cache, `vr.txt` and the logs live in the
same folder, which the Files app shows under *On My Apple Vision Pro*.

### Playing

Open **Metroid Prime Recomp** and press *Play*. The game runs in a full immersive space. Leaving
it, or the game resetting the console (after a game over), closes the app; open it again
to carry on.

With **PlayStation VR2 Sense controllers**, the arm cannon follows your right hand, as on
the Quest:

| GameCube | Sense controller |
|---|---|
| Control stick | Left stick |
| C stick | Right stick |
| A (fire) | Right trigger |
| B (jump) | Right face button A |
| Y (missile) | Right face button B |
| X (morph ball) | Left face button A |
| Z (map) | Left face button B |
| R (free look) | Right grip |
| L (lock-on) | Left trigger |
| Start | Either menu button |
| D-pad (visors) | Hold left grip + left stick |

With a **gamepad** (Xbox, PlayStation or MFi), the arm cannon points where you look, and the
buttons are laid out as on the desktop:

| GameCube | Gamepad |
|---|---|
| A / B / X / Y | South / West / East / North face buttons |
| Z | Right shoulder |
| L / R | Left / right trigger |
| Start | Menu |
| Control stick, C stick, D-pad | Left stick, right stick, D-pad |

`vr.txt` in the app's Documents folder takes the same settings as on the Quest.

## Repository layout

- `gcn-recomp/`: the shared recompiler, GameCube runtime, renderer and frontends, as a
  submodule of [gcn-recomp](https://github.com/ethannicholas/gcn-recomp). Nothing in it is
  specific to this game.
- `analysis/`: function and data layout of `main.dol`, in decomp-toolkit's format.
- `recomp/`: the tables that steer the recompiler for this game (`hle.txt`,
  `special_calls.txt`, `idle.txt`, `names.txt`, `patches.txt`).
- `src/`: the little runtime code that is about this game: a heap checker for diagnostic
  builds, and the headset app's settings for it (`vr_prime.cpp`).
- `docs/dev/`: working notes.
