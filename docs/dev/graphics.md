# Graphics notes

The renderer is the shared runtime's; see
[`gcn-recomp/docs/graphics.md`](../../gcn-recomp/docs/graphics.md) for the GL profile, the
shader cache and the stereo scaffolding inherited from Wave Race.

Prime-specific observations:

- `src/shader_keys.bin` is the game's shader key list (`SHADER_KEYS`; see "Shader cache"
  in the shared doc), made on 2026-10-19 from the headset's three caches, in this order:
  the app's (2,524 records), the harness's (2,516 records) and an older harness cache from
  before canonical keys (11,188 records). Those caches were written before fog and the
  stages' konstants left the key, and under the canonicalization that came with that they
  are 931, 4 more and 7 more: 942 keys, 286 KB. (Under the canonicalization before it,
  the app's cache was already 2,524 distinct keys.) To extend it, put the current list
  first so its order holds: `shader_keys src/shader_keys.bin src/shader_keys.bin <new
  caches>`, run on the headset when no desktop toolchain is at hand.

- Retro stores textures bottom-up and the models' UVs compensate, so `GCN_TEXDUMP` output
  looks upside down by design.
- The lava's and the acid's fog volumes (`CCubeRenderer::DrawFogVolume`) are drawn per
  screen chunk from two Z16 depth copies, read through indirect texturing into a 256x256
  ramp whose rows are the depth's top byte and columns its middle byte. A renderer that
  writes the copy's two bytes the other way round transposes the lookup and draws bright
  lines along depth contours instead of fog (`vr.md`, 2026-10-13).
- The thermal visor (`CCubeRenderer::DoThermalBlendCold` and `DoThermalBlendHot`) is two
  post-passes over the whole frame. The room is drawn as usual, copied as I4 (640x448), and
  drawn back over the screen tinted purple, jittered by a 640x448 noise texture through an
  indirect stage. The hot things are then drawn with their heat in the frame's alpha (a
  64x64 noise texture plus a konstant), the frame is copied as I4 again to the same address,
  and that copy is drawn back bound as **CI4** with the heat palette loaded, multiplied by
  the palette's alpha and blended by the frame's alpha. A renderer that samples the copy as
  the intensity it was copied as draws the hot things white or grey, not yellow, orange and
  red; that was the case here until 2026-10-14 (`vr.md`, "The thermal visor").
- The title and menu backgrounds are THP videos decoded on the CPU into Y/U/V planes (I8,
  640x480 and two 320x240) and drawn through a YUV→RGB TEV configuration. All-zero planes
  render solid green, which is how a decoder that is not running shows itself.
