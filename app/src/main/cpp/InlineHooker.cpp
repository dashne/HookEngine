#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>
#include <errno.h>
#include <mutex>
#include "arm64_reloc.h"
#include "HookRegistry.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Session 15 — per-install quiet flag. When non-zero, the verbose
// IIHE(...) debug lines in InstallInlineHookEx are suppressed. Used by
// test_regression.cpp::RunRegressionBug4 to avoid ~15,000 lines of noise
// across 1000 install/uninstall cycles. Errors (LOGE) always print.
extern "C" int g_iihe_quiet = 0;
#define IIHE_LOG(...) do { if (!g_iihe_quiet) LOGD(__VA_ARGS__); } while (0)

#define ARM64_LDR_X17_8 0x58000051
#define ARM64_BR_X17    0xD61F0220

static std::mutex g_hook_mutex;
// Deferred-free grace period. Before munmap'ing the relay and trampoline
// pages during uninstall, sleep this many milliseconds so any caller that
// branched into the relay just before the target was patched back has time
// to finish executing (relay executes in ~2ns; 20ms is 10,000,000× margin).
// Tests override this to a smaller value to keep the stress test fast.
static long g_uninstall_grace_ms = 100;
extern "C" int g_relay_alloc_count = 0;
extern "C" int g_relay_free_count  = 0;

extern "C" void SetUninstallGraceMs(long ms) { g_uninstall_grace_ms = ms; }
extern "C" long GetUninstallGraceMs()        { return g_uninstall_grace_ms; }

static bool ReadPageProt(uintptr_t page, int *outProt) {
    FILE *f = fopen("/proc/self/maps", "r");
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
                *outProt = p;
                ok = true;
                break;
            }
        }
    }
    fclose(f);
    return ok;
}

void* AllocateMemoryNear(void* target, size_t size) {
    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t targetAddr = (uintptr_t)target;
    const uintptr_t MAX_DIST = 120UL * 1024 * 1024;

    auto tryOffset = [&](uintptr_t off) -> void* {
        uintptr_t cand[2];
        int n = 0;
        if (targetAddr >= off)       cand[n++] = (targetAddr - off) & ~(pageSize - 1);
        if (targetAddr + off > off)  cand[n++] = (targetAddr + off) & ~(pageSize - 1);
        for (int k = 0; k < n; k++) {
            void* hint = (void*)cand[k];
            void* m = mmap(hint, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (m == MAP_FAILED) continue;
            if (m != hint) { munmap(m, size); continue; }
            uintptr_t got = (uintptr_t)m;
            if (got > targetAddr + MAX_DIST || targetAddr > got + MAX_DIST) {
                munmap(m, size); continue;
            }
            return m;
        }
        return nullptr;
    };
    for (int i = 1; i <= 1024; i++) {
        void* m = tryOffset((uintptr_t)i * pageSize); if (m) return m;
    }
    for (int i = 1; i < 120; i++) {
        void* m = tryOffset((uintptr_t)i * 1024 * 1024); if (m) return m;
    }
    return nullptr;
}

static HookResult BuildTrampoline(void* target, void* tramp, size_t tramp_size) {
    uint32_t orig = *(volatile uint32_t*)target;
    uint32_t out[16] = {0};
    int n = EmitRelocated(orig, (uint64_t)target, (uint64_t)tramp, out, 16);
    if (n == 0) {
        LOGE("InlineHooker: cannot relocate insn 0x%08x (kind=%d) at %p",
             orig, (int)ClassifyInsn(orig).kind, target);
        return HOOK_UNSUPPORTED_INSN;
    }
    if ((size_t)(n + 1) * 4 > tramp_size) return HOOK_ALLOC_FAILED;

    uint32_t ret;
    if (!EncodeBranch(&ret, (uint64_t)tramp + n * 4, (uint64_t)target + 4, false)) {
        LOGE("InlineHooker: return branch out of range");
        return HOOK_ALLOC_FAILED;
    }
    out[n] = ret;

    memcpy(tramp, out, (n + 1) * 4);
    __builtin___clear_cache((char*)tramp, (char*)tramp + (n + 1) * 4);
    return HOOK_OK;
}

extern "C" HookResult InstallInlineHookEx(void* targetFunc, void* fakeFunc,
                                          void** originalTrampoline,
                                          const char* name) {
    if (!targetFunc || !fakeFunc) return HOOK_INTERNAL_ERROR;
    IIHE_LOG("IIHE(%s): enter target=%p", name ? name : "?", targetFunc);

    std::lock_guard<std::mutex> lock(g_hook_mutex);
    IIHE_LOG("IIHE(%s): mutex acquired", name ? name : "?");
    HookHandle* existing = FindHandle(targetFunc);
    if (existing) {
        if (existing->replacement != fakeFunc) {
            LOGE("IIHE(%s): conflict — target %p already hooked with different replacement",
                 name ? name : "?", targetFunc);
            return HOOK_INTERNAL_ERROR;
        }
        return HOOK_ALREADY_HOOKED;
    }
    IIHE_LOG("IIHE(%s): not already hooked", name ? name : "?");

    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return HOOK_INTERNAL_ERROR;
    uintptr_t pageStart = (uintptr_t)targetFunc & ~(pageSize - 1);

    int origProt = PROT_READ | PROT_EXEC;
    if (!ReadPageProt(pageStart, &origProt)) return HOOK_INTERNAL_ERROR;
    IIHE_LOG("IIHE(%s): origProt=0x%x", name ? name : "?", origProt);

    void* relay = AllocateMemoryNear(targetFunc, pageSize);
    if (!relay) {
        LOGE("IIHE(%s): no relay page", name ? name : "?");
        return HOOK_ALLOC_FAILED;
    }
    IIHE_LOG("IIHE(%s): relay=%p", name ? name : "?", relay);
    uint32_t* rp = (uint32_t*)relay;
    rp[0] = ARM64_LDR_X17_8;
    rp[1] = ARM64_BR_X17;
    *(uint64_t*)&rp[2] = (uint64_t)fakeFunc;
    __builtin___clear_cache((char*)relay, (char*)relay + 16);

    void* tramp = AllocateMemoryNear(targetFunc, pageSize);
    if (!tramp) {
        LOGE("IIHE(%s): no tramp page", name ? name : "?");
        munmap(relay, pageSize);
        return HOOK_ALLOC_FAILED;
    }
    IIHE_LOG("IIHE(%s): tramp=%p", name ? name : "?", tramp);

    IIHE_LOG("IIHE(%s): calling BuildTrampoline", name ? name : "?");
    HookResult r = BuildTrampoline(targetFunc, tramp, pageSize);
    IIHE_LOG("IIHE(%s): BuildTrampoline r=%d", name ? name : "?", (int)r);
    if (r != HOOK_OK) {
        munmap(tramp, pageSize);
        munmap(relay, pageSize);
        return r;
    }
    if (mprotect(tramp, pageSize, PROT_READ | PROT_EXEC) != 0) {
        LOGE("IIHE(%s): tramp RX failed errno=%d", name ? name : "?", errno);
        munmap(tramp, pageSize);
        munmap(relay, pageSize);
        return HOOK_MPROTECT_FAILED;
    }
    IIHE_LOG("IIHE(%s): tramp RX OK", name ? name : "?");

    uint32_t branch;
    if (!EncodeBranch(&branch, (uint64_t)targetFunc, (uint64_t)relay, false)) {
        LOGE("IIHE(%s): branch encode failed", name ? name : "?");
        munmap(tramp, pageSize);
        munmap(relay, pageSize);
        return HOOK_ALLOC_FAILED;
    }
    IIHE_LOG("IIHE(%s): target branch=0x%08x", name ? name : "?", branch);

    if (mprotect((void*)pageStart, pageSize, origProt | PROT_WRITE) != 0) {
        LOGE("IIHE(%s): target RW failed errno=%d", name ? name : "?", errno);
        munmap(tramp, pageSize);
        munmap(relay, pageSize);
        return HOOK_MPROTECT_FAILED;
    }
    IIHE_LOG("IIHE(%s): target RW OK", name ? name : "?");

    uint32_t saved = *(volatile uint32_t*)targetFunc;
    *(volatile uint32_t*)targetFunc = branch;
    __builtin___clear_cache((char*)targetFunc, (char*)targetFunc + 4);

    if (*(volatile uint32_t*)targetFunc != branch) {
        LOGE("IIHE(%s): patch verify failed", name ? name : "?");
        *(volatile uint32_t*)targetFunc = saved;
        __builtin___clear_cache((char*)targetFunc, (char*)targetFunc + 4);
        mprotect((void*)pageStart, pageSize, origProt);
        munmap(tramp, pageSize);
        munmap(relay, pageSize);
        return HOOK_INTERNAL_ERROR;
    }
    IIHE_LOG("IIHE(%s): patch verified", name ? name : "?");

    if (mprotect((void*)pageStart, pageSize, origProt) != 0) {
        LOGE("IIHE(%s): target restore failed errno=%d", name ? name : "?", errno);
        mprotect((void*)pageStart, pageSize, origProt | PROT_WRITE);
        *(volatile uint32_t*)targetFunc = saved;
        __builtin___clear_cache((char*)targetFunc, (char*)targetFunc + 4);
        mprotect((void*)pageStart, pageSize, origProt);
        munmap(tramp, pageSize);
        munmap(relay, pageSize);
        return HOOK_MPROTECT_FAILED;
    }
    IIHE_LOG("IIHE(%s): target restored to 0x%x", name ? name : "?", origProt);

    IIHE_LOG("IIHE(%s): RegistryAlloc", name ? name : "?");
    HookHandle* h = RegistryAlloc();
    if (!h) {
        LOGE("IIHE(%s): no registry slot", name ? name : "?");
        mprotect((void*)pageStart, pageSize, origProt | PROT_WRITE);
        *(volatile uint32_t*)targetFunc = saved;
        __builtin___clear_cache((char*)targetFunc, (char*)targetFunc + 4);
        mprotect((void*)pageStart, pageSize, origProt);
        munmap(tramp, pageSize);
        munmap(relay, pageSize);
        return HOOK_INTERNAL_ERROR;
    }
    h->type = HT_INLINE;
    h->target = targetFunc;
    h->replacement = fakeFunc;
    h->saved = (void*)(uintptr_t)saved;
    h->trampoline = tramp;
    h->tramp_size = pageSize;
    h->relay      = relay;
    h->relay_size = pageSize;
    h->orig_prot  = origProt;
    h->page_size = pageSize;
    h->state = HS_INSTALLED;
    if (name) snprintf(h->name, sizeof(h->name), "%s", name);

    if (originalTrampoline) *originalTrampoline = tramp;

    g_relay_alloc_count++;
    IIHE_LOG("IIHE(%s): DONE relay=%p tramp=%p", name ? name : "?", relay, tramp);
    return HOOK_OK;
}

extern "C" bool InstallInlineHook(void* targetFunc, void* fakeFunc, void** originalTrampoline) {
    return InstallInlineHookEx(targetFunc, fakeFunc, originalTrampoline, "inline") == HOOK_OK;
}

extern "C" HookResult UninstallInlineHook(void* targetFunc) {
    void* relay = nullptr;
    size_t relay_size = 0;
    void* tramp = nullptr;
    size_t tramp_size = 0;

    {
        std::lock_guard<std::mutex> lock(g_hook_mutex);
        HookHandle* h = FindHandle(targetFunc);
        if (!h || h->type != HT_INLINE) return HOOK_NOT_HOOKED;
        h->state = HS_REMOVING;

        uintptr_t pageStart = (uintptr_t)targetFunc & ~(h->page_size - 1);
        if (mprotect((void*)pageStart, h->page_size, h->orig_prot | PROT_WRITE) != 0) {
            h->state = HS_INSTALLED;
            return HOOK_MPROTECT_FAILED;
        }
        *(volatile uint32_t*)targetFunc = (uint32_t)(uintptr_t)h->saved;
        __builtin___clear_cache((char*)targetFunc, (char*)targetFunc + 4);
        if (mprotect((void*)pageStart, h->page_size, h->orig_prot) != 0) {
            h->state = HS_INSTALLED;
            return HOOK_MPROTECT_FAILED;
        }

        // Steal the pages so we can free them after the grace period.
        relay = h->relay; relay_size = h->relay_size;
        tramp = h->trampoline; tramp_size = h->tramp_size;
        RegistryFree(h);
    }
    // Mutex released. Concurrent uninstalls no longer serialize on the sleep.

    // ─── Deferred free: caller may still be in relay / trampoline ───
    if (g_uninstall_grace_ms > 0) {
        struct timespec ts;
        ts.tv_sec  = g_uninstall_grace_ms / 1000;
        ts.tv_nsec = (g_uninstall_grace_ms % 1000) * 1000000L;
        nanosleep(&ts, nullptr);
    }
    if (relay) munmap(relay, relay_size);
    if (tramp) munmap(tramp, tramp_size);
    g_relay_free_count++;
    return HOOK_OK;
}