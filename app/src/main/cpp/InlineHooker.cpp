#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>
#include <errno.h>
#include <mutex>
#include <sys/eventfd.h>
#include <poll.h>
#include <pthread.h>
#include <time.h>
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

// ─── AllocateMemoryNear — Session 65 rewrite ─────────────────────────
//
// Old algorithm tried 1024 page offsets + 119 MB offsets — a fixed set of
// SPECIFIC pages. When two targets are close together (e.g., two fixtures
// in the same .so, a few KB apart), the first install consumes the free
// pages near both targets; the second install's fixed offsets all land on
// occupied pages and it returns nullptr even though plenty of free pages
// exist further out. Session 65 replaced the fixed-offset list with a
// page-by-page outward scan bounded by ±120MB. Fast case still 1-10
// iterations; worst case ~30K (30ms of failed mmap(2) with MAP_FIXED_NOREPLACE,
// which returns EEXIST immediately for occupied pages).
void* AllocateMemoryNear(void* target, size_t size) {
    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return nullptr;
    uintptr_t targetAddr = (uintptr_t)target;
    uintptr_t targetPage = targetAddr & ~(uintptr_t)(pageSize - 1);
    const uintptr_t MAX_DIST = 120UL * 1024 * 1024;

    auto tryOnce = [&](uintptr_t cand) -> void* {
        void* m = mmap((void*)cand, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (m == MAP_FAILED) return nullptr;
        if (m != (void*)cand) { munmap(m, size); return nullptr; }
        uintptr_t got = (uintptr_t)m;
        if (got > targetAddr + MAX_DIST || targetAddr > got + MAX_DIST) {
            munmap(m, size);
            return nullptr;
        }
        return m;
    };

    for (uintptr_t off = (uintptr_t)pageSize; off < MAX_DIST; off += (uintptr_t)pageSize) {
        if (targetPage >= off) {
            void* m = tryOnce(targetPage - off);
            if (m) return m;
        }
        if (targetPage <= UINTPTR_MAX - off) {
            void* m = tryOnce(targetPage + off);
            if (m) return m;
        }
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

// ─── Feature 1.5 — dynamic runtime attach by computed address ──────

extern "C" HookResult AttachRuntime(uint64_t addr, void* replacement, void** outOrig) {
    if (addr == 0 || replacement == nullptr) return HOOK_INTERNAL_ERROR;
    if ((addr & 3u) != 0) return HOOK_TARGET_INVALID;

    long pageSize = sysconf(_SC_PAGESIZE);
    if (pageSize <= 0) return HOOK_INTERNAL_ERROR;

    uintptr_t targetPage = (uintptr_t)addr & ~((uintptr_t)pageSize - 1);
    int targetProt = 0;
    if (!ReadPageProt(targetPage, &targetProt)) {
        return HOOK_TARGET_INVALID;
    }
    if ((targetProt & PROT_EXEC) == 0) {
        return HOOK_TARGET_INVALID;
    }

    uintptr_t replPage = (uintptr_t)replacement & ~((uintptr_t)pageSize - 1);
    int replProt = 0;
    if (!ReadPageProt(replPage, &replProt)) return HOOK_TARGET_INVALID;
    if ((replProt & PROT_EXEC) == 0) return HOOK_TARGET_INVALID;

    return InstallInlineHookEx((void*)(uintptr_t)addr, replacement, outOrig, "runtime");
}

extern "C" HookResult DetachRuntime(uint64_t addr) {
    if (addr == 0) return HOOK_INTERNAL_ERROR;
    return UninstallInlineHook((void*)(uintptr_t)addr);
}

// ─── Feature 6.8 — Signal-safe deferred uninstall ────────────────────
//
// UninstallInlineHook() is NOT async-signal-safe:
//   - std::lock_guard<mutex>(g_hook_mutex) — pthread_mutex_lock, not on
//     the POSIX async-signal-safe list
//   - FindHandle / RegistryFree — second mutex (g_reg_mutex)
//   - nanosleep(100ms) — not sig-safe, blocks handler for full grace
//   Self-deadlock if signal fires on the thread already holding the lock.
//
// Deferred design: the signal handler enqueues the target into a
// spinlock-protected SPSC ring buffer and writes one byte to an eventfd.
// A dedicated worker thread (spawned by SignalSafeUninstallArm) drains
// the queue and calls UninstallInlineHook() from normal thread context —
// all existing mutex/nanosleep/munmap calls are safe there.
//
// Signal-handler stack usage: < 256 bytes (MINSIGSTKSZ is 8192 = 32×).
// Signal-handler runtime: microseconds (CAS acquire + memcpy + write).
// Zero changes to UninstallInlineHook — it stays normal-context.

#define SIG_UNINSTALL_QUEUE_N 64

static void*         g_sig_queue[SIG_UNINSTALL_QUEUE_N];
static volatile long g_sig_head       = 0;
static volatile long g_sig_tail       = 0;
static volatile int  g_sig_prod_lock  = 0;
static int           g_sig_wakeup_fd  = -1;
static pthread_t     g_sig_worker;
static volatile int  g_sig_armed      = 0;
static volatile int  g_sig_shutdown   = 0;
static volatile long g_sig_enqueued   = 0;
static volatile long g_sig_completed  = 0;
static volatile long g_sig_dropped    = 0;
static volatile long g_sig_failed     = 0;

static long SigUninstallNowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int SigUninstallEnqueue(void* target) {
    int spins = 0;
    while (__atomic_exchange_n(&g_sig_prod_lock, 1, __ATOMIC_ACQ_REL) != 0) {
        if (++spins > 10000) {
            __atomic_fetch_add(&g_sig_dropped, 1, __ATOMIC_RELAXED);
            return -1;
        }
        __asm__ volatile("yield" ::: "memory");
    }
    long h = g_sig_head;
    long t = __atomic_load_n(&g_sig_tail, __ATOMIC_ACQUIRE);
    if (h - t >= SIG_UNINSTALL_QUEUE_N) {
        __atomic_store_n(&g_sig_prod_lock, 0, __ATOMIC_RELEASE);
        __atomic_fetch_add(&g_sig_dropped, 1, __ATOMIC_RELAXED);
        return -1;
    }
    g_sig_queue[h % SIG_UNINSTALL_QUEUE_N] = target;
    __atomic_store_n(&g_sig_head, h + 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_sig_prod_lock, 0, __ATOMIC_RELEASE);
    __atomic_fetch_add(&g_sig_enqueued, 1, __ATOMIC_RELAXED);
    return 0;
}

static int SigUninstallDequeue(void** out) {
    long t = g_sig_tail;
    long h = __atomic_load_n(&g_sig_head, __ATOMIC_ACQUIRE);
    if (t == h) return 0;
    *out = g_sig_queue[t % SIG_UNINSTALL_QUEUE_N];
    __atomic_store_n(&g_sig_tail, t + 1, __ATOMIC_RELEASE);
    return 1;
}

static void* SigUninstallWorkerMain(void*) {
    while (true) {
        struct pollfd pfd = { g_sig_wakeup_fd, POLLIN, 0 };
        int pr = poll(&pfd, 1, 500);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            uint64_t v = 0;
            (void)read(g_sig_wakeup_fd, &v, sizeof(v));
        }
        void* target = nullptr;
        while (SigUninstallDequeue(&target)) {
            HookResult r = UninstallInlineHook(target);
            if (r == HOOK_OK || r == HOOK_NOT_HOOKED) {
                __atomic_fetch_add(&g_sig_completed, 1, __ATOMIC_RELAXED);
            } else {
                __atomic_fetch_add(&g_sig_failed, 1, __ATOMIC_RELAXED);
                LOGE("SigUninstallWorker: UninstallInlineHook(%p) r=%d", target, (int)r);
            }
        }
        if (__atomic_load_n(&g_sig_shutdown, __ATOMIC_ACQUIRE)) break;
    }
    return nullptr;
}

extern "C" int SignalSafeUninstallArm() {
    if (__atomic_load_n(&g_sig_armed, __ATOMIC_ACQUIRE)) return 0;
    // Session 65 fix: reset counters on fresh arm so each Arm/Disarm cycle
    // observes only its own enqueue/completed/failed/dropped counts. Without
    // this, counters accumulate across tests and stats assertions see stale
    // values from prior Arm/Disarm cycles in the same process.
    __atomic_store_n(&g_sig_enqueued,  0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sig_completed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sig_failed,    0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_sig_dropped,   0, __ATOMIC_RELAXED);
    int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (fd < 0) {
        LOGE("SignalSafeUninstallArm: eventfd failed errno=%d", errno);
        return -1;
    }
    g_sig_wakeup_fd = fd;
    __atomic_store_n(&g_sig_shutdown, 0, __ATOMIC_RELEASE);
    int rc = pthread_create(&g_sig_worker, nullptr, SigUninstallWorkerMain, nullptr);
    if (rc != 0) {
        LOGE("SignalSafeUninstallArm: pthread_create rc=%d", rc);
        close(fd);
        g_sig_wakeup_fd = -1;
        return -2;
    }
    __atomic_store_n(&g_sig_armed, 1, __ATOMIC_RELEASE);
    LOGD("SignalSafeUninstallArm: armed, wakeup_fd=%d", fd);
    return 0;
}

extern "C" int SignalSafeUninstallDisarm() {
    if (!__atomic_load_n(&g_sig_armed, __ATOMIC_ACQUIRE)) return 0;
    __atomic_store_n(&g_sig_shutdown, 1, __ATOMIC_RELEASE);
    if (g_sig_wakeup_fd >= 0) {
        uint64_t v = 1;
        (void)write(g_sig_wakeup_fd, &v, sizeof(v));
    }
    pthread_join(g_sig_worker, nullptr);
    if (g_sig_wakeup_fd >= 0) {
        close(g_sig_wakeup_fd);
        g_sig_wakeup_fd = -1;
    }
    __atomic_store_n(&g_sig_armed, 0, __ATOMIC_RELEASE);
    LOGD("SignalSafeUninstallDisarm: (enq=%ld done=%ld fail=%ld drop=%ld)",
         __atomic_load_n(&g_sig_enqueued, __ATOMIC_RELAXED),
         __atomic_load_n(&g_sig_completed, __ATOMIC_RELAXED),
         __atomic_load_n(&g_sig_failed, __ATOMIC_RELAXED),
         __atomic_load_n(&g_sig_dropped, __ATOMIC_RELAXED));
    return 0;
}

extern "C" int SignalSafeUninstallFromHandler(void* target) {
    if (!target) return -2;
    if (!__atomic_load_n(&g_sig_armed, __ATOMIC_ACQUIRE)) return -3;
    int rc = SigUninstallEnqueue(target);
    if (rc != 0) return rc;
    if (g_sig_wakeup_fd >= 0) {
        uint64_t v = 1;
        ssize_t w = write(g_sig_wakeup_fd, &v, sizeof(v));
        (void)w;
    }
    return 0;
}

// Session 65 fix: WaitFor must wait for TWO things, not one:
//   1. Target out of the registry (FindHandle==null)  — happens mid-UninstallInlineHook
//   2. Stats counters reflect the enqueue             — happens AFTER
//      UninstallInlineHook returns (i.e., after the 100ms nanosleep grace)
// Without #2, WaitFor returns while the worker is still in its deferred-free
// sleep; a caller that then reads stats sees completed=0 and thinks the
// worker failed. The stats read must happen after BOTH conditions.
extern "C" int SignalSafeUninstallWaitFor(void* target, int timeoutMs) {
    if (!target) return -1;
    long deadline = SigUninstallNowMs() + timeoutMs;
    while (SigUninstallNowMs() < deadline) {
        bool reg_clear = !FindHandle(target);
        long enq  = __atomic_load_n(&g_sig_enqueued,  __ATOMIC_ACQUIRE);
        long done = __atomic_load_n(&g_sig_completed, __ATOMIC_ACQUIRE);
        long fail = __atomic_load_n(&g_sig_failed,    __ATOMIC_ACQUIRE);
        if (reg_clear && (done + fail) >= enq) return 0;
        struct timespec ts = { 0, 5 * 1000000L };
        nanosleep(&ts, nullptr);
    }
    bool reg_clear = !FindHandle(target);
    long enq  = __atomic_load_n(&g_sig_enqueued,  __ATOMIC_ACQUIRE);
    long done = __atomic_load_n(&g_sig_completed, __ATOMIC_ACQUIRE);
    long fail = __atomic_load_n(&g_sig_failed,    __ATOMIC_ACQUIRE);
    if (reg_clear && (done + fail) >= enq) return 0;
    return -1;
}

extern "C" void SignalSafeUninstallStats(long out[4]) {
    if (!out) return;
    out[0] = __atomic_load_n(&g_sig_enqueued,  __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&g_sig_completed, __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&g_sig_failed,    __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&g_sig_dropped,   __ATOMIC_RELAXED);
}