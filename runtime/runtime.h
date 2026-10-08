// Internal runtime interfaces (C++).
#pragma once
#include "recomp.h"
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <functional>

// ---- logging ----
enum LogCat { LOG_CPU, LOG_OS, LOG_HW, LOG_DVD, LOG_GX, LOG_VI, LOG_SI, LOG_EXI, LOG_DSP, LOG_AI, LOG_THREAD, LOG_COUNT };
extern bool g_log_enabled[LOG_COUNT];
void log_msg(LogCat cat, const char* fmt, ...) MP_PRINTF_FMT(2, 3);
[[noreturn]] void fatal(const char* fmt, ...) MP_PRINTF_FMT(1, 2);
#define LOG(cat, ...) do { if (g_log_enabled[cat]) log_msg(cat, __VA_ARGS__); } while (0)

// ---- memory ----
constexpr uint32_t RAM_SIZE = 0x01800000;
constexpr uint32_t ARAM_SIZE = 0x01000000;
constexpr uint32_t LC_BASE = 0x20000000;   // locked cache (guest 0xE0000000)
void mem_init();
extern uint8_t* g_aram;

// Where the memory card lives. Null means the working directory, which is right for a
// desktop run started from the repository. An app has no useful working directory -- on
// Android it is "/", which is not writable -- so a frontend that knows one sets this
// before the guest boots.
extern const char* g_save_dir;
inline uint8_t* mem_ptr(uint32_t a) { return HOST(a); }
inline uint32_t mem_r32(uint32_t a) { uint32_t v; memcpy(&v, HOST(a), 4); return MP_BSWAP32(v); }
inline uint16_t mem_r16(uint32_t a) { uint16_t v; memcpy(&v, HOST(a), 2); return MP_BSWAP16(v); }
inline uint8_t mem_r8(uint32_t a) { return *HOST(a); }
inline void mem_w32(uint32_t a, uint32_t v) { v = MP_BSWAP32(v); memcpy(HOST(a), &v, 4); }
inline void mem_w16(uint32_t a, uint16_t v) { v = MP_BSWAP16(v); memcpy(HOST(a), &v, 2); }
inline void mem_w8(uint32_t a, uint8_t v) { *HOST(a) = v; }
// Physical address (as used by DMA engines) -> host pointer into main RAM.
inline uint8_t* phys_ptr(uint32_t pa) { return g_mem + (pa & 0x01FFFFFF); }

// How much of a DMA at physical address `pa` fits inside main memory.
//
// The engines take their address and their length from registers the guest writes, and
// nothing downstream checked either: phys_ptr() masks to 32 MB against 24 MB of RAM, so
// a transfer aimed past the end writes over whatever is there rather than faulting. What
// that looks like is a crash minutes later somewhere unrelated -- OSAllocFromHeap
// walking a free list whose `next` field now holds a word of someone else's data.
//
// Returns how many bytes may be transferred, and says so the first few times that is
// fewer than asked for, naming the engine. Clamping cannot rescue a transfer that was
// already aimed at the wrong place; what it does is keep the mistake inside the
// transfer, and make it visible where it happens instead of where it is noticed.
uint32_t dma_fit(const char* engine, uint32_t pa, uint32_t len);

// ---- timing ----
constexpr uint64_t TB_FREQ = 40500000;  // timebase / decrementer ticks per second
uint64_t now_ticks();                   // monotonic, TB units
void timing_init();

// Scheduled hardware events, run on the guest thread holding the baton.
using EventFn = std::function<void()>;
void event_schedule(uint64_t at_ticks, EventFn fn);
void event_schedule_in(uint64_t delta_ticks, EventFn fn);
void events_run_due();

// ---- interrupts (PI) ----
enum : uint32_t {
    INT_PI = 0x1, INT_RSW = 0x2, INT_DI = 0x4, INT_SI = 0x8, INT_EXI = 0x10, INT_AI = 0x20,
    INT_DSP = 0x40, INT_MEM = 0x80, INT_VI = 0x100, INT_PE_TOKEN = 0x200, INT_PE_FINISH = 0x400,
    INT_CP = 0x800, INT_DEBUG = 0x1000, INT_HSP = 0x2000,
};
void pi_set_interrupt(uint32_t cause, bool set);
void pi_update();
uint32_t pi_read32(uint32_t off);
void pi_write32(uint32_t off, uint32_t v);

// ---- threads ----
struct HostThread;
HostThread* host_current();
CPU* cpu_current();
void threads_start_boot(uint32_t entry);
void threads_request_quit();

// ---- HLE ----
void hle_os_report(CPU* c);
std::string guest_format(CPU* c, uint32_t fmt_addr, int first_gpr, int first_fpr);
std::string guest_str(uint32_t addr, size_t max = 4096);

// Names a guest address. func_name wants a function's first instruction; func_containing
// takes any address inside one, which is what a return address off the stack is.
const char* func_name(uint32_t addr);
const char* func_containing(uint32_t addr, uint32_t* start);

// ---- hw modules ----
void vi_init(); uint16_t vi_read16(uint32_t off); void vi_write16(uint32_t off, uint16_t v);
void di_init(const char* iso_path); uint32_t di_read32(uint32_t off); void di_write32(uint32_t off, uint32_t v);
void si_init(); uint32_t si_read32(uint32_t off); void si_write32(uint32_t off, uint32_t v);
void exi_init(); uint32_t exi_read32(uint32_t off); void exi_write32(uint32_t off, uint32_t v);
void ai_init(); uint32_t ai_read32(uint32_t off); void ai_write32(uint32_t off, uint32_t v);
void dsp_init(); uint16_t dsp_read16(uint32_t off); void dsp_write16(uint32_t off, uint16_t v);
void cp_init(); uint16_t cp_read16(uint32_t off); void cp_write16(uint32_t off, uint16_t v);
uint16_t pe_read16(uint32_t off); void pe_write16(uint32_t off, uint16_t v);
void gp_write8(uint8_t v); void gp_write16(uint16_t v); void gp_write32(uint32_t v);
uint32_t efb_peek(uint32_t addr); void efb_poke(uint32_t addr, uint32_t v);

// iso reading
bool iso_read(uint64_t offset, void* dst, uint32_t len);
