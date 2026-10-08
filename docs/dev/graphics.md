# Graphics notes

The renderer is the shared runtime's; see
[`gcn-recomp/docs/graphics.md`](../../gcn-recomp/docs/graphics.md) for the GL profile, the
shader cache and the stereo scaffolding inherited from Wave Race.

Prime-specific observations:

- Retro stores textures bottom-up and the models' UVs compensate, so `GCN_TEXDUMP` output
  looks upside down by design.
- The title and menu backgrounds are THP videos decoded on the CPU into Y/U/V planes (I8,
  640x480 and two 320x240) and drawn through a YUV→RGB TEV configuration. All-zero planes
  render solid green, which is how a decoder that is not running shows itself.
