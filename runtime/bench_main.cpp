// Headless benchmark harness: runs the recompiled game with no graphics, audio or
// input and reports how fast the guest actually advances.
//
// This exists to compare host CPUs on equal terms. The renderer and the SDL frontend
// are left out of this target entirely (see CMakeLists), so it builds and runs
// anywhere the recompiled code builds -- including an Android/arm64 device over adb,
// which is how we judge whether a standalone headset build could sustain the game's
// 60 fps before committing to an OpenGL ES port.
//
// Usage: prime_bench [--seconds=N] [--warmup=N] [path/to/game.iso]
//
// The first several seconds are boot and asset loading, which run at a different rate
// than gameplay and would otherwise skew the average. Those are reported but excluded
// from the steady-state figure, so numbers from two devices are comparable.
#include "runtime.h"
#include "platform.h"
#include "gx/render.h"
#include "input_script.h"
#include "hw/dtk.h"
#include "hw/pad.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

uint32_t boot_load(const char* iso_path);
void debug_dump_threads();

// The audio subsystem this target omits is stubbed in audio_stub.cpp.

// ---------------------------------------------------------------------------
static void on_interrupt() {
    plat_watchdog(2, 2);
    debug_dump_threads();
    plat_exit_now(1);
}

static void on_fault(const void* fault_addr, int code) {
    uintptr_t a = (uintptr_t)fault_addr;
    if (a >= (uintptr_t)g_mem && a < (uintptr_t)g_mem + 0x40000000)
        fprintf(stderr, "\nFAULT: guest memory access at (addr & 0x3FFFFFFF) = %08lX\n",
                (unsigned long)(a - (uintptr_t)g_mem));
    else
        fprintf(stderr, "\nFAULT: host address %p (code %d)\n", fault_addr, code);
    plat_watchdog(2, 2);
    plat_backtrace_print();
    debug_dump_threads();
    plat_exit_now(1);
}

int main(int argc, char** argv) {
    std::string iso = MP_DEFAULT_ISO;
    int seconds = 30;
    int warmup = 8;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "--seconds=", 10)) seconds = atoi(argv[i] + 10);
        else if (!strncmp(argv[i], "--warmup=", 9)) warmup = atoi(argv[i] + 9);
        else if (argv[i][0] != '-') iso = argv[i];
    }
    if (!plat_readable(iso.c_str())) iso = plat_find_file("rom", ".iso");
    if (!plat_readable(iso.c_str())) {
        fprintf(stderr, "No game image found. Pass the path to your .iso.\n");
        return 1;
    }

    // Quiet: per-frame DVD logging would dominate the measurement.
    for (auto& e : g_log_enabled) e = false;
    plat_install_crash_handlers(on_interrupt, on_fault);

    mem_init();
    timing_init();
    input_script_init();
    uint32_t entry = boot_load(iso.c_str());
    threads_start_boot(entry);

    printf("benchmarking %s for %d s\n", iso.c_str(), seconds);
    printf("  time   frames/s   verts/s   draws/s\n");
    fflush(stdout);

    using clock = std::chrono::steady_clock;
    const auto t_start = clock::now();
    auto t_mark = t_start;
    uint32_t frames_at_mark = 0;
    uint64_t verts = 0, verts_at_mark = 0;
    uint64_t draws = 0, draws_at_mark = 0;

    // Steady-state accounting begins once warmup has elapsed.
    bool warmed = false;
    double t_warmed = 0;
    uint32_t frames_warmed = 0;
    uint64_t verts_warmed = 0;

    for (;;) {
        // Keep a controller plugged in so the game doesn't sit in a "please connect"
        // state, and drain batches so the guest never blocks on a renderer.
        PadState p;
        p.connected = true;
        input_script_apply(p);
        pad_set_state(0, p);

        if (auto b = gx::take_batch(5)) {
            verts += b->verts.size();
            draws += b->cmds.size();
        }

        const auto now = clock::now();
        const double elapsed = std::chrono::duration<double>(now - t_start).count();
        const double since_mark = std::chrono::duration<double>(now - t_mark).count();

        if (!warmed && elapsed >= warmup) {
            warmed = true;
            t_warmed = elapsed;
            frames_warmed = gx::g_frames_submitted.load();
            verts_warmed = verts;
        }

        if (since_mark >= 1.0) {
            const uint32_t frames = gx::g_frames_submitted.load();
            printf("  %4.0fs   %8.1f   %7.0fk   %7.0f%s\n", elapsed,
                   (frames - frames_at_mark) / since_mark,
                   (verts - verts_at_mark) / since_mark / 1000.0,
                   (draws - draws_at_mark) / since_mark,
                   warmed ? "" : "   (warmup)");
            fflush(stdout);
            frames_at_mark = frames;
            verts_at_mark = verts;
            draws_at_mark = draws;
            t_mark = now;
        }
        if (elapsed >= seconds) break;
    }

    const double elapsed = std::chrono::duration<double>(clock::now() - t_start).count();
    const uint32_t frames = gx::g_frames_submitted.load();
    printf("\nwhole run:    %u frames in %.1f s = %.2f fps\n", frames, elapsed, frames / elapsed);
    if (warmed && elapsed > t_warmed) {
        const double sd = elapsed - t_warmed;
        const double fps = (frames - frames_warmed) / sd;
        printf("steady state: %u frames in %.1f s = %.2f fps = %.0f%% of the game's 60 fps\n",
               frames - frames_warmed, sd, fps, 100.0 * fps / 60.0);
        printf("              %.0fk vertices/s\n", (verts - verts_warmed) / sd / 1000.0);
    } else {
        printf("steady state: not measured (warmup %ds was not shorter than the %ds run)\n",
               warmup, seconds);
    }
    fflush(stdout);

    // Guest threads are still running in longjmp-based contexts; don't unwind them.
    plat_exit_now(0);
}
