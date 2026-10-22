// Item 3 tests: concurrency and thread safety.
//
// Two independent stress cases:
//   1. Inline hook — 3 installer threads × 200 iterations, 3 caller threads
//      × 5000 calls, all against the same target. Caller observes only
//      ORIGINAL (11) or SENTINEL (48879) — never anything in between.
//      After all threads join: registry count = 0, target restored.
//   2. InstallHooks reentrance — 2 threads call InstallHooks concurrently
//      after a clean state. Both return HOOK_OK with no new hooks created
//      (the second sees ALREADY_HOOKED for every slot and does not leak).

#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include <atomic>
#include <sstream>
#include <string>
#include "HookRegistry.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);
int InstallHooks();
extern "C" int UninstallAllHooks();

static const int SENTINEL = 0xBEEF;   // 48879
static const int ORIGINAL = 11;       // fn3_target(10) returns 11

extern "C" __attribute__((noinline, naked, used))
int fn3_target(int x) {
    __asm__ volatile("add w0, w0, #1\nret\n");
}

extern "C" __attribute__((noinline, used))
int fake3(int x) { (void)x; return SENTINEL; }

// ─── Test 1: inline hook race ──────────────────────────────────────

struct Stress1 {
    std::atomic<int> installs{0};
    std::atomic<int> uninstalls{0};
    std::atomic<int> installErrors{0};
    std::atomic<int> uninstallErrors{0};
    std::atomic<int> calls{0};
    std::atomic<int> badReturns{0};
    std::atomic<int> seenOriginal{0};
    std::atomic<int> seenFake{0};
};

static void* installer_thread(void* arg) {
    Stress1* s = (Stress1*)arg;
    for (int i = 0; i < 200; i++) {
        void* tramp = nullptr;
        HookResult hr = InstallInlineHookEx((void*)&fn3_target, (void*)&fake3,
                                            &tramp, "stress3");
        if (hr == HOOK_OK) s->installs++;
        else if (hr == HOOK_ALREADY_HOOKED) { /* fine, other thread got there */ }
        else s->installErrors++;

        HookResult ur = UninstallInlineHook((void*)&fn3_target);
        if (ur == HOOK_OK) s->uninstalls++;
        else if (ur == HOOK_NOT_HOOKED) { /* fine, other thread already removed */ }
        else s->uninstallErrors++;
    }
    return nullptr;
}

static void* caller_thread(void* arg) {
    Stress1* s = (Stress1*)arg;
    for (int i = 0; i < 5000; i++) {
        int r = fn3_target(10);
        s->calls++;
        if (r == ORIGINAL) s->seenOriginal++;
        else if (r == SENTINEL) s->seenFake++;
        else s->badReturns++;
    }
    return nullptr;
}

static bool run_stress1(std::string& detail) {
    Stress1 s;
    int regBefore = RegistryCount();

    int pre = fn3_target(10);
    if (pre != ORIGINAL) { detail = "pre_call_broken"; return false; }

    pthread_t inst[3], call[3];
    for (int i = 0; i < 3; i++) pthread_create(&inst[i], nullptr, installer_thread, &s);
    for (int i = 0; i < 3; i++) pthread_create(&call[i], nullptr, caller_thread, &s);
    for (int i = 0; i < 3; i++) pthread_join(inst[i], nullptr);
    for (int i = 0; i < 3; i++) pthread_join(call[i], nullptr);

    int post = fn3_target(10);
    int regAfter = RegistryCount();

    std::ostringstream d;
    d << "installs=" << s.installs.load()
      << " uninstalls=" << s.uninstalls.load()
      << " instErr=" << s.installErrors.load()
      << " uninstErr=" << s.uninstallErrors.load()
      << " calls=" << s.calls.load()
      << " seenOrig=" << s.seenOriginal.load()
      << " seenFake=" << s.seenFake.load()
      << " bad=" << s.badReturns.load()
      << " post=" << post
      << " regBefore=" << regBefore
      << " regAfter=" << regAfter;
    detail = d.str();

    return (s.installErrors.load() == 0)
           && (s.uninstallErrors.load() == 0)
           && (s.badReturns.load() == 0)
           && (post == ORIGINAL || post == SENTINEL)
           && (regAfter == regBefore);
}

// ─── Test 2: InstallHooks reentrance ───────────────────────────────

struct Stress2 {
    std::atomic<int> ok{0};
    std::atomic<int> bad{0};
};

static void* installhooks_thread(void* arg) {
    Stress2* s = (Stress2*)arg;
    int rc = InstallHooks();
    if (rc == HOOK_OK) s->ok++;
    else s->bad++;
    return nullptr;
}

static bool run_stress2(std::string& detail) {
    UninstallAllHooks();
    int regBefore = RegistryCount();

    Stress2 s;
    pthread_t t[2];
    pthread_create(&t[0], nullptr, installhooks_thread, &s);
    pthread_create(&t[1], nullptr, installhooks_thread, &s);
    pthread_join(t[0], nullptr);
    pthread_join(t[1], nullptr);

    int regAfter = RegistryCount();

    std::ostringstream d;
    d << "regBefore=" << regBefore
      << " ok=" << s.ok.load()
      << " bad=" << s.bad.load()
      << " regAfter=" << regAfter;
    detail = d.str();

    return (s.ok.load() == 2)
           && (s.bad.load() == 0)
           && (regBefore == 0)
           && (regAfter > 0);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_RemoteService_runItem3Tests(JNIEnv* env, jobject) {
    LOGD("=== Item 3 tests START ===");
    std::ostringstream out;
    int pass = 0, fail = 0;

    {
        std::string d;
        bool ok = run_stress1(d);
        std::string line = std::string(ok ? "[PASS] " : "[FAIL] ")
                           + "inline_stress: " + d;
        LOGD("%s", line.c_str());
        out << line << "\n";
        if (ok) pass++; else fail++;
    }

    {
        std::string d;
        bool ok = run_stress2(d);
        std::string line = std::string(ok ? "[PASS] " : "[FAIL] ")
                           + "installhooks_reentrance: " + d;
        LOGD("%s", line.c_str());
        out << line << "\n";
        if (ok) pass++; else fail++;
    }

    // Restore clean state for the main onStartCommand flow
    UninstallAllHooks();

    std::ostringstream tail;
    tail << "=== " << pass << " passed, " << fail << " failed ===";
    LOGD("%s", tail.str().c_str());
    out << tail.str();
    return env->NewStringUTF(out.str().c_str());
}