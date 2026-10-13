# Graphics notes

The renderer is the shared runtime's; see
[`gcn-recomp/docs/graphics.md`](../../gcn-recomp/docs/graphics.md) for the GL profile, the
shader cache and the stereo scaffolding inherited from Wave Race.

Prime-specific observations:

- Retro stores textures bottom-up and the models' UVs compensate, so `GCN_TEXDUMP` output
  looks upside down by design.
- The lava's and the acid's fog volumes (`CCubeRenderer::DrawFogVolume`) are drawn per
  screen chunk from two Z16 depth copies, read through indirect texturing into a 256x256
  ramp whose rows are the depth's top byte and columns its middle byte. A renderer that
  writes the copy's two bytes the other way round transposes the lookup and draws bright
  lines along depth contours instead of fog (`vr.md`, 2026-10-13).
- The title and menu backgrounds are THP videos decoded on the CPU into Y/U/V planes (I8,
  640x480 and two 320x240) and drawn through a YUV→RGB TEV configuration. All-zero planes
  render solid green, which is how a decoder that is not running shows itself.
