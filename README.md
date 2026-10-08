# prime-recomp

A static recompilation of *Metroid Prime* (Nintendo GameCube, 2002) to native code, for
macOS, with a VR port as the goal.

The game's PowerPC executable is translated ahead of time into C, then compiled and linked
against a runtime that stands in for the GameCube hardware (graphics, audio, DVD, controllers,
memory card). The result is a native program that runs the original game logic without an
emulator's CPU core. The recompiler and runtime come from
[bluestorm-recomp](https://github.com/ethannicholas/bluestorm-recomp), the same treatment of
*Wave Race: Blue Storm*.

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
./build.sh
```

### 3. Run

```sh
./build/prime            # uses the configured image / rom/*.iso
./build/prime path/to/game.iso
```

Run it from the repository root: the memory card is created in `saves/` relative to the
current directory.

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

## Repository layout

- `recomp/`: the PowerPC → C recompiler (`recomp.py`, `ppc.py`) and the tables that steer
  it (`hle.txt`, `special_calls.txt`, `patches.txt`).
- `runtime/`: the GameCube in software — CPU helpers, OS context switching, hardware
  registers (`hw/`), the GX graphics pipeline (`gx/`), audio, and the SDL frontend.
- `analysis/`: function and data layout of `main.dol`, in decomp-toolkit's format.
- `docs/dev/`: working notes.
- `tools/fetch_dtk.sh`: downloads decomp-toolkit, used to regenerate `analysis/`.
