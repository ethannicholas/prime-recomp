// Metroid Prime's heap, verified. Installed as the runtime's guest check (see
// gcn-recomp/runtime/runtime.h, debug_set_guest_check): built with GCN_GUEST_CHECKS the
// runtime walks the whole heap at every interrupt poll, DMA and frame, so that a
// corrupted block header is reported within a fraction of a frame of the write that did
// it, with the address of the word that is wrong. docs/dev/diagnostics.md, "Heap checks".
//
// What is known about CGameAllocator, from the game's own code (the addresses are this
// disc's, GM8E01 revision 2, from analysis/symbols.txt; nothing here is the game's code):
//
//   Every block carries a 32-byte SGameMemInfo header:
//     +0x00  sentinel 0xEFEFEFEF          +0x10  previous block; bit 0 set = not free
//     +0x04  length of the payload        +0x14  next block (low 5 bits are flags)
//     +0x08  file and line of the caller  +0x18  next free block in the same bin
//     +0x0C  type string                  +0x1C  canary 0xEAEAEAEA
//   The blocks tile the heap: the next header sits at block + 0x20 + length rounded up
//   to 32 (lengths are the sizes asked for), or a little beyond when a split would have
//   left a remainder too small to be a block. The allocator holds the heap size at +0x8
//   and its base at +0xC, and keeps sixteen free lists by size at +0x14..+0x50, threaded
//   through +0x18. CMemory's allocator pointer is `mpAllocator__7CMemory`
//   (the small-data word at r13 - 0x6348), and CGameAllocator's vtable is at 0x803F06F0.
#include "runtime.h"
#include <cstdio>
#include <cstdlib>

namespace {

constexpr uint32_t kAllocatorPtr = 0x805A98F8;
constexpr uint32_t kAllocatorVtable = 0x803F06F0;
constexpr uint32_t kSentinel = 0xEFEFEFEF, kCanary = 0xEAEAEAEA;
constexpr uint32_t kHeader = 0x20;

char g_msg[600];
uint32_t g_first;        // the first block, once found
uint32_t g_max_gap;      // the largest gap seen between a block's end and the next header
bool g_logged;
constexpr uint32_t kMaxGap = 0x40;  // the largest remainder seen is one header; a clobbered length misses by far more

inline bool in_ram(uint32_t a) { return a >= 0x80000000u && a - 0x80000000u < RAM_SIZE; }

const char* fail(const char* fmt, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e = 0) {
    snprintf(g_msg, sizeof(g_msg), fmt, a, b, c, d, e);
    return g_msg;
}

// The first block is the one with no predecessor: the header at the heap base (+0xC),
// or thereabouts. Walk back from a free block if that fails.
uint32_t find_first(uint32_t alloc) {
    const uint32_t base = mem_r32(alloc + 0xC);
    const int32_t offs[] = {0, -0x20, -0x40, 0x20, 0x40};
    for (int32_t o : offs) {
        uint32_t b = base + (uint32_t)o;
        if (in_ram(b) && mem_r32(b) == kSentinel && mem_r32(b + 0x1C) == kCanary && (mem_r32(b + 0x10) & ~0x1Fu) == 0) return b;
    }
    for (int i = 0; i < 16; i++) {
        uint32_t node = mem_r32(alloc + 0x14 + 4 * i) & ~0x1Fu;
        for (int n = 0; node && in_ram(node) && n < 1000000; n++) {
            uint32_t prev = mem_r32(node + 0x10) & ~0x1Fu;
            if (!prev) return node;
            node = prev;
        }
    }
    return 0;
}

const char* heap_check() {
    const uint32_t alloc = mem_r32(kAllocatorPtr);
    if (!in_ram(alloc) || mem_r32(alloc) != kAllocatorVtable) {  // not up yet
        return nullptr;
    }
    if (!g_first) {
        g_first = find_first(alloc);
        if (!g_first) {
            static bool said;
            if (!said && getenv("GCN_HEAPLOG")) {
                said = true;
                fprintf(stderr, "[heap] allocator %08X: no first block found; words +0..+0x1C:", alloc);
                for (uint32_t o = 0; o < 0x20; o += 4) fprintf(stderr, " %08X", mem_r32(alloc + o));
                fprintf(stderr, "\n");
            }
            return nullptr;
        }
    }

    // The block list.
    uint32_t blk = g_first, prev = 0, n = 0, flagged = 0;
    while (blk) {
        if (!in_ram(blk) || (blk & 0x1F))
            return fail("block list: #%u after %08X points at %08X (link word %08X)", n, prev, blk, prev ? prev + 0x14 : alloc + 0x8);
        if (mem_r32(blk) != kSentinel)
            return fail("block %08X (#%u, after %08X): sentinel %08X", blk, n, prev, mem_r32(blk));
        if (mem_r32(blk + 0x1C) != kCanary)
            return fail("block %08X (#%u): canary at %08X is %08X", blk, n, blk + 0x1C, mem_r32(blk + 0x1C));
        const uint32_t len = mem_r32(blk + 0x4);
        if (len > RAM_SIZE || blk + kHeader + len < blk)
            return fail("block %08X (#%u): length at %08X is %08X", blk, n, blk + 0x4, len);
        const uint32_t p = mem_r32(blk + 0x10);
        if ((p & ~0x1Fu) != prev)
            return fail("block %08X (#%u): prev link at %08X is %08X, expected the previous block", blk, n, blk + 0x10, p);
        if (p & 1) flagged++;
        const uint32_t next = mem_r32(blk + 0x14) & ~0x1Fu;
        if (next) {
            if (next <= blk)
                return fail("block %08X (#%u): next link at %08X is %08X, not after it", blk, n, blk + 0x14, next);
            const uint32_t end = (blk + kHeader + len + 31) & ~31u;
            if (next < end || next - end > kMaxGap)
                return fail("block %08X (#%u): length at %08X is %08X, which does not fit the next block at %08X", blk, n, blk + 0x4, len, next);
            if (next - end > g_max_gap) g_max_gap = next - end;
        }
        prev = blk;
        blk = next;
        if (++n > 4000000) return fail("block list from %08X does not terminate (%u blocks, at %08X)", g_first, n, blk, 0);
    }

    // The free lists. Every node must be a free block with an intact header.
    uint32_t bin_nodes[16] = {};
    for (uint32_t i = 0; i < 16; i++) {
        uint32_t node = mem_r32(alloc + 0x14 + 4 * i) & ~0x1Fu, last = alloc + 0x14 + 4 * i;
        while (node) {
            if (!in_ram(node) || (node & 0x1F))
                return fail("free list %u: link at %08X points at %08X", i, last, node, 0);
            if (mem_r32(node) != kSentinel || mem_r32(node + 0x1C) != kCanary)
                return fail("free list %u: node %08X has sentinel %08X canary %08X", i, node, mem_r32(node), mem_r32(node + 0x1C));
            if (mem_r32(node + 0x10) & 1)
                return fail("free list %u: node %08X is flagged in use (word %08X = %08X)", i, node, node + 0x10, mem_r32(node + 0x10));
            last = node + 0x18;
            node = mem_r32(node + 0x18) & ~0x1Fu;
            if (++bin_nodes[i] > 4000000) return fail("free list %u from %08X does not terminate", i, mem_r32(alloc + 0x14 + 4 * i), 0, 0);
        }
    }

    if (!g_logged) {
        g_logged = true;
        if (getenv("GCN_HEAPLOG")) {
            fprintf(stderr, "[heap] allocator %08X, first block %08X: %u blocks, %u flagged in use; bins", alloc, g_first, n, flagged);
            for (uint32_t i = 0; i < 16; i++) fprintf(stderr, " %u", bin_nodes[i]);
            fprintf(stderr, "\n");
        }
    }
    if (getenv("GCN_HEAPLOG")) {
        static uint32_t reported_gap;
        if (g_max_gap > reported_gap) {
            reported_gap = g_max_gap;
            fprintf(stderr, "[heap] %u blocks; largest gap between a block's end and the next header so far: 0x%X\n", n, g_max_gap);
        }
    }
    return nullptr;
}

struct Install {
    Install() { debug_set_guest_check(heap_check); }
} g_install;

}  // namespace
