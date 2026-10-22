// Item 3 tests — concurrency.
//
// Test A (installed_state) — proves the hook actually fires under
//   concurrent load. Installs the hook once, runs 5 caller threads ×
//   5000 calls each, and asserts seenFake > 0. This is the assertion
//   the previous Item 3 test lacked; it can only pass if dispatch
//   through the relay→fake path actually occurred. Then uninstalls
//   (with the deferred-free grace period) and verifies restoration.
//
// Test B (transition_stress) — proves install/uninstall churn is clean.
//   Installer thread cycles 50× install→uninstall. Three caller threads
//   hammer the target for the duration. Every return is classified as
//   original / fake / invalid. Asserts invalid == 0, post == original,
//   registry == 0.
//
// Test C (installhooks_reentrance) — keep: two parallel InstallHooks()
//   both succeed, no leaks.
//
// Grace-period override is set to 20ms for Test B so 50 cycles finish
// in ~1s instead of 5s. Restored to default (100ms) afterward.

#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#include <atomic>
#include <sstream>
#include <string>
#include "HookRegistry.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);
extern "C" void SetUninstallGraceMs(long);
extern "C" long GetUninstallGraceMs();
int InstallHooks();
extern "C" int UninstallAllHooks();

static const int SENTINEL = 0xBEEF;   // 48879
static const int ORIGINAL = 11;       // fn3_target(10) → 11

extern "C" __attribute__((noinline, naked, used))
int fn3_target(int x) {
    __asm__ volatile("add w0, w0, #1\nret\n");
}

extern "C" __attribute__((noinline, used))
int fake3(int x) { (void)x; return SENTINEL; }

// ─── Test A: installed-state concurrency ─────────────────────────────
struct InstalledCtx {
    std::atomic<int> orig{0};
    std::atomic<int> fake{0};
    std::atomic<int> bad{0};
    int iterations = 5000;
};

static void* installed_caller(void* arg) {
    InstalledCtx* c = (InstalledCtx*)arg;
    for (int i = 0; i < c->iterations; i++) {
        int r = fn3_target(10);
        if (r == ORIGINAL)      c->orig.fetch_add(1, std::memory_order_relaxed);
        else if (r == SENTINEL) c->fake.fetch_add(1, std::memory_order_relaxed);
        else                    c->bad.fetch_add(1, std::memory_order_relaxed);
    }
    return nullptr;
}

static bool run_test_installed_state(std::string& detail) {
    UninstallAllHooks();
    int regBefore = RegistryCount();

    void* tramp = nullptr;
    HookResult hr = InstallInlineHookEx((void*)&fn3_target, (void*)&fake3,
                                        &tramp, "installed");
    if (hr != HOOK_OK) {
        std::ostringstream d; d << "install failed r=" << (int)hr;
        detail = d.str();
        return false;
    }

    InstalledCtx c;
    pthread_t threads[5];
    for (int i = 0; i < 5; i++)
        pthread_create(&threads[i], nullptr, installed_caller, &c);
    for (int i = 0; i < 5; i++)
        pthread_join(threads[i], nullptr);

    UninstallInlineHook((void*)&fn3_target);

    int post = fn3_target(10);
    int regAfter = RegistryCount();

    std::ostringstream d;
    d << "orig=" << c.orig.load()
      << " fake=" << c.fake.load()
      << " bad=" << c.bad.load()
      << " post=" << post
      << " regBefore=" << regBefore
      << " regAfter=" << regAfter;
    detail = d.str();

    int total = c.orig.load() + c.fake.load() + c.bad.load();
    return (total == 5 * c.iterations)
           && (c.fake.load() > 0)          // ← the key assertion
           && (c.bad.load() == 0)
           && (post == ORIGINAL)
           && (regAfter == regBefore);
}

// ─── Test B: transition stress ───────────────────────────────────────
struct TransitionCtx {
    std::atomic<bool> stop{false};
    std::atomic<int> orig{0};
    std::atomic<int> fake{0};
    std::atomic<int> bad{0};
    int cycles = 50;
};

static void* transition_installer(void* arg) {
    TransitionCtx* c = (TransitionCtx*)arg;
    for (int i = 0; i < c->cycles; i++) {
        void* t = nullptr;
        InstallInlineHookEx((void*)&fn3_target, (void*)&fake3, &t, "cycle");
        UninstallInlineHook((void*)&fn3_target);
    }
    return nullptr;
}

static void* transition_caller(void* arg) {
    TransitionCtx* c = (TransitionCtx*)arg;
    while (!c->stop.load(std::memory_order_acquire)) {
        int r = fn3_target(10);
        if (r == ORIGINAL)      c->orig.fetch_add(1, std::memory_order_relaxed);
        else if (r == SENTINEL) c->fake.fetch_add(1, std::memory_order_relaxed);
        else                    c->bad.fetch_add(1, std::memory_order_relaxed);
    }
    return nullptr;
}

static bool run_test_transition(std::string& detail) {
    UninstallAllHooks();
    int regBefore = RegistryCount();

    long savedGrace = GetUninstallGraceMs();
    SetUninstallGraceMs(20);   // fast but still 10,000,000× a relay execution

    TransitionCtx c;
    pthread_t installer, callers[3];
    pthread_create(&installer, nullptr, transition_installer, &c);
    for (int i = 0; i < 3; i++)
        pthread_create(&callers[i], nullptr, transition_caller, &c);
    pthread_join(installer, nullptr);
    c.stop.store(true, std::memory_order_release);
    for (int i = 0; i < 3; i++)
        pthread_join(callers[i], nullptr);

    SetUninstallGraceMs(savedGrace);

    int post = fn3_target(10);
    int regAfter = RegistryCount();

    std::ostringstream d;
    d << "cycles=" << c.cycles
      << " orig=" << c.orig.load()
      << " fake=" << c.fake.load()
      << " bad=" << c.bad.load()
      << " post=" << post
      << " regBefore=" << regBefore
      << " regAfter=" << regAfter;
    detail = d.str();

    return (c.bad.load() == 0)
           && (post == ORIGINAL)
           && (regAfter == regBefore);
}

// ─── Test C: InstallHooks reentrance (unchanged semantics) ──────────
struct ReentCtx { std::atomic<int> ok{0}; std::atomic<int> bad{0}; };

static void* installhooks_thread(void* arg) {
    ReentCtx* c = (ReentCtx*)arg;
    int rc = InstallHooks();
    if (rc == 0) c->ok.fetch_add(1); else c->bad.fetch_add(1);
    return nullptr;
}

static bool run_test_reentrance(std::string& detail) {
    UninstallAllHooks();
    int regBefore = RegistryCount();

    ReentCtx c;
    pthread_t t[2];
    pthread_create(&t[0], nullptr, installhooks_thread, &c);
    pthread_create(&t[1], nullptr, installhooks_thread, &c);
    pthread_join(t[0], nullptr);
    pthread_join(t[1], nullptr);

    int regAfter = RegistryCount();

    std::ostringstream d;
    d << "regBefore=" << regBefore
      << " ok=" << c.ok.load()
      << " bad=" << c.bad.load()
      << " regAfter=" << regAfter;
    detail = d.str();

    return (c.ok.load() == 2)
           && (c.bad.load() == 0)
           && (regBefore == 0)
           && (regAfter > 0);
}

// ─── Driver ─────────────────────────────────────────────────────────
extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_RemoteService_runItem3Tests(JNIEnv* env, jobject) {
    LOGD("=== Item 3 tests START ===");

    std::ostringstream out;
    int pass = 0, fail = 0;
    auto emit = [&](bool ok, const char* name, const std::string& detail) {
        std::string line = std::string(ok ? "[PASS] " : "[FAIL] ")
                           + name + ": " + detail;
        LOGD("%s", line.c_str());
        out << line << "\n";
        if (ok) pass++; else fail++;
    };

    { std::string d; bool ok = run_test_installed_state(d);  emit(ok, "installed_state", d); }
    { std::string d; bool ok = run_test_transition(d);       emit(ok, "transition_stress", d); }
    { std::string d; bool ok = run_test_reentrance(d);       emit(ok, "installhooks_reentrance", d); }

    // Restore clean state for onStartCommand's STEP flow.
    UninstallAllHooks();

    std::ostringstream tail;
    tail << "=== " << pass << " passed, " << fail << " failed ===";
    LOGD("%s", tail.str().c_str());
    out << tail.str();
    return env->NewStringUTF(out.str().c_str());
}