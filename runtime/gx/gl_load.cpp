// Resolves the GL entry points through whatever the host uses to look them up. Kept
// free of SDL so the EGL-only Android frontend can link it too.
#include "gl.h"

int gl_load_with(GLLoadProc proc) {
#ifdef __APPLE__
    (void)proc;
    return 41;  // linked directly against the system framework, which is 4.1
#else
    int v;
#ifdef MP_GL_ES
    v = gladLoadGLES2((GLADloadfunc)proc);
#else
    v = gladLoadGL((GLADloadfunc)proc);
#endif
    if (!v) return 0;
    return GLAD_VERSION_MAJOR(v) * 10 + GLAD_VERSION_MINOR(v);
#endif
}
