#pragma once
#include <cstdint>

// Feature 8.7 — Per-hook CPU / memory profiling.
//
// A HookProfile is attached to every instrumented hook site. When enabled,
// the site's C++ dispatch helper wraps each user-handler invocation with
// two clock_gettime(CLOCK_MONOTONIC) reads and accumulates the delta.
//
// Design decisions:
//
//   - CLOCK_MONOTONIC (VDSO-accelerated, ~25ns/read) rather than
//     CLOCK_MONOTONIC_RAW (syscall, ~500ns/read). Two reads per handler
//     invocation = ~50-60ns of baseline overhead when profiling is enabled.
//     Acceptable for an opt-in debug feature; zero overhead when disabled
//     (single atomic bool load).
//
//   - Fields are plain uint64_t (POD), accessed via __atomic_*_n intrinsics.
//     No std::atomic<> — the struct stays trivially copyable so it can be
//     returned by value from JNI and reset wholesale.
//
//   - min_ns initialized to UINT64_MAX ("no samples yet"). Reset sets it
//     back to UINT64_MAX. Consumers check `min_ns == UINT64_MAX` to detect
//     "no samples recorded" rather than relying on call_count alone.
//
//   - mem_bytes is set once at install time (sum of relay page + tramp page
//     + per-slot aux) and never changes. Not atomic.
//
//   - avg_ns is NOT stored — computed on-the-fly as total_ns / call_count.
//
// Session 66 scope: only ChainHook sites are instrumented. ReturnMod and
// RegInspect extend in S67 (same pattern, different dispatch helper).

struct HookProfile {
    bool     enabled;      // toggled via HookProfileEnable/Disable
    uint64_t call_count;   // number of user-handler invocations measured
    uint64_t total_ns;     // sum of all measured handler durations
    uint64_t min_ns;       // minimum single-handler duration (UINT64_MAX if none)
    uint64_t max_ns;       // maximum single-handler duration
    uint32_t mem_bytes;    // static: memory footprint of this hook site
};

// Initialize a freshly-created profile. Set mem_bytes to the site's
// footprint. All counters to zero, enabled=false, min_ns=UINT64_MAX.
extern "C" void HookProfileInit(HookProfile* p, uint32_t mem_bytes);

// Accumulate one observation into a profile. caller must have verified
// p->enabled before calling; this does not check enabled. Thread-safe
// (atomics on all mutating fields).
extern "C" void HookProfileAccumulate(HookProfile* p, uint64_t elapsed_ns);

// Reset all counters to zero (min_ns → UINT64_MAX). Preserves enabled
// flag and mem_bytes.
extern "C" void HookProfileClear(HookProfile* p);

// Read CLOCK_MONOTONIC as nanoseconds. ~25ns per call.
extern "C" uint64_t HookProfileReadNs();

// Global dispatchers — try each subsystem's profile lookup in turn.
// Return true if target was found in any subsystem's active slot array.
extern "C" bool HookProfileLookup(void* target, HookProfile* out);
extern "C" bool HookProfileEnable(void* target);
extern "C" bool HookProfileDisable(void* target);
extern "C" bool HookProfileReset(void* target);