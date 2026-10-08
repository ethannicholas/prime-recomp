// OpenGL include shim and render-target selection.
//
// The renderer targets two profiles from one source:
//
//   * desktop OpenGL 3.3 core  -- macOS, Windows, Linux
//   * OpenGL ES 3.2            -- Android / standalone headsets (MP_GL_ES)
//
// Nothing here uses a feature newer than GL 3.3 / ES 3.0: samplers, VAOs, FBOs and
// explicit attribute locations are the whole requirement. Keeping the desktop floor at
// 3.3 also means mapping layers that stop there (Mesa's D3D12 driver, as shipped in
// Microsoft's OpenGL compatibility pack) can run it.
//
// macOS links the system framework directly; everywhere else the entry points are
// resolved at runtime by a generated glad loader -- see glad/, glad_es/ and gl_load.cpp.
#pragma once

#ifdef MP_GL_ES

#define MP_GL_MAJOR 3
#define MP_GL_MINOR 2
// GLSL ES has no default precision for float or int in a fragment shader, so it has to
// be stated or the shader fails to compile. Stays on one line per statement so the
// #version directive remains the first line.
#define MP_GLSL_VERSION "#version 320 es\nprecision highp float;\nprecision highp int;\n"
#include <glad/gles2.h>

#else

#define MP_GL_MAJOR 3
#define MP_GL_MINOR 3
#define MP_GLSL_VERSION "#version 330 core\n"
#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <glad/gl.h>
#endif

#endif

// Minimum acceptable version, encoded as major * 10 + minor, as gl_load() returns it.
#define MP_GL_VERSION_MIN (MP_GL_MAJOR * 10 + MP_GL_MINOR)

// How the GL entry points are found: SDL_GL_GetProcAddress for the SDL frontend, or
// eglGetProcAddress for an EGL-only host such as a headless Android device.
using GLLoadProc = void* (*)(const char* name);

// Resolve the GL entry points for the current context. Must be called once after the
// context is made current, before any other gx:: call. Returns the version actually
// resolved as major * 10 + minor, or 0 if no usable GL was found.
//
// The result has to be checked against MP_GL_VERSION_MIN: a legacy driver (Windows'
// "GDI Generic" 1.1, say) resolves successfully but leaves every GL 2.0+ entry point
// null, so calling into the renderer afterwards would dereference a null pointer.
int gl_load_with(GLLoadProc proc);
