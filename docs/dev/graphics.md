# Graphics back end notes

The renderer is Blue Storm's: an **OpenGL 3.3 core profile** back end behind a GX front end
that runs vertex decoding, transform, lighting and texgen on the CPU, and generates one
fragment shader per TEV configuration. On macOS the system framework is linked directly;
elsewhere a vendored glad loader resolves the entry points.

The OpenGL ES path (Android, headsets) was left behind in Blue Storm and will come back
with the VR port.

## Shader cache

Each TEV configuration is compiled the first time a draw uses it, mid-frame. Every key
compiled is appended to `saves/shaders.bin`, and the next run builds all of them before the
game boots. Deleting the file is always safe. The startup line reports what happened:

```
[shaders] 29 programs from cache (0 from binaries, 29 compiled) in 44 ms
```

## Inherited VR scaffolding

`runtime/gx/render_gl.cpp` and `shadergen.cpp` still carry Blue Storm's stereo machinery:
`render_execute_eye`, the theater/stereo morph, the world-pitch and first-person camera
heuristics, and the eye grab/refraction handling. None of it runs from the desktop frontend,
and most of it is specific to a chase-camera racing game. It stays until the Prime VR design
replaces it, rather than being deleted and re-derived.
