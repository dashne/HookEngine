#include "ChainHook.h"
#include "HookProfile.h"
#include "HookArgLog.h"
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <mutex>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);

#define MAX_CHAINS              16
#define MAX_HANDLERS_PER_CHAIN  8
#define STUB_SIZE               32
#define STUB_PAGE_STUBS         32

struct ChainSlot {
    bool           active = false;
    void*          target = nullptr;
    void*          trampoline = nullptr;
    int            handler_count = 0;
    int            stub_slot = -1;
    ChainHandlerFn   handlers[MAX_HANDLERS_PER_CHAIN]   = {nullptr};
    ChainPredicateFn predicates[MAX_HANDLERS_PER_CHAIN] = {nullptr};
    HookProfile    profile;   // S66 — per-site profiling
    HookArgLog     arglog;    // S69 — per-site argument capture
};

static ChainSlot       g_chains[MAX_CHAINS];
static std::mutex      g_chain_mutex;
static uint8_t*        g_stub_page = nullptr;
static std::once_flag  g_stub_init_flag;
static bool            g_stub_init_ok = false;

extern "C" __attribute__((visibility("hidden"))) void chain_dispatch_helper(uint64_t* saved_frame);

extern "C" __attribute__((naked)) void chain_dispatch_naked() {
    __asm__ volatile(
            "sub sp, sp, #320\n"
            "stp x0,  x1,  [sp, #0]\n"
            "stp x2,  x3,  [sp, #16]\n"
            "stp x4,  x5,  [sp, #32]\n"
            "stp x6,  x7,  [sp, #48]\n"
            "stp x8,  x9,  [sp, #64]\n"
            "stp x19, x20, [sp, #80]\n"
            "stp x21, x22, [sp, #96]\n"
            "stp x23, x24, [sp, #112]\n"
            "stp x25, x26, [sp, #128]\n"
            "stp x27, x28, [sp, #144]\n"
            "stp x29, x30, [sp, #160]\n"
            "stp q0,  q1,  [sp, #176]\n"
            "stp q2,  q3,  [sp, #208]\n"
            "stp q4,  q5,  [sp, #240]\n"
            "stp q6,  q7,  [sp, #272]\n"
            "mov x0, sp\n"
            "adrp x16, chain_dispatch_helper\n"
            "add  x16, x16, :lo12:chain_dispatch_helper\n"
            "blr  x16\n"
            "ldr x16, [sp, #304]\n"
            "ldp x0,  x1,  [sp, #0]\n"
            "ldp x2,  x3,  [sp, #16]\n"
            "ldp x4,  x5,  [sp, #32]\n"
            "ldp x6,  x7,  [sp, #48]\n"
            "ldp x8,  x9,  [sp, #64]\n"
            "ldp x19, x20, [sp, #80]\n"
            "ldp x21, x22, [sp, #96]\n"
            "ldp x23, x24, [sp, #112]\n"
            "ldp x25, x26, [sp, #128]\n"
            "ldp x27, x28, [sp, #144]\n"
            "ldp x29, x30, [sp, #160]\n"
            "ldp q0,  q1,  [sp, #176]\n"
            "ldp q2,  q3,  [sp, #208]\n"
            "ldp q4,  q5,  [sp, #240]\n"
            "ldp q6,  q7,  [sp], #320\n"
            "br  x16\n"
            );
}

extern "C" __attribute__((visibility("hidden"))) void chain_dispatch_helper(uint64_t* saved) {
    uint64_t slot_id = saved[9];
    uint64_t* xregs  = &saved[0];
    uint64_t* tramp_slot = &saved[38];

    ChainHandlerFn   local_handlers[MAX_HANDLERS_PER_CHAIN];
    ChainPredicateFn local_preds[MAX_HANDLERS_PER_CHAIN];
    HookProfile*     local_profile = nullptr;
    HookArgLog*      local_arglog  = nullptr;
    int n = 0;
    void* tramp = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_chain_mutex);
        for (auto& s : g_chains) {
            if (s.active && s.stub_slot == (int)slot_id) {
                n = s.handler_count;
                tramp = s.trampoline;
                local_profile = &s.profile;
                local_arglog  = &s.arglog;
                for (int i = 0; i < n; i++) {
                    local_handlers[i] = s.handlers[i];
                    local_preds[i]    = s.predicates[i];
                }
                break;
            }
        }
    }

    // S69 — capture args once per dispatch, before any handler mutates
    // the observable frame. Capture is a cheap POD write to the ring;
    // disabled path is a single relaxed atomic load.
    if (local_arglog && __atomic_load_n(&local_arglog->enabled, __ATOMIC_RELAXED)) {
        HookArgLogCaptureFull(local_arglog, saved);
    }

    bool profile_enabled = local_profile
                           && __atomic_load_n(&local_profile->enabled, __ATOMIC_RELAXED);

    for (int i = 0; i < n; i++) {
        if (!local_handlers[i]) continue;
        if (local_preds[i] && !local_preds[i](xregs)) continue;
        if (profile_enabled) {
            uint64_t t0 = HookProfileReadNs();
            local_handlers[i](xregs);
            uint64_t t1 = HookProfileReadNs();
            HookProfileAccumulate(local_profile, t1 - t0);
        } else {
            local_handlers[i](xregs);
        }
    }
    *tramp_slot = (uint64_t)tramp;
}

static bool EnsureStubPage() {
    std::call_once(g_stub_init_flag, []() {
        long ps = sysconf(_SC_PAGESIZE);
        void* p = mmap(nullptr, ps, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) return;
        g_stub_page = (uint8_t*)p;
        for (int i = 0; i < STUB_PAGE_STUBS; i++) {
            uint32_t* u32 = (uint32_t*)(g_stub_page + i * STUB_SIZE);
            u32[0] = 0x58000090u;
            u32[1] = 0x580000A9u;
            u32[2] = 0xD61F0200u;
            u32[3] = 0xD503201Fu;
            *(uint64_t*)(u32 + 4) = (uint64_t)&chain_dispatch_naked;
            *(uint64_t*)(u32 + 6) = (uint64_t)i;
        }
        __builtin___clear_cache((char*)g_stub_page, (char*)g_stub_page + ps);
        if (mprotect(g_stub_page, ps, PROT_READ | PROT_EXEC) != 0) {
            munmap(g_stub_page, ps);
            g_stub_page = nullptr;
            return;
        }
        g_stub_init_ok = true;
    });
    return g_stub_init_ok;
}

static bool ChainReadPageProt(uintptr_t page, int* out) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3) {
            if (s <= page && page < e) {
                int p = 0;
                if (perms[0] == 'r') p |= PROT_READ;
                if (perms[1] == 'w') p |= PROT_WRITE;
                if (perms[2] == 'x') p |= PROT_EXEC;
                *out = p; ok = true; break;
            }
        }
    }
    fclose(f);
    return ok;
}

static bool ChainValidateTarget(void* target) {
    if (!target) return false;
    if (((uintptr_t)target & 3u) != 0) return false;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)target & ~((uintptr_t)ps - 1);
    int prot = 0;
    if (!ChainReadPageProt(page, &prot)) return false;
    return (prot & PROT_EXEC) != 0;
}

extern "C" HookResult ChainAttach(void* target, ChainHandlerFn handler, void** outTrampoline) {
    return ChainAttachCond(target, nullptr, handler, outTrampoline);
}

extern "C" HookResult ChainAttachCond(void* target, ChainPredicateFn predicate,
                                      ChainHandlerFn handler, void** outTrampoline) {
    if (!target || !handler) return HOOK_INTERNAL_ERROR;
    if (!ChainValidateTarget(target)) return HOOK_TARGET_INVALID;

    std::lock_guard<std::mutex> lk(g_chain_mutex);

    for (auto& s : g_chains) {
        if (s.active && s.target == target) {
            if (s.handler_count >= MAX_HANDLERS_PER_CHAIN) return HOOK_INTERNAL_ERROR;
            s.handlers[s.handler_count]   = handler;
            s.predicates[s.handler_count] = predicate;
            s.handler_count++;
            if (outTrampoline) *outTrampoline = s.trampoline;
            LOGD("ChainAttachCond: %p += handler (count=%d pred=%p)",
                 target, s.handler_count, (void*)predicate);
            return HOOK_OK;
        }
    }

    ChainSlot* slot = nullptr;
    int idx = -1;
    for (int i = 0; i < MAX_CHAINS; i++) {
        if (!g_chains[i].active) { slot = &g_chains[i]; idx = i; break; }
    }
    if (!slot) return HOOK_ALLOC_FAILED;
    if (!EnsureStubPage()) return HOOK_ALLOC_FAILED;

    void* stub_addr = g_stub_page + idx * STUB_SIZE;
    void* tramp = nullptr;
    HookResult r = InstallInlineHookEx(target, stub_addr, &tramp, "chain");
    if (r != HOOK_OK) {
        LOGE("ChainAttachCond: InstallInlineHookEx failed r=%d", (int)r);
        return r;
    }

    slot->active = true;
    slot->target = target;
    slot->trampoline = tramp;
    slot->handler_count = 1;
    slot->handlers[0]   = handler;
    slot->predicates[0] = predicate;
    slot->stub_slot = idx;

    // S66 — profile footprint: stub slot + tramp page + relay page.
    long ps = sysconf(_SC_PAGESIZE);
    HookProfileInit(&slot->profile, (uint32_t)(STUB_SIZE + 2 * ps));

    // S69 — arglog init. captures_fp=1 because chain_dispatch_naked saves q0..q7.
    HookArgLogInit(&slot->arglog, target, 1);

    if (outTrampoline) *outTrampoline = tramp;
    LOGD("ChainAttachCond: %p slot=%d tramp=%p (new chain pred=%p)",
         target, idx, tramp, (void*)predicate);
    return HOOK_OK;
}

extern "C" HookResult ChainDetach(void* target, ChainHandlerFn handler) {
    if (!target || !handler) return HOOK_INTERNAL_ERROR;
    std::lock_guard<std::mutex> lk(g_chain_mutex);

    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;

        int found = -1;
        for (int i = 0; i < s.handler_count; i++) {
            if (s.handlers[i] == handler) { found = i; break; }
        }
        if (found < 0) return HOOK_NOT_HOOKED;

        for (int i = found; i < s.handler_count - 1; i++) {
            s.handlers[i]   = s.handlers[i + 1];
            s.predicates[i] = s.predicates[i + 1];
        }
        s.handler_count--;
        s.handlers[s.handler_count]   = nullptr;
        s.predicates[s.handler_count] = nullptr;

        if (s.handler_count == 0) {
            HookResult r = UninstallInlineHook(target);
            if (r != HOOK_OK && r != HOOK_NOT_HOOKED) return r;
            s.active = false;
            s.target = nullptr;
            s.trampoline = nullptr;
            s.stub_slot = -1;
            LOGD("ChainDetach: %p — chain fully removed", target);
        } else {
            LOGD("ChainDetach: %p — %d handlers remain", target, s.handler_count);
        }
        return HOOK_OK;
    }
    return HOOK_NOT_HOOKED;
}

extern "C" int ChainCount() {
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    int n = 0;
    for (auto& s : g_chains) if (s.active) n++;
    return n;
}

extern "C" int ChainHandlerCount(void* target) {
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) if (s.active && s.target == target) return s.handler_count;
    return 0;
}

extern "C" void ChainResetAll() {
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        s.active = false;
        s.target = nullptr;
        s.trampoline = nullptr;
        s.handler_count = 0;
        s.stub_slot = -1;
        for (int i = 0; i < MAX_HANDLERS_PER_CHAIN; i++) {
            s.handlers[i]   = nullptr;
            s.predicates[i] = nullptr;
        }
    }
}

// ─── S66 — ChainHook profile dispatch ────────────────────────────────
extern "C" bool ChainHook_ProfileLookup(void* target, HookProfile* out) {
    if (!target || !out) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        out->enabled    = __atomic_load_n(&s.profile.enabled,    __ATOMIC_RELAXED);
        out->call_count = __atomic_load_n(&s.profile.call_count, __ATOMIC_RELAXED);
        out->total_ns   = __atomic_load_n(&s.profile.total_ns,   __ATOMIC_RELAXED);
        out->min_ns     = __atomic_load_n(&s.profile.min_ns,     __ATOMIC_RELAXED);
        out->max_ns     = __atomic_load_n(&s.profile.max_ns,     __ATOMIC_RELAXED);
        out->mem_bytes  = s.profile.mem_bytes;
        return true;
    }
    return false;
}

extern "C" bool ChainHook_ProfileEnable(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        __atomic_store_n(&s.profile.enabled, true, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

extern "C" bool ChainHook_ProfileDisable(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        __atomic_store_n(&s.profile.enabled, false, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

extern "C" bool ChainHook_ProfileReset(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        HookProfileClear(&s.profile);
        return true;
    }
    return false;
}

// ─── S69 — ChainHook arg-log dispatch ────────────────────────────────
extern "C" bool ChainHook_ArgLogSnapshot(void* target, HookArgLog* out) {
    if (!target || !out) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        out->enabled     = __atomic_load_n(&s.arglog.enabled, __ATOMIC_RELAXED);
        out->head        = __atomic_load_n(&s.arglog.head,    __ATOMIC_RELAXED);
        out->count       = __atomic_load_n(&s.arglog.count,   __ATOMIC_RELAXED);
        out->mem_bytes   = s.arglog.mem_bytes;
        out->captures_fp = s.arglog.captures_fp;
        out->target      = s.arglog.target;
        return true;
    }
    return false;
}

extern "C" bool ChainHook_ArgLogEnable(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        __atomic_store_n(&s.arglog.enabled, true, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

extern "C" bool ChainHook_ArgLogDisable(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        __atomic_store_n(&s.arglog.enabled, false, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

extern "C" bool ChainHook_ArgLogReset(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        HookArgLogClear(&s.arglog);
        return true;
    }
    return false;
}

extern "C" bool ChainHook_ArgLogReadNth(void* target, uint32_t ringIdx, HookArgCapture* out) {
    if (!target || !out || ringIdx >= HOOK_ARGLOG_RING_SIZE) return false;
    std::lock_guard<std::mutex> lk(g_chain_mutex);
    for (auto& s : g_chains) {
        if (!s.active || s.target != target) continue;
        *out = s.arglog.ring[ringIdx];
        return true;
    }
    return false;
}