// Host OS services that differ between POSIX and Windows. Implemented in platform.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// ---- virtual memory ----
// Reserve `size` bytes of address space without committing it. Returns nullptr on failure.
void* plat_reserve(size_t size);
// Make [base, base+size) readable and writable within an existing reservation.
bool plat_commit(void* base, size_t size);

// ---- diagnostics ----
// Print a symbolized backtrace of the calling thread to stderr.
void plat_backtrace_print();
// Install handlers for interactive interrupt and for memory faults. `on_fault` is
// passed the faulting address (0 if unknown); both callbacks must not return.
void plat_install_crash_handlers(void (*on_interrupt)(), void (*on_fault)(const void* addr, int sig));
// Hard-exit after `seconds` no matter what, so a crash handler can never hang.
void plat_watchdog(int seconds, int exit_code);
// Record a fatal message where the platform keeps crash reports, which on Android is the
// one place the app's own logging cannot overwrite it. See the definition.
void plat_record_fatal(const char* msg);
// Exit immediately without running destructors or flushing other threads' buffers.
[[noreturn]] void plat_exit_now(int code);

// ---- threads ----
// Start a thread with a `stack_size`-byte stack (reserved, not committed).
bool plat_thread_start(void* (*fn)(void*), void* arg, size_t stack_size);
// Terminate the calling thread. Used when the process is shutting down, so skipping
// destructor/unwind cleanup is intentional.
[[noreturn]] void plat_thread_exit();

// ---- time ----
// Seconds to add to UTC to get local wall-clock time, including any DST in effect.
long plat_utc_offset_seconds();

// ---- files ----
bool plat_readable(const char* path);
// Create `path` and any missing parents. Ignores already-exists.
void plat_make_dirs(const std::string& path);
// Replace `to` with `from`, overwriting an existing file.
bool plat_replace_file(const char* from, const char* to);
// First entry in `dir` whose name ends with `suffix` (case-insensitive), or "".
std::string plat_find_file(const char* dir, const char* suffix);

// A read-only file supporting concurrent positional reads.
struct PlatFile;
PlatFile* plat_open_read(const char* path);
bool plat_read_at(PlatFile* f, uint64_t offset, void* dst, uint32_t len);
