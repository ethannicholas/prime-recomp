# Diagnostics and measurement

How to drive the port without a controller, measure it, and find out which guest code or
which draw is responsible for something. Environment variables are `MP_*`; none of this is
needed to play.

## Scripted input

`MP_INPUT="frame:BUTTON:duration,..."` presses buttons at given submitted-frame counts; `+`
joins buttons (`A+B`), and `SL SR SU SD` push the control stick. Presses are counted in
presented game frames, not wall-clock, so a script reaches the same place whatever the frame
rate. `MP_INPUT_LOG=1` prints each press as it fires.

The route to gameplay from a fresh boot (no save on the memory card):

```
MP_INPUT="1300:START:10,1700:A:10,2100:A:10,2500:A:10,2900:A:10"
```

Title (press Start), *Start*, *New Game*, confirm, and through the intro text; the cutscene
runs on its own and gameplay begins around frame 9000. The counts are loose because the menus
fade on wall-clock time; a press that lands during a transition is lost.

## Frame dumps

`./build/prime --hidden --dump-dir=DIR --dump-every=N` runs with the window hidden and writes
every Nth presented frame as a PNG. `MP_DUMP_RANGE=a-b` restricts that to a window of frames.
`--headless` runs the guest with no renderer at all.

## Measuring the guest

`prime_bench` runs the game with no graphics, audio or input and reports how fast the guest
advances:

```sh
./build/prime_bench --seconds=25
```

The first seconds are boot; `--warmup=N` (default 8) excludes them from the steady-state figure.
`MP_TIMESCALE=N` makes the emulated timebase advance N times faster, so the game tries to run
at N× real time; raise it until the frame rate stops climbing to find a machine's ceiling. It
skews every other emulated timing, so it is a diagnostic only.

`MP_FRAMETIME=1` prints a line per frame from each thread: the guest's interval between
presents and how much of it the GX front end took, the batch's shape, and the renderer's time
to issue it.

## Textures

`MP_TEXLOG=1` prints a line per decoded texture: frame, id, guest address, format, size and how
many top-level texels are not black. That is how a texture the game never wrote (all zero) is
told from one the shader mishandles. `MP_TEXDUMP=<dir>` writes each texture's top level as
`<dir>/tex_<id>.png`, alpha forced opaque. Retro stores textures bottom-up, so the PNGs look
upside down; the models' UVs compensate.

## Reference captures with Dolphin

Dolphin is installed at `/Applications/Dolphin.app` (5.0-15260) and plays the same CISO. Its
controller profile maps the GameCube pad to a gamepad, not the keyboard. Launching it with a
scratch user directory (`-u <dir>` with a keyboard `GCPadNew.ini`) and `-e <image>` opened the
main window but never booted the game, with nothing in its log; the cause was not found.
Killing Dolphin with a signal makes macOS put up a "reopen windows?" dialog on the next launch
that swallows the first key presses, so quit it through AppleScript instead.

## Finding a draw or a function

- `MP_DRAWLOG=<frame>` lists every draw in one frame with its index; `MP_DRAW_SKIP=a-b` then
  drops a range of them, to attribute a piece of the image to the draws that made it.
- `MP_MTXLOG=<frame>` prints the position matrix and projection of every draw in that frame;
  `MP_PNMLOG=a-b` lists the position matrices used over a window of frames.
- `MP_WATCH=a,b,c` (with `MP_MTXLOG` set) prints those guest addresses every frame; `MP_PEEK=a,b`
  prints them in the crash dump.
- `MP_COPYLOG=1` logs EFB copies; `MP_GXSTATS=1` prints per-frame GX counts.
- `--sample` reports once a second which game functions each guest thread is in, by walking
  the guest's own stack. Function names come from `analysis/symbols.txt`.
- Build with `-DMP_TRACE_CALLS=ON` to keep a real guest call stack, printed by the crash
  dump (`MP_TRACE_DEPTH` sets how many frames).
- `--log-all` enables every log category, including the DSP mailbox and DVD reads.

## Panics and faults

The game's `OSPanic` is replaced (`hle.txt`) so that a panic prints the message and a guest
backtrace and exits, instead of spinning in `PPCHalt` looking like a hang. A host fault
inside guest memory prints the guest address and every thread's registers.
