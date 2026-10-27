#include <jni.h>
#include <cstdint>
#include <android/log.h>
#include "Backtrace.h"

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

static BtFrame g_bt_frames[BT_MAX_FRAMES];
static int     g_bt_n = 0;

// ─── Fixture chain: three distinct non-inlined functions ─────────────
// volatile local forces each function to establish its own frame so the
// FP-chain walk can see all three. At -O0 they already push, but the
// volatile ensures correctness if a future build uses -O2.

extern "C" __attribute__((noinline)) int bt_fixture_c() {
    volatile int local = 0;
    g_bt_n = BacktraceCaptureHere(g_bt_frames, BT_MAX_FRAMES);
    return local;
}

extern "C" __attribute__((noinline)) int bt_fixture_b() {
    volatile int local = 0;
    int r = bt_fixture_c();
    return r + local + 1;
}

extern "C" __attribute__((noinline)) int bt_fixture_a() {
    volatile int local = 0;
    int r = bt_fixture_b();
    return r + local + 1;
}

// ─── JNI bridges ─────────────────────────────────────────────────────

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btRunChain(JNIEnv*, jobject) {
    g_bt_n = 0;
    return (jint)bt_fixture_a();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btTrigger(JNIEnv*, jobject) {
    g_bt_n = BacktraceCaptureHere(g_bt_frames, BT_MAX_FRAMES);
    return (jint)g_bt_n;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btFrameCount(JNIEnv*, jobject) {
    return (jint)g_bt_n;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btFramePc(JNIEnv*, jobject, jint idx) {
    if (idx < 0 || idx >= g_bt_n) return 0;
    return (jlong)g_bt_frames[idx].pc;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btFrameFp(JNIEnv*, jobject, jint idx) {
    if (idx < 0 || idx >= g_bt_n) return 0;
    return (jlong)g_bt_frames[idx].fp;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btFixtureAAddr(JNIEnv*, jobject) {
    return (jlong)(uintptr_t)&bt_fixture_a;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btFixtureBAddr(JNIEnv*, jobject) {
    return (jlong)(uintptr_t)&bt_fixture_b;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btFixtureCAddr(JNIEnv*, jobject) {
    return (jlong)(uintptr_t)&bt_fixture_c;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_btFilterOutLib(
        JNIEnv* env, jobject, jstring jlib) {
    if (!env || !jlib) return g_bt_n;
    const char* lib = env->GetStringUTFChars(jlib, nullptr);
    if (!lib) return g_bt_n;
    int new_n = BacktraceFilterOutLib(g_bt_frames, g_bt_n, lib);
    env->ReleaseStringUTFChars(jlib, lib);
    g_bt_n = new_n;
    return (jint)new_n;
}