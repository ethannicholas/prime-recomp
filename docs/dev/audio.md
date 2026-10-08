# Audio notes

Metroid Prime's sound runs on the **MusyX** DSP microcode (the boot log shows it uploaded:
`boot ucode: iram ... len=19E0 start=0010`), not the SDK's AX. The DSP HLE in
`runtime/hw/dsp_hle.cpp` and `hw/ax.cpp` was written for AX: it boots whatever ucode the
game sends and then interprets every mail as an AX command list, which produces no sound and
logs `AX: unknown command` (throttled to a few lines).

That the game runs at all under this treatment is luck: the MusyX driver evidently tolerates
the replies it gets. Do not read that as the DSP being emulated.

Music is streamed from `Audio/*.dsp` files on the disc (DSP-ADPCM, stereo as separate L/R
files), decoded through the DSP, so there is nothing to hear until MusyX is implemented. The
title and menu THP videos most likely pace on audio too; see `status.md`.

The host side -- the mix in `runtime/audio.cpp` resampling the AI DMA stream to 48 kHz and
the SDL device in `audio_sdl.cpp` -- is unchanged from Blue Storm and will carry whatever the
DSP HLE eventually produces. `MP_WAV=<path>` records what the device was handed.
