# prime-recomp

A static recompilation of *Metroid Prime* (Nintendo GameCube, 2002) to native code, for
macOS and Windows, with a VR port as the goal.

The game's PowerPC executable is translated ahead of time into C, then compiled and linked
against a runtime that stands in for the GameCube hardware (graphics, audio, DVD, controllers,
memory card). The result is a native program that runs the original game logic without an
emulator's CPU core. The recompiler and runtime are
[gcn-recomp](https://github.com/ethannicholas/gcn-recomp), shared with
[bluestorm-recomp](https://github.com/ethannicholas/bluestorm-recomp), where they were first
written for *Wave Race: Blue Storm*.

> **This repository contains no game code or data.** You must supply your own disc image,
> dumped from a copy of the game that you own. The build process reads the executable out of
> *your* image and recompiles it locally. Nothing derived from the game is distributed here.

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

The build verifies this hash and stops with an error for any other version.

## Status

Early. The game boots and runs its startup; see `docs/dev/status.md` for how far it gets and
what is known not to work.

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

## Controls

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

Early: the app builds and installs, but has not yet been played in the headset.

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

The app appears under **Library → Unknown Sources → Metroid Prime**. Menus, cinematics and
the morph ball are shown on a flat screen in front of you; first-person play switches to
stereo 3D by itself. Clicking the left thumbstick switches by hand, until the game next
changes view. In stereo the arm cannon is in your right hand and fires where it points, and
the scan visor's window follows your head: look at something to scan it, no free look needed.

| GameCube | Touch controller |
|---|---|
| Control stick | Left thumbstick |
| C stick | Right thumbstick |
| A / B | A / B (right) |
| A (fire) | Right trigger as well, except on the pause and map screens, where it is R |
| X (morph ball) / Y (missile) | Y / X (left) |
| Z | Right grip |
| L | Left trigger |
| Start | Menu (left) |
| D-pad (visors) | Hold left grip + left thumbstick |
| *(flat screen / stereo, by hand)* | Left thumbstick click |

`adb logcat -s prime` shows the app's log.

### Tuning the VR view

Settings are read at startup from `/sdcard/Android/data/com.ethannicholas.prime/files/vr.txt`, so
they can be changed with `adb push` between runs. The file is optional; every key has a
default. One `key value` per line, `#` starts a comment:

```
units_per_metre 1        # game units per real metre: sets the apparent size of the world
near_m 0.05              # near and far planes in metres
far_m 1000
hud_distance_m 4         # where the game's 2D elements hang, and how much of the view
hud_scale 0.5            #   they fill
foreground_scale 0.5     # how much nearer and smaller the arm cannon and visor are drawn
eye_scale 1.4            # eye resolution, times what the headset recommends
msaa 4                   # antialiasing samples per pixel in stereo
theater_scale 3          # supersampling of the flat screen
stereo_scale 1           # resolution of what stereo copies out of the flat pass, such as
                         #   the scan visor's magnified window: 2 doubles it
transition_s 1           # seconds the switch between flat and stereo takes
start_in_stereo 0        # 1 to start in stereo
gun_follows_hand 1       # the arm cannon in the right hand (0: the game's own aim)
scan_follows_head 1      # the scan visor's window and target follow the head
```

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
