#include "ReturnMod.h"
#include "arm64_reloc.h"
#include "HookProfile.h"
#include "HookArgLog.h"
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <mutex>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);

void* AllocateMemoryNear(void* target, size_t size);

#define MAX_RETMODS 16

struct RetModSlot {
    bool             active       = false;
    void*            target       = nullptr;
    void*            shadow_tramp = nullptr;
    ReturnModifierFn modifier     = nullptr;
    int              stub_slot    = -1;
    HookProfile      profile;   // S67 — per-site profiling
    HookArgLog       arglog;    // S69 — per-site GP argument capture
};

static RetModSlot  g_retmods[MAX_RETMODS];
static std::mutex  g_retmod_mutex;

extern "C" __attribute__((visibility("hidden")))
void* g_retmod_tramps[MAX_RETMODS] = {nullptr};

// S69 — capture is GP-only for RetMod: retmod_stub_* only saves x0..x7
// (80-byte frame). FP args would require growing the stub frame by 128B
// on the hot path, which is not worth it for an opt-in feature. ChainHook
// captures FP because chain_dispatch_naked already saves q0..q7.
extern "C" __attribute__((visibility("hidden")))
uint64_t retmod_dispatch_helper(uint64_t* saved_args, uint64_t orig_ret, int slot) {
    ReturnModifierFn fn = nullptr;
    HookProfile*     prof = nullptr;
    HookArgLog*      arglog = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_retmod_mutex);
        if (slot >= 0 && slot < MAX_RETMODS && g_retmods[slot].active) {
            fn     = g_retmods[slot].modifier;
            prof   = &g_retmods[slot].profile;
            arglog = &g_retmods[slot].arglog;
        }
    }
    if (!fn) return orig_ret;

    if (arglog && __atomic_load_n(&arglog->enabled, __ATOMIC_RELAXED)) {
        HookArgLogCaptureGp(arglog, saved_args);
    }

    bool enabled = prof && __atomic_load_n(&prof->enabled, __ATOMIC_RELAXED);
    if (enabled) {
        uint64_t t0 = HookProfileReadNs();
        uint64_t rv = fn(saved_args, orig_ret);
        uint64_t t1 = HookProfileReadNs();
        HookProfileAccumulate(prof, t1 - t0);
        return rv;
    }
    return fn(saved_args, orig_ret);
}

#define DEFINE_RETMOD_STUB(N) \
extern "C" __attribute__((naked, used)) void retmod_stub_##N() { \
    __asm__ volatile( \
        "sub sp, sp, #80\n" \
        "stp x0, x1, [sp, #0]\n" \
        "stp x2, x3, [sp, #16]\n" \
        "stp x4, x5, [sp, #32]\n" \
        "stp x6, x7, [sp, #48]\n" \
        "str x30, [sp, #64]\n" \
        "str x9,  [sp, #72]\n" \
        "mov x9, #" #N "\n" \
        "adrp x16, g_retmod_tramps\n" \
        "add  x16, x16, :lo12:g_retmod_tramps\n" \
        "ldr  x16, [x16, x9, lsl #3]\n" \
        "blr  x16\n" \
        "mov x1, x0\n" \
        "mov x0, sp\n" \
        "mov x2, #" #N "\n" \
        "adrp x16, retmod_dispatch_helper\n" \
        "add  x16, x16, :lo12:retmod_dispatch_helper\n" \
        "blr  x16\n" \
        "ldr x30, [sp, #64]\n" \
        "ldr x9,  [sp, #72]\n" \
        "add sp, sp, #80\n" \
        "ret\n" \
    ); \
}

DEFINE_RETMOD_STUB(0)
DEFINE_RETMOD_STUB(1)
DEFINE_RETMOD_STUB(2)
DEFINE_RETMOD_STUB(3)
DEFINE_RETMOD_STUB(4)
DEFINE_RETMOD_STUB(5)
DEFINE_RETMOD_STUB(6)
DEFINE_RETMOD_STUB(7)
DEFINE_RETMOD_STUB(8)
DEFINE_RETMOD_STUB(9)
DEFINE_RETMOD_STUB(10)
DEFINE_RETMOD_STUB(11)
DEFINE_RETMOD_STUB(12)
DEFINE_RETMOD_STUB(13)
DEFINE_RETMOD_STUB(14)
DEFINE_RETMOD_STUB(15)

static void* retmodStubFor(int slot) {
    static void* fns[] = {
            (void*)&retmod_stub_0,  (void*)&retmod_stub_1,  (void*)&retmod_stub_2,  (void*)&retmod_stub_3,
            (void*)&retmod_stub_4,  (void*)&retmod_stub_5,  (void*)&retmod_stub_6,  (void*)&retmod_stub_7,
            (void*)&retmod_stub_8,  (void*)&retmod_stub_9,  (void*)&retmod_stub_10, (void*)&retmod_stub_11,
            (void*)&retmod_stub_12, (void*)&retmod_stub_13, (void*)&retmod_stub_14, (void*)&retmod_stub_15,
    };
    if (slot < 0 || slot >= (int)(sizeof(fns) / sizeof(fns[0]))) return nullptr;
    return fns[slot];
}

static void* BuildRetModShadow(void* target) {
    long ps = sysconf(_SC_PAGESIZE);
    void* tramp = AllocateMemoryNear(target, ps);
    if (!tramp) return nullptr;

    uint32_t orig = *(volatile uint32_t*)target;
    uint32_t out[16] = {0};
    int n = EmitRelocated(orig, (uint64_t)target, (uint64_t)tramp, out, 16);
    if (n == 0) {
        LOGE("BuildRetModShadow: cannot relocate insn 0x%08x at %p", orig, target);
        munmap(tramp, ps);
        return nullptr;
    }
    uint32_t ret;
    if (!EncodeBranch(&ret, (uint64_t)tramp + n * 4, (uint64_t)target + 4, false)) {
        LOGE("BuildRetModShadow: return branch out of range");
        munmap(tramp, ps);
        return nullptr;
    }
    out[n] = ret;
    memcpy(tramp, out, (n + 1) * 4);
    __builtin___clear_cache((char*)tramp, (char*)tramp + (n + 1) * 4);
    if (mprotect(tramp, ps, PROT_READ | PROT_EXEC) != 0) {
        LOGE("BuildRetModShadow: mprotect RX failed errno=%d", errno);
        munmap(tramp, ps);
        return nullptr;
    }
    return tramp;
}

static bool RetModReadProt(uintptr_t page, int* out) {
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

extern "C" HookResult AttachRetMod(void* target, ReturnModifierFn modifier, void** outTrampoline) {
    if (!target || !modifier) return HOOK_INTERNAL_ERROR;
    if (((uintptr_t)target & 3u) != 0) return HOOK_TARGET_INVALID;

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)target & ~((uintptr_t)ps - 1);
    int prot = 0;
    if (!RetModReadProt(page, &prot)) return HOOK_TARGET_INVALID;
    if ((prot & PROT_EXEC) == 0) return HOOK_TARGET_INVALID;

    std::lock_guard<std::mutex> lk(g_retmod_mutex);

    for (auto& s : g_retmods) {
        if (s.active && s.target == target) return HOOK_ALREADY_HOOKED;
    }

    int slot = -1;
    for (int i = 0; i < MAX_RETMODS; i++) {
        if (!g_retmods[i].active) { slot = i; break; }
    }
    if (slot < 0) return HOOK_ALLOC_FAILED;

    void* stub = retmodStubFor(slot);
    if (!stub) return HOOK_INTERNAL_ERROR;

    void* shadow = BuildRetModShadow(target);
    if (!shadow) return HOOK_ALLOC_FAILED;

    g_retmod_tramps[slot]           = shadow;
    g_retmods[slot].modifier        = modifier;
    g_retmods[slot].active          = true;
    g_retmods[slot].target          = target;
    g_retmods[slot].shadow_tramp    = shadow;
    g_retmods[slot].stub_slot       = slot;
    HookProfileInit(&g_retmods[slot].profile, (uint32_t)(3 * ps));
    HookArgLogInit(&g_retmods[slot].arglog, target, 0);  // GP-only

    void* unused_tramp = nullptr;
    HookResult r = InstallInlineHookEx(target, stub, &unused_tramp, "retmod");
    if (r != HOOK_OK) {
        LOGE("AttachRetMod: InstallInlineHookEx failed r=%d", (int)r);
        g_retmod_tramps[slot] = nullptr;
        g_retmods[slot]       = RetModSlot{};
        munmap(shadow, (size_t)ps);
        return r;
    }

    if (outTrampoline) *outTrampoline = shadow;
    LOGD("AttachRetMod: %p slot=%d shadow=%p (new retmod)", target, slot, shadow);
    return HOOK_OK;
}

extern "C" HookResult DetachRetMod(void* target) {
    if (!target) return HOOK_INTERNAL_ERROR;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);

    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        int slot = s.stub_slot;
        void* shadow = s.shadow_tramp;

        HookResult r = UninstallInlineHook(target);
        if (r != HOOK_OK && r != HOOK_NOT_HOOKED) return r;

        g_retmod_tramps[slot] = nullptr;
        s = RetModSlot{};

        if (shadow) munmap(shadow, (size_t)sysconf(_SC_PAGESIZE));
        LOGD("DetachRetMod: %p slot=%d — removed", target, slot);
        return HOOK_OK;
    }
    return HOOK_NOT_HOOKED;
}

extern "C" int RetModCount() {
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    int n = 0;
    for (auto& s : g_retmods) if (s.active) n++;
    return n;
}

extern "C" void RetModResetAll() {
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (int i = 0; i < MAX_RETMODS; i++) {
        auto& s = g_retmods[i];
        if (!s.active) continue;
        UninstallInlineHook(s.target);
        if (s.shadow_tramp) munmap(s.shadow_tramp, (size_t)sysconf(_SC_PAGESIZE));
        g_retmod_tramps[i] = nullptr;
        s = RetModSlot{};
    }
}

// ─── S67 — ReturnMod profile dispatch ────────────────────────────────
extern "C" bool RetMod_ProfileLookup(void* target, HookProfile* out) {
    if (!target || !out) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
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

extern "C" bool RetMod_ProfileEnable(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        __atomic_store_n(&s.profile.enabled, true, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

extern "C" bool RetMod_ProfileDisable(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        __atomic_store_n(&s.profile.enabled, false, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

extern "C" bool RetMod_ProfileReset(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        HookProfileClear(&s.profile);
        return true;
    }
    return false;
}

// ─── S69 — ReturnMod arg-log dispatch ────────────────────────────────
extern "C" bool RetMod_ArgLogSnapshot(void* target, HookArgLog* out) {
    if (!target || !out) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
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

extern "C" bool RetMod_ArgLogEnable(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        __atomic_store_n(&s.arglog.enabled, true, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

extern "C" bool RetMod_ArgLogDisable(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        __atomic_store_n(&s.arglog.enabled, false, __ATOMIC_RELEASE);
        return true;
    }
    return false;
}

extern "C" bool RetMod_ArgLogReset(void* target) {
    if (!target) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        HookArgLogClear(&s.arglog);
        return true;
    }
    return false;
}

extern "C" bool RetMod_ArgLogReadNth(void* target, uint32_t ringIdx, HookArgCapture* out) {
    if (!target || !out || ringIdx >= HOOK_ARGLOG_RING_SIZE) return false;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        *out = s.arglog.ring[ringIdx];
        return true;
    }
    return false;
}