// Entry point and SDL frontend: window, input, audio. The main thread executes GX
// batches produced by the guest thread.
#include "runtime.h"
#include "platform.h"
#include "input_script.h"
#include "gx/render_gl.h"
#include "gx/gl.h"
#include "hw/pad.h"
#ifdef _WIN32
// Console app: keep our own main() rather than SDL2main's WinMain shim.
#define SDL_MAIN_HANDLED
#endif
#include <SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

uint32_t boot_load(const char* iso_path);
void debug_dump_threads();
void debug_sampler_start();

// Stringify, to quote the required GL version in a message without a format argument.
#define MP_STR_(x) #x
#define MP_STR(x) MP_STR_(x)

static void on_interrupt() {
    plat_watchdog(2, 2);  // never hang in here (stdio locks may be held by other threads)
    debug_dump_threads();
    plat_exit_now(1);
}

static void on_fault(const void* fault_addr, int code) {
    uintptr_t a = (uintptr_t)fault_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x40000000)
        fprintf(stderr, "\nFAULT: guest memory access at (addr & 0x3FFFFFFF) = %08lX\n", (unsigned long)(a - (uintptr_t)g_mem));
    else
        fprintf(stderr, "\nFAULT: host address %p (code %d)\n", fault_addr, code);
    plat_watchdog(2, 2);
    plat_backtrace_print();
    debug_dump_threads();
    plat_exit_now(1);
}

bool audio_open();

// ---------------------------------------------------------------------------
// Input: keyboard + first game controller -> GC pad 1
// ---------------------------------------------------------------------------
static SDL_GameController* g_ctrl;

static uint8_t axis_to_u8(int v, bool invert) {
    float f = v / 32767.0f;
    if (invert) f = -f;
    int r = 128 + (int)(f * 100.0f);
    return (uint8_t)std::clamp(r, 0, 255);
}

static void update_pad() {
    PadState p;
    p.connected = true;
    const uint8_t* k = SDL_GetKeyboardState(nullptr);
    if (k[SDL_SCANCODE_RETURN]) p.buttons |= PAD_START;
    if (k[SDL_SCANCODE_X]) p.buttons |= PAD_A;
    if (k[SDL_SCANCODE_Z]) p.buttons |= PAD_B;
    if (k[SDL_SCANCODE_C]) p.buttons |= PAD_X;
    if (k[SDL_SCANCODE_S]) p.buttons |= PAD_Y;
    if (k[SDL_SCANCODE_D]) p.buttons |= PAD_Z;
    if (k[SDL_SCANCODE_Q]) { p.buttons |= PAD_L; p.trig_l = 255; }
    if (k[SDL_SCANCODE_W]) { p.buttons |= PAD_R; p.trig_r = 255; }
    if (k[SDL_SCANCODE_I]) p.buttons |= PAD_UP;
    if (k[SDL_SCANCODE_K]) p.buttons |= PAD_DOWN;
    if (k[SDL_SCANCODE_J]) p.buttons |= PAD_LEFT;
    if (k[SDL_SCANCODE_L]) p.buttons |= PAD_RIGHT;
    int sx = 0, sy = 0;
    if (k[SDL_SCANCODE_LEFT]) sx -= 100;
    if (k[SDL_SCANCODE_RIGHT]) sx += 100;
    if (k[SDL_SCANCODE_UP]) sy += 100;
    if (k[SDL_SCANCODE_DOWN]) sy -= 100;
    p.stick_x = (uint8_t)(128 + sx);
    p.stick_y = (uint8_t)(128 + sy);
    if (g_ctrl) {
        auto b = [&](SDL_GameControllerButton btn) { return SDL_GameControllerGetButton(g_ctrl, btn); };
        if (b(SDL_CONTROLLER_BUTTON_A)) p.buttons |= PAD_A;
        if (b(SDL_CONTROLLER_BUTTON_X)) p.buttons |= PAD_B;
        if (b(SDL_CONTROLLER_BUTTON_B)) p.buttons |= PAD_X;
        if (b(SDL_CONTROLLER_BUTTON_Y)) p.buttons |= PAD_Y;
        if (b(SDL_CONTROLLER_BUTTON_START)) p.buttons |= PAD_START;
        if (b(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) p.buttons |= PAD_Z;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_UP)) p.buttons |= PAD_UP;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) p.buttons |= PAD_DOWN;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) p.buttons |= PAD_LEFT;
        if (b(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) p.buttons |= PAD_RIGHT;
        int lx = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_LEFTX);
        int ly = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_LEFTY);
        if (abs(lx) > 4000 || abs(ly) > 4000) { p.stick_x = axis_to_u8(lx, false); p.stick_y = axis_to_u8(ly, true); }
        p.cstick_x = axis_to_u8(SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_RIGHTX), false);
        p.cstick_y = axis_to_u8(SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_RIGHTY), true);
        int lt = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        int rt = SDL_GameControllerGetAxis(g_ctrl, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (lt > 1000) { p.trig_l = (uint8_t)(lt * 255 / 32767); if (lt > 30000) p.buttons |= PAD_L; }
        if (rt > 1000) { p.trig_r = (uint8_t)(rt * 255 / 32767); if (rt > 30000) p.buttons |= PAD_R; }
    }
    input_script_apply(p);
    pad_set_state(0, p);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    // Default: the image the build was configured with, else the first image in ./rom
    std::string iso_default = MP_DEFAULT_ISO;
    if (!plat_readable(iso_default.c_str())) iso_default = plat_find_file("rom", ".iso");
    if (iso_default.empty()) iso_default = plat_find_file("rom", ".ciso");
    const char* iso = iso_default.c_str();
    bool headless = false, hidden = false;
    int scale = 2;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--log-all")) for (auto& e : g_log_enabled) e = true;
        else if (!strcmp(argv[i], "--sample")) debug_sampler_start();
        else if (!strcmp(argv[i], "--headless")) headless = true;
        else if (!strcmp(argv[i], "--hidden")) hidden = true;
        else if (!strcmp(argv[i], "--cull-swap")) gx::g_cull_swap = true;
        else if (!strncmp(argv[i], "--scale=", 8)) scale = atoi(argv[i] + 8);
        else if (!strncmp(argv[i], "--dump-dir=", 11)) gx::g_dump_dir = argv[i] + 11;
        else if (!strncmp(argv[i], "--dump-every=", 13)) gx::g_dump_every = atoi(argv[i] + 13);
        else if (argv[i][0] != '-') iso = argv[i];
    }
    plat_install_crash_handlers(on_interrupt, on_fault);

    if (!plat_readable(iso)) {
        fprintf(stderr, "No game image found. Put your Metroid Prime (USA) .iso or .ciso in rom/ or pass its path.\n");
        return 1;
    }
    mem_init();
    timing_init();
    input_script_init();

    if (headless) {
        uint32_t entry = boot_load(iso);
        threads_start_boot(entry);
        for (;;) {
            PadState p;
            p.connected = true;
            input_script_apply(p);
            pad_set_state(0, p);
            auto b = gx::take_batch(5);  // discard
            (void)b;
        }
    }

#ifdef _WIN32
    SDL_SetMainReady();
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) fatal("SDL_Init: %s", SDL_GetError());
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, MP_GL_MAJOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, MP_GL_MINOR);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_Window* win = SDL_CreateWindow("Metroid Prime (recompiled)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                       1280, 960, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                                       (hidden ? SDL_WINDOW_HIDDEN : 0));
    if (!win) fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    static const char* kGLHelp =
        "  The renderer needs an OpenGL " MP_STR(MP_GL_MAJOR) "." MP_STR(MP_GL_MINOR)
        " core profile. If this host only\n"
        "  offers legacy OpenGL (\"GDI Generic\" 1.1, typical for a VM with no GL driver),\n"
        "  install a GL implementation that provides it -- see docs/dev/graphics.md.";
    if (!ctx) fatal("SDL_GL_CreateContext: %s\n%s", SDL_GetError(), kGLHelp);
    SDL_GL_SetSwapInterval(0);
    // A legacy driver still "loads": it resolves the GL 1.1 exports and leaves every
    // 2.0+ entry point null, so check the version before handing off to the renderer.
    int glver = gl_load_with(SDL_GL_GetProcAddress);
    if (glver < MP_GL_VERSION_MIN) {
        fatal("OpenGL %d.%d is too old (got \"%s\" / \"%s\")\n%s",
              glver / 10, glver % 10, glGetString(GL_VERSION) ? (const char*)glGetString(GL_VERSION) : "?",
              glGetString(GL_RENDERER) ? (const char*)glGetString(GL_RENDERER) : "?", kGLHelp);
    }
    LOG(LOG_GX, "GL: %s / %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
    // Beside the memory card: both are state this machine accumulates for this game.
    plat_make_dirs("saves");
    gx::render_set_shader_cache("saves/shaders.bin");
    gx::render_init(scale);
    if (gx::g_dump_dir) plat_make_dirs(gx::g_dump_dir);

    audio_open();

    uint32_t entry = boot_load(iso);
    threads_start_boot(entry);

    bool running = true;
    uint32_t frames = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            else if (e.type == SDL_CONTROLLERDEVICEADDED && !g_ctrl) g_ctrl = SDL_GameControllerOpen(e.cdevice.which);
            else if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) running = false;
        }
        update_pad();
        int dw, dh;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        gx::render_set_window_size(dw, dh);
        auto b = gx::take_batch(4);
        if (!b) continue;
        if (gx::render_execute(*b)) {
            // macOS (GL on Metal) only presents correctly with the window framebuffer bound.
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            SDL_GL_SwapWindow(win);
            frames++;
            auto now = std::chrono::steady_clock::now();
            double secs = std::chrono::duration<double>(now - t0).count();
            if (secs >= 2.0) {
                char title[128];
                snprintf(title, sizeof(title), "Metroid Prime (recompiled) - %.1f fps", frames / secs);
                SDL_SetWindowTitle(win, title);
                frames = 0;
                t0 = now;
            }
        }
    }
    threads_request_quit();
    plat_exit_now(0);
}
