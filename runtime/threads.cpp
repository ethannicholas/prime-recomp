// Guest threads, context switching and interrupt delivery.
//
// The SDK scheduler (SelectThreadToRun etc.) is recompiled unmodified. We HLE only
// OSSaveContext / OSLoadContext:
//   * Each guest execution stream runs on its own host thread; exactly one host
//     thread runs guest code at a time (a baton is handed over on switch).
//   * OSSaveContext(ctx) records a "binding" ctx -> (host thread, jmp_buf, CPU
//     snapshot). The jmp_buf was filled by setjmp at the call site (the recompiler
//     emits that for calls to OSSaveContext), so resuming = longjmp there with r3=1.
//   * Interrupt delivery at a safe point binds the current OSContext in the same way
//     (setjmp in irq_deliver) and runs the SDK's exception handler.
//   * OSLoadContext(ctx) resumes whichever host thread owns the binding (possibly
//     ourselves), or spawns a new host thread for a never-run context.
#include "runtime.h"
#include "platform.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

volatile int g_irq_pending;

enum BindKind { BIND_NONE, BIND_SAVED, BIND_EXC };

struct Binding {
    HostThread* h = nullptr;
    BindKind kind = BIND_NONE;
    jmp_buf jb;
    CPU snapshot;
};

struct HostThread {
    CPU cpu;
    int id;
    std::mutex m;
    std::condition_variable cv;
    bool run = false;
    Binding* resume = nullptr;   // set by whoever wakes us
    uint32_t start_ctx = 0;      // for fresh threads
};

static thread_local HostThread* t_self;
static std::unordered_map<uint32_t, Binding> g_bindings;
static std::vector<HostThread*> g_threads;
static std::atomic<bool> g_quit{false};

HostThread* host_current() { return t_self; }
CPU* cpu_current() { return t_self ? &t_self->cpu : nullptr; }

// OSContext layout
enum { CTX_GPR = 0x000, CTX_CR = 0x080, CTX_LR = 0x084, CTX_CTR = 0x088, CTX_XER = 0x08C,
       CTX_FPR = 0x090, CTX_FPSCR = 0x194, CTX_SRR0 = 0x198, CTX_SRR1 = 0x19C,
       CTX_MODE = 0x1A0, CTX_STATE = 0x1A2, CTX_GQR = 0x1A4, CTX_PSF = 0x1C8 };
enum { OS_CONTEXT_STATE_EXC = 0x02 };

static void cpu_to_context(const CPU* c, uint32_t ctx, uint32_t srr0) {
    for (int i = 0; i < 32; i++) mem_w32(ctx + CTX_GPR + 4 * i, c->r[i]);
    mem_w32(ctx + CTX_CR, cpu_get_cr(c));
    mem_w32(ctx + CTX_LR, c->lr);
    mem_w32(ctx + CTX_CTR, c->ctr);
    mem_w32(ctx + CTX_XER, ((uint32_t)c->xer_so << 31) | ((uint32_t)c->xer_ov << 30) | ((uint32_t)c->xer_ca << 29));
    mem_w32(ctx + CTX_SRR0, srr0);
    mem_w32(ctx + CTX_SRR1, c->msr);
    for (int i = 0; i < 8; i++) mem_w32(ctx + CTX_GQR + 4 * i, c->gqr[i]);
}

static void context_to_cpu(CPU* c, uint32_t ctx) {
    for (int i = 0; i < 32; i++) c->r[i] = mem_r32(ctx + CTX_GPR + 4 * i);
    cpu_set_cr(c, mem_r32(ctx + CTX_CR));
    c->lr = mem_r32(ctx + CTX_LR);
    c->ctr = mem_r32(ctx + CTX_CTR);
    uint32_t xer = mem_r32(ctx + CTX_XER);
    c->xer_so = xer >> 31; c->xer_ov = (xer >> 30) & 1; c->xer_ca = (xer >> 29) & 1;
    for (int i = 0; i < 32; i++) {
        uint64_t v = ((uint64_t)mem_r32(ctx + CTX_FPR + 8 * i) << 32) | mem_r32(ctx + CTX_FPR + 8 * i + 4);
        c->f[i].u = v;
        uint64_t p = ((uint64_t)mem_r32(ctx + CTX_PSF + 8 * i) << 32) | mem_r32(ctx + CTX_PSF + 8 * i + 4);
        memcpy(&c->ps1[i], &p, 8);
    }
    c->fpscr = mem_r32(ctx + CTX_FPSCR);
    c->msr = mem_r32(ctx + CTX_SRR1);
    for (int i = 0; i < 8; i++) c->gqr[i] = mem_r32(ctx + CTX_GQR + 4 * i);
}

// ---------------------------------------------------------------------------
// Baton passing
// ---------------------------------------------------------------------------
static void* host_thread_main(void* arg);

static HostThread* new_host_thread() {
    HostThread* h = new HostThread();
    h->id = (int)g_threads.size();
    h->cpu.host = h;
    g_threads.push_back(h);
    return h;
}

// Guest code recurses deeply and the SDK gives its threads generous stacks, so give
// each host thread 64MB rather than the platform default.
static constexpr size_t HOST_STACK = 64 << 20;

static void spawn(HostThread* h) {
    if (!plat_thread_start(host_thread_main, h, HOST_STACK))
        fatal("could not start host thread %d", h->id);
}

static void wake(HostThread* h, Binding* b) {
    std::lock_guard<std::mutex> lk(h->m);
    h->resume = b;
    h->run = true;
    h->cv.notify_one();
}

// Block the current host thread until someone hands us the baton. Returns the
// binding we should resume at (or nullptr for a fresh start).
static Binding* wait_for_baton(HostThread* self) {
    std::unique_lock<std::mutex> lk(self->m);
    self->cv.wait(lk, [&] { return self->run || g_quit.load(); });
    if (g_quit) { lk.unlock(); plat_thread_exit(); }
    return self->resume;
}

[[noreturn]] static void resume_binding(HostThread* self, Binding* b) {
    self->cpu = b->snapshot;
    self->cpu.host = self;
    if (b->kind == BIND_SAVED) self->cpu.r[3] = 1;
    b->kind = BIND_NONE;
    g_irq_pending = 1;  // re-evaluate pending interrupts after a switch
    longjmp(b->jb, 1);
}

// Hand the baton to `target` (resuming at binding b) and block until we get it back.
[[noreturn]] static void switch_to(HostThread* self, HostThread* target, Binding* b) {
    {
        std::lock_guard<std::mutex> lk(self->m);
        self->run = false;
        self->resume = nullptr;
    }
    wake(target, b);
    Binding* mine = wait_for_baton(self);
    if (!mine) fatal("thread %d woken without resume point", self->id);
    resume_binding(self, mine);
}

static void* host_thread_main(void* arg) {
    HostThread* self = (HostThread*)arg;
    t_self = self;
    Binding* b = wait_for_baton(self);
    if (b) fatal("fresh host thread %d asked to resume a binding", self->id);
    CPU* c = &self->cpu;
    uint32_t pc = mem_r32(self->start_ctx + CTX_SRR0);
    context_to_cpu(c, self->start_ctx);
    mem_w16(self->start_ctx + CTX_STATE, mem_r16(self->start_ctx + CTX_STATE) & ~OS_CONTEXT_STATE_EXC);
    LOG(LOG_THREAD, "host thread %d starting guest context %08X at %08X", self->id, self->start_ctx, pc);
    call_indirect(c, pc);
    for (;;) call_indirect(c, c->lr);  // thread function returned: continue at LR (OSExitThread)
    return nullptr;
}

// ---------------------------------------------------------------------------
// HLE: OSSaveContext / OSLoadContext
// ---------------------------------------------------------------------------
extern "C" jmp_buf* hle_context_jmpbuf(CPU* c) {
    // Called at an OSSaveContext call site before the call; r3 = context.
    return &g_bindings[c->r[3]].jb;
}

extern "C" void orig_OSSaveContext(CPU* c);
extern "C" void hle_OSSaveContext(CPU* c) {
    uint32_t ctx = c->r[3];
    orig_OSSaveContext(c);  // writes the context to guest memory, returns r3 = 0
    Binding& b = g_bindings[ctx];
    b.h = t_self;
    b.kind = BIND_SAVED;
    b.snapshot = *c;
    LOG(LOG_THREAD, "save ctx %08X on host %d", ctx, t_self->id);
}

extern "C" void hle_OSLoadContext(CPU* c) {
    uint32_t ctx = c->r[3];
    HostThread* self = t_self;
    // Clear the exception flag like the real OSLoadContext does.
    mem_w16(ctx + CTX_STATE, mem_r16(ctx + CTX_STATE) & ~OS_CONTEXT_STATE_EXC);
    auto it = g_bindings.find(ctx);
    if (it != g_bindings.end() && it->second.kind != BIND_NONE) {
        Binding* b = &it->second;
        LOG(LOG_THREAD, "load ctx %08X (host %d -> %d)", ctx, self->id, b->h->id);
        if (b->h == self) resume_binding(self, b);
        switch_to(self, b->h, b);
    }
    // Never-run context: start a new host thread for it.
    HostThread* h = new_host_thread();
    h->start_ctx = ctx;
    LOG(LOG_THREAD, "load fresh ctx %08X -> new host thread %d", ctx, h->id);
    spawn(h);
    switch_to(self, h, nullptr);
}

// ---------------------------------------------------------------------------
// Decrementer
// ---------------------------------------------------------------------------
static uint64_t g_dec_base_tick;
static uint32_t g_dec_value;
static std::atomic<uint64_t> g_dec_deadline{UINT64_MAX};
static bool g_dec_pending;

void dec_write(CPU* c, uint32_t v) {
    g_dec_base_tick = now_ticks();
    g_dec_value = v;
    g_dec_pending = false;
    // Exception fires when the decrementer passes from 0 to -1.
    g_dec_deadline = (v & 0x80000000u) ? UINT64_MAX : g_dec_base_tick + (uint64_t)v + 1;
}

uint32_t dec_read(CPU* c) {
    return g_dec_value - (uint32_t)(now_ticks() - g_dec_base_tick);
}

// ---------------------------------------------------------------------------
// Interrupts
// ---------------------------------------------------------------------------
extern std::atomic<uint32_t> g_pi_intsr, g_pi_intmr;
extern std::atomic<uint64_t> g_next_event_at;

static void irq_deliver(CPU* c, uint32_t exc) {
    uint32_t ctx = mem_r32(0x800000D4);  // OSCurrentContext
    if (!ctx || (ctx & 0x3FFFFFFF) >= RAM_SIZE) fatal("interrupt with bad current context %08X (exc %u)", ctx, exc);
    Binding* b = &g_bindings[ctx];
    b->h = t_self;
    b->kind = BIND_EXC;
    b->snapshot = *c;
    cpu_to_context(c, ctx, 0);
    mem_w16(ctx + CTX_STATE, mem_r16(ctx + CTX_STATE) | OS_CONTEXT_STATE_EXC);
    if (setjmp(b->jb) != 0) return;  // resumed by OSLoadContext(ctx)
    c->spr[26] = 0;          // SRR0
    c->spr[27] = c->msr;     // SRR1
    c->msr &= ~MSR_EE;
    c->r[3] = exc;
    c->r[4] = ctx;
    uint32_t handler = mem_r32(0x80003000 + exc * 4);
    call_indirect(c, handler);
    fatal("exception handler %08X for exception %u returned", handler, exc);
}

extern "C" void irq_poll(CPU* c) {
    g_irq_pending = 0;
    if (g_quit) plat_thread_exit();
    events_run_due();
    uint64_t now = now_ticks();
    if (g_dec_deadline.load() <= now) { g_dec_pending = true; g_dec_deadline = UINT64_MAX; }
    bool ext = (g_pi_intsr.load() & g_pi_intmr.load()) != 0;
    if (!ext && !g_dec_pending) return;
    if (!(c->msr & MSR_EE)) { g_irq_pending = 1; return; }
    if (ext) { LOG(LOG_THREAD, "deliver external irq: intsr=%08X intmr=%08X", g_pi_intsr.load(), g_pi_intmr.load()); irq_deliver(c, 4); return; }
    g_dec_pending = false;
    irq_deliver(c, 8);
}

// Background ticker: raises the poll flag when a timed event or the decrementer is due.
static void ticker_main() {
    while (!g_quit) {
        uint64_t now = now_ticks();
        if (g_next_event_at.load() <= now || g_dec_deadline.load() <= now) g_irq_pending = 1;
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

void threads_start_boot(uint32_t entry) {
    static std::thread ticker(ticker_main);
    ticker.detach();
    HostThread* h = new_host_thread();
    // Fake a context-free start: run entry directly on a host thread.
    struct Boot { static void* main(void* arg) {
        HostThread* self = (HostThread*)arg;
        t_self = self;
        CPU* c = &self->cpu;
        c->msr = 0x00002032;  // FP, IR, DR, RI (EE off)
        call_indirect(c, self->start_ctx);
        LOG(LOG_THREAD, "boot entry returned");
        return nullptr;
    } };
    h->start_ctx = entry;
    if (!plat_thread_start(Boot::main, h, HOST_STACK))
        fatal("could not start the boot thread");
}

void threads_request_quit() {
    g_quit = true;
    for (auto* h : g_threads) { std::lock_guard<std::mutex> lk(h->m); h->cv.notify_all(); }
}

// ---------------------------------------------------------------------------
// Debug: dump guest thread state
// ---------------------------------------------------------------------------
const char* func_name(uint32_t addr);

void debug_dump_fifo();
void debug_print_counts();
void debug_dump_threads() {
    debug_dump_fifo();
    if (const char* e = getenv("MP_PEEK")) {
        std::string s = e;
        size_t p = 0;
        while (p < s.size()) {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            uint32_t a = (uint32_t)strtoul(s.substr(p, q - p).c_str(), nullptr, 16);
            fprintf(stderr, "[peek] %08X = %08X\n", a, mem_r32(a));
            p = q + 1;
        }
    }
#ifdef MP_WATCH
    debug_print_counts();
#endif
    for (auto* h : g_threads) {
        CPU* c = &h->cpu;
        fprintf(stderr, "=== host thread %d (run=%d) lr=%08X msr=%08X r1=%08X r3=%08X\n", h->id, h->run, c->lr, c->msr, c->r[1], c->r[3]);
#ifdef MP_CALL_TRACE
        fprintf(stderr, "  guest call stack (innermost first):\n");
        uint32_t want = getenv("MP_TRACE_DEPTH") ? (uint32_t)atoi(getenv("MP_TRACE_DEPTH")) : 40;
        // The depth counts past the end of the array so that deep recursion still unwinds
        // correctly; anything above 256 was never recorded.
        uint32_t have = c->depth < 256 ? c->depth : 256;
        if (c->depth > 256)
            fprintf(stderr, "    (%u frames deeper than the stack records)\n", c->depth - 256);
        if (want > have) want = have;
        for (uint32_t i = 1; i <= want; i++) {
            uint32_t a = c->stack[have - i];
            fprintf(stderr, "    %08X %s\n", a, func_name(a));
        }
#else
        fprintf(stderr, "  (built without MP_TRACE_CALLS, so there is no guest call stack)\n");
#endif
    }
}

#ifdef MP_WATCH
// Debug build only: MP_WATCH_ADDR=<hex guest address> reports every change to that word.
static std::vector<std::pair<uint32_t, uint64_t>> g_count_fns = [] {
    std::vector<std::pair<uint32_t, uint64_t>> v;
    if (const char* e = getenv("MP_COUNT")) {
        std::string s = e;
        size_t p = 0;
        while (p < s.size()) {
            size_t q = s.find(',', p);
            if (q == std::string::npos) q = s.size();
            v.push_back({(uint32_t)strtoul(s.substr(p, q - p).c_str(), nullptr, 16), 0});
            p = q + 1;
        }
    }
    return v;
}();
void debug_print_counts() {
    for (auto& kv : g_count_fns) fprintf(stderr, "[count] %08X %s: %llu\n", kv.first, func_name(kv.first), (unsigned long long)kv.second);
}
extern "C" void debug_watch_check(CPU* c, uint32_t fn) {
    for (auto& kv : g_count_fns) if (kv.first == fn) kv.second++;
    static uint32_t addr = [] { const char* e = getenv("MP_WATCH_ADDR"); return e ? (uint32_t)strtoul(e, nullptr, 16) : 0u; }();
    static uint32_t last = 0xFFFFFFFF;
    if (!addr) return;
    uint32_t v = mem_r32(addr);
    if (v != last) {
        fprintf(stderr, "[watch] %08X: %08X -> %08X at %s %08X %s (lr %08X)\n", addr, last, v,
                (fn & 1) ? "store" : "entry", fn & ~1u, (fn & 1) ? "" : func_name(fn), c->lr);
        last = v;
    }
}
#endif

// Periodically report which game-code functions (below the SDK at 0x80100000) are active.
uint32_t gx_frames_submitted();
void debug_sampler_start() {
    std::thread([] {
        extern std::atomic<uint32_t> g_vi_retrace_count;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            for (auto* h : g_threads) {
                CPU* c = &h->cpu;
                // Walk the guest's own stack: the PowerPC ABI keeps the caller's frame
                // pointer at [sp] and its return address at [sp+4], so this is a real
                // guest backtrace read straight out of guest memory. It needs no
                // instrumentation at all, which is why it, rather than anything ENTER()
                // keeps, is what --sample reports.
                std::string bt, s;
                uint32_t seen[8]; int ns = 0;
                uint32_t sp = c->r[1];
                for (int d = 0; d < 24 && sp >= 0x80000000u && sp < 0x81800000u; d++) {
                    uint32_t prev = mem_r32(sp);
                    if (prev < 0x80000000u || prev >= 0x81800000u || prev <= sp) break;
                    uint32_t ret = mem_r32(prev + 4);
                    char buf[16]; snprintf(buf, sizeof(buf), " %08X", ret); bt += buf;
                    // Game code lives below the SDK; name the distinct ones.
                    if (ret >= 0x80006000u && ret < 0x80100000u && ns < 8) {
                        bool dup = false;
                        for (int k = 0; k < ns; k++) dup |= seen[k] == ret;
                        if (!dup) {
                            seen[ns++] = ret;
                            char b2[24]; snprintf(b2, sizeof(b2), " %08X", ret); s += b2;
                        }
                    }
                    sp = prev;
                }
                static uint32_t last_frames;
                uint32_t fr = gx_frames_submitted();
                fprintf(stderr, "[sample] vi=%u frames=%u (+%u) thread %d game fns:%s\n", g_vi_retrace_count.load(), fr, fr - last_frames, h->id, s.c_str());
                last_frames = fr;
                fprintf(stderr, "         guest stack:%s\n", bt.c_str());
            }
        }
    }).detach();
}
