# VR

Not started. The plan, so that it is written down before the first attempt:

- Prime is first person. The game's own camera is the player's head, so an eye is the game's
  view with a per-eye projection and an eye offset applied in the GX transform stage, not the
  chase-camera reconstruction Blue Storm needed. Cutscenes and the morph ball keep the game's
  camera and want a theater view instead.
- The HUD is drawn as world geometry hanging in front of the camera (the visor frame, the
  energy bar, the map). Which draws those are is readable from their position matrix, as in
  Blue Storm's `view_space` flag; they should sit on a fixed frame in head space.
- Visor effects (scan, thermal, X-ray) and the many EFB copies they use are the renderer's
  hard part, as the water was for Blue Storm.
- Target: Meta Quest 3 through OpenXR, which means bringing back Blue Storm's OpenGL ES path
  and Android frontend.
