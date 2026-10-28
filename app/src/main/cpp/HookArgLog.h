#pragma once
#include <cstdint>

// Feature 8.2 — Per-hook argument logging.
//
// A HookArgLog is attached to every ChainHook and ReturnMod hook site.
// When enabled, the site's dispatch helper captures the argument
// registers the target received (a snapshot taken by the naked dispatcher
// BEFORE any C++ code runs — no clobber risk) into a per-site ring buffer
// of the last N calls. Opt-in; zero overhead when disabled (single atomic
// bool load per dispatch).
//
// Scope:
//   - ChainHook  → GP + FP capture (its naked stub saves x0-x7 and q0-q7)
//   - ReturnMod  → GP only    (its stub saves x0-x7; q0-q7 not on frame,
//                              and adding them would bloat the hot path)
//   - Deferred: RegInspect (frame already readable via regInspectSnapshot),
//               SvcHook (signal context), InlineHooker (pure asm relay).
//               Same discipline as §8.7.
//
// AAPCS64:
//   GP args come in x0..x7. FP args (double, float32, etc.) come in the
//   low 64 bits of q0..q7 (aliased d0..d7). We capture the low 64 bits
//   only — long double / SIMD types are out of scope for this feature.
//
// Ring buffer:
//   - Fixed 16 captures per site.
//   - head is the next-write position (uint32, wraps mod RING_SIZE).
//   - count is total captures since enable (may exceed RING_SIZE).
//   - Ring[head % RING_SIZE] is the next slot to overwrite.
//   - After count >= RING_SIZE, the oldest retained capture is at
//     ring[head % RING_SIZE] and the newest is at
//     ring[(head - 1 + RING_SIZE) % RING_SIZE].
//
// Concurrency:
//   - head/count/enabled are accessed via __atomic_*_n (POD discipline,
//     same as HookProfile).
//   - Ring entries are written by the capturing dispatch thread and read
//     under the subsystem mutex. Torn reads possible under contention,
//     same as HookProfile.
//
// Memory:
//   - sizeof(HookArgLog) ≈ 2208 bytes per slot (16 captures × 136 bytes
//     + 32-byte header). mem_bytes in HookArgLog is this value; separate
//     from HookProfile::mem_bytes which reports the stub/tramp/relay
//     footprint.

#define HOOK_ARGLOG_RING_SIZE 16

struct HookArgCapture {
    uint64_t gp[8];   // x0..x7 at target entry
    uint64_t fp[8];   // d0..d7 = low 64 bits of q0..q7; zero for GP-only sites
    uint64_t pc;      // target function entry (for sanity checks)
};

struct HookArgLog {
    bool        enabled;      // atomic — opt-in toggle
    uint32_t    head;         // atomic — next write position, wraps mod RING_SIZE
    uint32_t    count;        // atomic — total captures since enable
    uint32_t    mem_bytes;    // static — sizeof(HookArgLog)
    int         captures_fp;  // 0 = GP-only (RetMod), 1 = GP+FP (ChainHook)
    void*       target;       // function entry (populates HookArgCapture::pc)
    HookArgCapture ring[HOOK_ARGLOG_RING_SIZE];
};

// Initialize a freshly-created HookArgLog. Zeroes the ring, sets enabled=false,
// count=0, head=0, mem_bytes=sizeof(HookArgLog), target, captures_fp.
extern "C" void HookArgLogInit(HookArgLog* log, void* target, int captures_fp);

// Reset counters (count=0, head=0). Preserves enabled, mem_bytes, target,
// captures_fp. Does NOT zero the ring (stale data remains readable but
// overridden on next capture).
extern "C" void HookArgLogClear(HookArgLog* log);

// Capture helpers — called from dispatch helpers when enabled.
// CaptureFull reads x0..x7 from frame[0..7] and d0..d7 from frame[22,24,...,36].
// CaptureGp reads only x0..x7 from x0_x7[0..7], sets fp[] to zero.
extern "C" void HookArgLogCaptureFull(HookArgLog* log, const uint64_t* frame_saved);
extern "C" void HookArgLogCaptureGp(HookArgLog* log, const uint64_t* x0_x7);

// Global dispatchers — try ChainHook then ReturnMod. Same pattern as
// HookProfile's global dispatchers.
extern "C" bool HookArgLogSnapshot(void* target, HookArgLog* out);  // ring NOT copied
extern "C" bool HookArgLogEnable(void* target);
extern "C" bool HookArgLogDisable(void* target);
extern "C" bool HookArgLogReset(void* target);
extern "C" bool HookArgLogReadNth(void* target, uint32_t ringIdx, HookArgCapture* out);