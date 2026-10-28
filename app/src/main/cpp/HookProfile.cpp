#include "HookProfile.h"
#include <time.h>
#include <cstring>

extern "C" uint64_t HookProfileReadNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

extern "C" void HookProfileInit(HookProfile* p, uint32_t mem_bytes) {
    if (!p) return;
    p->enabled    = false;
    p->call_count = 0;
    p->total_ns   = 0;
    p->min_ns     = UINT64_MAX;
    p->max_ns     = 0;
    p->mem_bytes  = mem_bytes;
}

extern "C" void HookProfileAccumulate(HookProfile* p, uint64_t elapsed_ns) {
    if (!p) return;
    __atomic_fetch_add(&p->call_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&p->total_ns,   elapsed_ns, __ATOMIC_RELAXED);
    __atomic_fetch_add(&p->max_ns,     0, __ATOMIC_RELAXED);   // ensure loaded

    // min: CAS loop
    uint64_t prev = __atomic_load_n(&p->min_ns, __ATOMIC_RELAXED);
    while (elapsed_ns < prev) {
        if (__atomic_compare_exchange_n(&p->min_ns, &prev, elapsed_ns,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) break;
    }

    // max: CAS loop
    prev = __atomic_load_n(&p->max_ns, __ATOMIC_RELAXED);
    while (elapsed_ns > prev) {
        if (__atomic_compare_exchange_n(&p->max_ns, &prev, elapsed_ns,
                                        false,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) break;
    }
}

extern "C" void HookProfileClear(HookProfile* p) {
    if (!p) return;
    __atomic_store_n(&p->call_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&p->total_ns,   0, __ATOMIC_RELAXED);
    __atomic_store_n(&p->min_ns,     UINT64_MAX, __ATOMIC_RELAXED);
    __atomic_store_n(&p->max_ns,     0, __ATOMIC_RELAXED);
    // enabled and mem_bytes preserved
}

// ─── Subsystem dispatch ──────────────────────────────────────────────
// Each subsystem exports ChainHook_ProfileLookup(target, out). We try
// them in order. Session 66: ChainHook only. S67 will add ReturnMod.

extern "C" bool ChainHook_ProfileLookup(void* target, HookProfile* out);
extern "C" bool ChainHook_ProfileEnable(void* target);
extern "C" bool ChainHook_ProfileDisable(void* target);
extern "C" bool ChainHook_ProfileReset(void* target);

extern "C" bool HookProfileLookup(void* target, HookProfile* out) {
    if (!target || !out) return false;
    if (ChainHook_ProfileLookup(target, out)) return true;
    return false;
}

extern "C" bool HookProfileEnable(void* target) {
    if (!target) return false;
    if (ChainHook_ProfileEnable(target)) return true;
    return false;
}

extern "C" bool HookProfileDisable(void* target) {
    if (!target) return false;
    if (ChainHook_ProfileDisable(target)) return true;
    return false;
}

extern "C" bool HookProfileReset(void* target) {
    if (!target) return false;
    if (ChainHook_ProfileReset(target)) return true;
    return false;
}