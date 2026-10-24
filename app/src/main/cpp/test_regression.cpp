// Regression tests for historical bugs 2, 3 (stub), 4, 5.
// Bug 1 lives in native-lib.cpp (needs access to static HookJavaMethod).
//
// Bug 2 — Fake_dlopen inline rescan caused execmod denials.
// Bug 3 — MIUI routes through sPackageManager, not mPM (Kotlin-side).
// Bug 4 — relay page leak after ~510 installs.
// Bug 5 — trampoline lifetime under contention.

#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#include "HookRegistry.h"
#include "TestReport.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);
extern "C" void SetUninstallGraceMs(long);
extern "C" long GetUninstallGraceMs();
extern "C" int UninstallAllHooks();
extern "C" bool ShouldHookDlopenForTest(const char*);


extern "C" int g_iihe_quiet;
extern "C" int g_relay_alloc_count;
extern "C" int g_relay_free_count;

// ─── Bug 2: ShouldHookDlopen invariant ──────────────────────────────
extern "C" int RunRegressionBug2() {
    struct Case { const char* path; bool expect; };
    Case cases[] = {
            {"libart.so",               true},
            {"libnativeloader.so",      true},
            {"libnativebridge.so",      true},
            {"/data/app/foo/lib.so",    true},
            {"/data/user/0/foo/lib.so", true},
            {"/data/data/foo/lib.so",   true},
            {"libfoo.so",               false},
            {"",                        false},
            {nullptr,                   false},
    };

    bool all_ok = true;
    std::string bad;
    for (auto& c : cases) {
        bool got = ShouldHookDlopenForTest(c.path);
        if (got != c.expect) {
            all_ok = false;
            bad += std::string(c.path ? c.path : "<null>")
                   + "=" + (got ? "true" : "false") + " ";
        }
    }
    testreport::Report("bug2_dlopen_hooks_disabled", all_ok,
                       all_ok ? "all 9 paths correct" : ("wrong: " + bad));
    return all_ok ? 1 : 0;
}

// ─── Bug 3: Kotlin-only, stub here for dispatch ─────────────────────
// ─── Bug 3: Kotlin-only, stub here for dispatch ─────────────────────
extern "C" int RunRegressionBug3() {
    testreport::Report("bug3_kotlin_only", true,
                       "tested by RegressionTest.bug3_both_pm_fields_patched");
    return 1;
}

// ─── Bug 4: relay page leak ─────────────────────────────────────────
extern "C" __attribute__((noinline, naked, used))
int fn_bug4_target(int x) {
    __asm__ volatile("add w0, w0, #1\nret\n");
}
extern "C" __attribute__((noinline, used))
int fake_bug4(int x) { (void)x; return 0xBEEF; }

extern "C" int RunRegressionBug4() {
    UninstallAllHooks();
    long savedGrace = GetUninstallGraceMs();
    SetUninstallGraceMs(0);

    int allocs_before = g_relay_alloc_count;
    int frees_before  = g_relay_free_count;
    uint32_t baseline = *(volatile uint32_t*)&fn_bug4_target;

    int bad = 0;
    int completed = 0;
    // Session 15 — suppress verbose IIHE(...) debug output across the
    // 1000-cycle stress loop. ~15,000 lines of log noise otherwise, which
    // drowns out other tests' output in the run log. LOGE still prints.
    g_iihe_quiet = 1;
    for (int i = 0; i < 1000; i++) {
        HookResult r = InstallInlineHookEx((void*)&fn_bug4_target,
                                           (void*)&fake_bug4, nullptr, "bug4");
        if (r != HOOK_OK) { bad++; break; }
        HookResult u = UninstallInlineHook((void*)&fn_bug4_target);
        if (u != HOOK_OK) { bad++; break; }
        completed++;
    }
    g_iihe_quiet = 0;

    int allocs_after = g_relay_alloc_count;
    int frees_after  = g_relay_free_count;
    uint32_t final_bytes = *(volatile uint32_t*)&fn_bug4_target;
    int reg_after = RegistryCount();

    int alloc_delta = allocs_after - allocs_before;
    int free_delta  = frees_after  - frees_before;

    char d1[128];
    snprintf(d1, sizeof d1, "cycles=%d bad=%d alloc=%d free=%d",
             completed, bad, alloc_delta, free_delta);
    testreport::Report("bug4_no_relay_leak",
                       bad == 0 && completed == 1000 && alloc_delta == 1000 && free_delta == 1000, d1);

    char d2[64];
    snprintf(d2, sizeof d2, "before=0x%x after=0x%x", baseline, final_bytes);
    testreport::Report("bug4_target_bytes_restored", baseline == final_bytes, d2);

    char d3[64];
    snprintf(d3, sizeof d3, "registry=%d", reg_after);
    testreport::Report("bug4_inline_count_zero", reg_after == 0, d3);

    SetUninstallGraceMs(savedGrace);
    return 3;
}

// ─── Bug 5: trampoline lifetime under contention ────────────────────
extern "C" __attribute__((noinline, naked, used))
int fn_bug5_target(int x) {
    __asm__ volatile("add w0, w0, #1\nret\n");
}
extern "C" __attribute__((noinline, used))
int fake_bug5(int x) { (void)x; return 0xDEAD; }

struct Bug5Ctx {
    std::atomic<bool> stop{false};
    std::atomic<int> orig{0};
    std::atomic<int> fake{0};
    std::atomic<int> bad{0};
};

static void* bug5_caller(void* arg) {
    Bug5Ctx* c = (Bug5Ctx*)arg;
    while (!c->stop.load(std::memory_order_acquire)) {
        int r = fn_bug5_target(10);
        if (r == 11)            c->orig.fetch_add(1, std::memory_order_relaxed);
        else if (r == 0xDEAD)   c->fake.fetch_add(1, std::memory_order_relaxed);
        else                    c->bad.fetch_add(1, std::memory_order_relaxed);
    }
    return nullptr;
}

extern "C" int RunRegressionBug5() {
    UninstallAllHooks();

    long savedGrace = GetUninstallGraceMs();
    SetUninstallGraceMs(20);

    int allocs_before = g_relay_alloc_count;
    int frees_before  = g_relay_free_count;

    HookResult r = InstallInlineHookEx((void*)&fn_bug5_target,
                                       (void*)&fake_bug5, nullptr, "bug5");
    if (r != HOOK_OK) {
        testreport::Report("bug5_no_bad_returns", false, "install failed");
        testreport::Report("bug5_counters_balanced", false, "install failed");
        testreport::Report("bug5_target_restored", false, "install failed");
        SetUninstallGraceMs(savedGrace);
        return 0;
    }

    Bug5Ctx c;
    pthread_t t[3];
    for (int i = 0; i < 3; i++) pthread_create(&t[i], nullptr, bug5_caller, &c);

    struct timespec ts; ts.tv_sec = 0; ts.tv_nsec = 50L * 1000 * 1000;
    nanosleep(&ts, nullptr);

    UninstallInlineHook((void*)&fn_bug5_target);

    c.stop.store(true, std::memory_order_release);
    for (int i = 0; i < 3; i++) pthread_join(t[i], nullptr);

    SetUninstallGraceMs(savedGrace);

    int post = fn_bug5_target(10);
    int alloc_delta = g_relay_alloc_count - allocs_before;
    int free_delta  = g_relay_free_count  - frees_before;

    char d1[96];
    snprintf(d1, sizeof d1, "orig=%d fake=%d bad=%d",
             c.orig.load(), c.fake.load(), c.bad.load());
    testreport::Report("bug5_no_bad_returns", c.bad.load() == 0, d1);

    char d2[96];
    snprintf(d2, sizeof d2, "alloc=%d free=%d", alloc_delta, free_delta);
    testreport::Report("bug5_counters_balanced",
                       alloc_delta == 1 && free_delta == 1, d2);

    char d3[64];
    snprintf(d3, sizeof d3, "post=%d", post);
    testreport::Report("bug5_target_restored", post == 11, d3);

    return 3;
}