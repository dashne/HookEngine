#include <jni.h>
#include <signal.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstdint>
#include <android/log.h>
#include "SignalOverride.h"

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

// _NSIG = 65 on bionic — compile-time constant, unlike SIGRTMAX.
static constexpr int kNSig = _NSIG;

// Plain integers accessed via __atomic_*_n primitives (async-signal-safe,
// no std::atomic constructor overhead). Same pattern as SvcHook.cpp.
static int      g_last_si_code   = 0;
static int      g_last_pid       = 0;
static int      g_last_uid       = 0;
static int      g_last_sival_int = 0;
static uint64_t g_last_pc        = 0;

static void record_siginfo(siginfo_t* info, void* ucv) {
    __atomic_store_n(&g_last_si_code,   info ? info->si_code            : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_pid,       info ? (int)info->si_pid        : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_uid,       info ? (int)info->si_uid        : 0, __ATOMIC_RELAXED);
    __atomic_store_n(&g_last_sival_int, info ? info->si_value.sival_int : 0, __ATOMIC_RELAXED);
    if (ucv) {
        ucontext_t* uc = (ucontext_t*)ucv;
        __atomic_store_n(&g_last_pc, (uint64_t)uc->uc_mcontext.pc, __ATOMIC_RELAXED);
    } else {
        __atomic_store_n(&g_last_pc, (uint64_t)0, __ATOMIC_RELAXED);
    }
}

static SignalOverrideAction fixture_pass_record(int, siginfo_t* info, void* ucv, void*) {
    record_siginfo(info, ucv);
    return SIGOVR_PASS_THROUGH;
}

static SignalOverrideAction fixture_suppress_record(int, siginfo_t* info, void* ucv, void*) {
    record_siginfo(info, ucv);
    return SIGOVR_SUPPRESS;
}

// ─── Raw sigaction installation for chain-to-previous tests ──────────
static int               g_raw_sigaction_count = 0;
static void raw_sigaction_handler(int, siginfo_t*, void*) {
    __atomic_fetch_add(&g_raw_sigaction_count, 1, __ATOMIC_RELAXED);
}
static struct sigaction  g_raw_saved[kNSig] = {};
static int               g_raw_installed[kNSig] = {};

// ─── JNI bridges ─────────────────────────────────────────────────────

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrInstall(
        JNIEnv*, jobject, jint signo, jint handlerId, jlong /*userCtx*/) {
    SignalOverrideHandler h = nullptr;
    if (handlerId == 0) h = fixture_pass_record;
    else if (handlerId == 1) h = fixture_suppress_record;
    else return -1;
    return (jint)SignalOverrideInstall((int)signo, h, nullptr);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrUninstall(JNIEnv*, jobject, jint signo) {
    return (jint)SignalOverrideUninstall((int)signo);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrCount(JNIEnv*, jobject) {
    return (jint)SignalOverrideCount();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrFireCount(JNIEnv*, jobject, jint signo) {
    return (jint)SignalOverrideFireCount((int)signo);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrResetAll(JNIEnv*, jobject) {
    SignalOverrideResetAll();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrLastSiCode(JNIEnv*, jobject) {
    return (jint)__atomic_load_n(&g_last_si_code, __ATOMIC_RELAXED);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrLastPid(JNIEnv*, jobject) {
    return (jint)__atomic_load_n(&g_last_pid, __ATOMIC_RELAXED);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrLastUid(JNIEnv*, jobject) {
    return (jint)__atomic_load_n(&g_last_uid, __ATOMIC_RELAXED);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrLastSivalInt(JNIEnv*, jobject) {
    return (jint)__atomic_load_n(&g_last_sival_int, __ATOMIC_RELAXED);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrLastPc(JNIEnv*, jobject) {
    return (jlong)__atomic_load_n(&g_last_pc, __ATOMIC_RELAXED);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrRaiseWithInt(
        JNIEnv*, jobject, jint signo, jint sival) {
    pid_t pid = (pid_t)syscall(SYS_getpid);
    union sigval value;
    value.sival_int = (int)sival;
    return (jint)sigqueue(pid, (int)signo, value);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrInstallRawSigaction(
        JNIEnv*, jobject, jint signo) {
    if (signo <= 0 || signo >= kNSig) return -1;
    if (signo == SIGKILL || signo == SIGSTOP) return -1;
    if (g_raw_installed[signo]) return 0;

    struct sigaction sa = {};
    sa.sa_sigaction = raw_sigaction_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, signo);

    if (sigaction((int)signo, &sa, &g_raw_saved[signo]) != 0) return -2;
    g_raw_installed[signo] = 1;
    __atomic_store_n(&g_raw_sigaction_count, 0, __ATOMIC_RELAXED);
    return 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrRestoreRawSigaction(
        JNIEnv*, jobject, jint signo) {
    if (signo <= 0 || signo >= kNSig) return;
    if (!g_raw_installed[signo]) return;
    sigaction((int)signo, &g_raw_saved[signo], nullptr);
    g_raw_installed[signo] = 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrRawSigactionCount(JNIEnv*, jobject) {
    return (jint)__atomic_load_n(&g_raw_sigaction_count, __ATOMIC_RELAXED);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_sigovrCurrentHandlerKind(
        JNIEnv*, jobject, jint signo) {
    if (signo <= 0 || signo >= kNSig) return -1;
    struct sigaction cur = {};
    if (sigaction((int)signo, nullptr, &cur) != 0) return -2;
    if (cur.sa_handler == SIG_DFL) return 0;
    if (cur.sa_handler == SIG_IGN) return 1;
    return 2;
}