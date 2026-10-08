# Audio notes

## The DSP runs AX, driven by MusyX (2026-10-08)

The belief until 2026-10-08 was that Metroid Prime's sound needed a MusyX DSP ucode that the
runtime did not emulate, and that the AX HLE inherited from Blue Storm was only keeping the
game alive by luck. That was wrong. The ucode the game uploads (`boot ucode: ... len=19E0
start=0010 hash=4E8A8B21` in the `--log=dsp` output) is Nintendo's AX ucode of 2001, the
same one Dolphin's DSP HLE identifies as "spdemo, Crazy Taxi, ... Monkeyball" and handles
with its `AXUCode`. MusyX is only the CPU-side library: `salBuildCommandList` writes a
standard AX command list every 5 ms and `salCtrlDsp` sends it with the usual
`0xBABE0000 | length` and address mails. What disproved the old belief was hashing the
uploaded ucode the way Dolphin does (`ucode_hash` in `runtime/hw/dsp_hle.cpp`, now in the
boot log) and dumping the first command list, which read as plain AX commands.

The silence had a plain cause: the AX HLE skipped the commands Wave Race never used, and
had the argument count of one of them wrong. MusyX's list at the title screen is `SETUP`,
`MIX_AUXB_LR` (0x10), `UPLOAD_LRS`, `SET_OPPOSITE_LR` (0x11), `MIX_AUXB_NOWRITE`,
`OUTPUT`, `END`; the HLE skipped two words of 0x10's four, read the high half of the next
address as a command and gave up on the whole list, every frame. 0x10, 0x11, 0x13
(`SEND_AUX_AND_MIX`), 0x01 (download and mix with volume) and 0x07 (`SET_LR`) are now
implemented after Dolphin's, 0x09 adds instead of replacing, and the mixer-control word's
Dolby Pro Logic II bit (0x10) is honoured as Dolphin reads it for this ucode version. An
unknown command now logs the whole list once, so the next such gap is visible.

What MusyX does with those commands: the aux buses are effects sends. The DSP uploads the
AuxA/AuxB buffers to main memory, the CPU runs the reverb or chorus on them
(`salHandleAuxProcessing`, from the AI DMA interrupt), and the next frame's list mixes the
processed buffers back in. So reverb is the game's own code; the HLE only has to move the
buffers faithfully.

The title and menu music was audible before this fix, so it does not go through the DSP
voice path; it is not yet known whether that is the THP's audio track written straight into
the AI DMA buffer or the DVD streaming (DTK) path.

Not done: initial time delay (ITD, the per-voice left/right delay AX uses for
positioning) is ignored, as it was for Blue Storm, and the sample-rate converter is linear
rather than the ucode's polyphase filter.

## Diagnostics

`GCN_WAV=<path>` records what the host audio device was handed; `GCN_AXSTATS=1` prints,
every 400 frames, how many voices were running and the peak level. `--log=dsp` shows the
mails and the ucode boots.
