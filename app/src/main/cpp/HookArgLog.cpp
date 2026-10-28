#include "HookArgLog.h"
#include <android/log.h>
#include <cstring>
#include <sys/syscall.h>
#include <unistd.h>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" void HookArgLogInit(HookArgLog* log, void* target, int captures_fp) {
    if (!log) return;
    memset(log, 0, sizeof(*log));
    log->enabled     = false;
    log->head        = 0;
    log->count       = 0;
    log->mem_bytes   = (uint32_t)sizeof(HookArgLog);
    log->captures_fp = captures_fp;
    log->target      = target;
}

extern "C" void HookArgLogClear(HookArgLog* log) {
    if (!log) return;
    __atomic_store_n(&log->count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&log->head,  0, __ATOMIC_RELAXED);
    // enabled, mem_bytes, captures_fp, target preserved
}

extern "C" void HookArgLogCaptureFull(HookArgLog* log, const uint64_t* frame_saved) {
    if (!log || !frame_saved) return;
    uint32_t idx = __atomic_fetch_add(&log->head, 1, __ATOMIC_RELAXED) % HOOK_ARGLOG_RING_SIZE;
    HookArgCapture* c = &log->ring[idx];
    for (int i = 0; i < 8; i++) c->gp[i] = frame_saved[i];
    for (int i = 0; i < 8; i++) c->fp[i] = frame_saved[22 + i * 2];  // d0..d7 (low half of q0..q7)
    c->pc = (uint64_t)log->target;
    __atomic_fetch_add(&log->count, 1, __ATOMIC_RELAXED);
}

extern "C" void HookArgLogCaptureGp(HookArgLog* log, const uint64_t* x0_x7) {
    if (!log || !x0_x7) return;
    uint32_t idx = __atomic_fetch_add(&log->head, 1, __ATOMIC_RELAXED) % HOOK_ARGLOG_RING_SIZE;
    HookArgCapture* c = &log->ring[idx];
    for (int i = 0; i < 8; i++) c->gp[i] = x0_x7[i];
    for (int i = 0; i < 8; i++) c->fp[i] = 0;
    c->pc = (uint64_t)log->target;
    __atomic_fetch_add(&log->count, 1, __ATOMIC_RELAXED);
}

// ─── Subsystem dispatch ─────────────────────────────────────────────
extern "C" bool ChainHook_ArgLogSnapshot(void* target, HookArgLog* out);
extern "C" bool ChainHook_ArgLogEnable(void* target);
extern "C" bool ChainHook_ArgLogDisable(void* target);
extern "C" bool ChainHook_ArgLogReset(void* target);
extern "C" bool ChainHook_ArgLogReadNth(void* target, uint32_t ringIdx, HookArgCapture* out);

extern "C" bool RetMod_ArgLogSnapshot(void* target, HookArgLog* out);
extern "C" bool RetMod_ArgLogEnable(void* target);
extern "C" bool RetMod_ArgLogDisable(void* target);
extern "C" bool RetMod_ArgLogReset(void* target);
extern "C" bool RetMod_ArgLogReadNth(void* target, uint32_t ringIdx, HookArgCapture* out);

extern "C" bool HookArgLogSnapshot(void* target, HookArgLog* out) {
    if (!target || !out) return false;
    if (ChainHook_ArgLogSnapshot(target, out)) return true;
    if (RetMod_ArgLogSnapshot(target, out))    return true;
    return false;
}

extern "C" bool HookArgLogEnable(void* target) {
    if (!target) return false;
    if (ChainHook_ArgLogEnable(target)) return true;
    if (RetMod_ArgLogEnable(target))    return true;
    return false;
}

extern "C" bool HookArgLogDisable(void* target) {
    if (!target) return false;
    if (ChainHook_ArgLogDisable(target)) return true;
    if (RetMod_ArgLogDisable(target))    return true;
    return false;
}

extern "C" bool HookArgLogReset(void* target) {
    if (!target) return false;
    if (ChainHook_ArgLogReset(target)) return true;
    if (RetMod_ArgLogReset(target))    return true;
    return false;
}

extern "C" bool HookArgLogReadNth(void* target, uint32_t ringIdx, HookArgCapture* out) {
    if (!target || !out || ringIdx >= HOOK_ARGLOG_RING_SIZE) return false;
    if (ChainHook_ArgLogReadNth(target, ringIdx, out)) return true;
    if (RetMod_ArgLogReadNth(target, ringIdx, out))    return true;
    return false;
}

// ─── Fixtures — GP and FP targets for HookArgLogTest ─────────────────
//
// argLogFixtureGp: naked — args stay in x0..x7 at entry, so a ChainHook
// capture at the entry instruction sees them directly. Sum return value
// is a sanity check (expected = 11+22+33+44+55+66+77+88 = 396 for the
// canonical test call).
//
// argLogFixtureFp: normal C++ (noinline) — args in d0..d3 at entry,
// capture reads d0..d3 from the naked chain_dispatch_naked frame's q0..q3
// slots. Returns sum of doubles (1.5+2.5+3.5+4.5 = 12.0 for canonical).
//
// argLogNoopHandler: satisfies ChainHook's handler signature. Does nothing.
// argLogRetModNoop: satisfies ReturnModifierFn. Returns orig unchanged.

extern "C" __attribute__((noinline, naked, used))
int argLogFixtureGp(int a, int b, int c, int d, int e, int f, int g, int h) {
    __asm__ volatile(
            "add w0, w0, w1\n"
            "add w0, w0, w2\n"
            "add w0, w0, w3\n"
            "add w0, w0, w4\n"
            "add w0, w0, w5\n"
            "add w0, w0, w6\n"
            "add w0, w0, w7\n"
            "ret\n"
            );
}

extern "C" __attribute__((noinline, used))
double argLogFixtureFp(double a, double b, double c, double d) {
    return a + b + c + d;
}

extern "C" __attribute__((used))
void argLogNoopHandler(const uint64_t* x0_x7) {
    (void)x0_x7;
}

extern "C" __attribute__((used))
uint64_t argLogRetModNoop(const uint64_t* x0_x7, uint64_t orig) {
    (void)x0_x7;
    return orig;
}

// ─── JNI bridges ─────────────────────────────────────────────────────
#include <jni.h>

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_getArgLogFixtureGpAddr(JNIEnv*, jobject) {
    return (jlong)(uintptr_t)&argLogFixtureGp;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_callArgLogFixtureGp(
        JNIEnv*, jobject, jint a, jint b, jint c, jint d, jint e, jint f, jint g, jint h) {
    return (jint)argLogFixtureGp((int)a, (int)b, (int)c, (int)d,
                                 (int)e, (int)f, (int)g, (int)h);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_getArgLogFixtureFpAddr(JNIEnv*, jobject) {
    return (jlong)(uintptr_t)&argLogFixtureFp;
}

extern "C" JNIEXPORT jdouble JNICALL
Java_com_ims134_hookengine_NativeTestBridge_callArgLogFixtureFp(
        JNIEnv*, jobject, jdouble a, jdouble b, jdouble c, jdouble d) {
    return (jdouble)argLogFixtureFp((double)a, (double)b, (double)c, (double)d);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_getArgLogNoopHandlerAddr(JNIEnv*, jobject) {
    return (jlong)(uintptr_t)&argLogNoopHandler;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_getArgLogRetModNoopAddr(JNIEnv*, jobject) {
    return (jlong)(uintptr_t)&argLogRetModNoop;
}

// ─── JNI: hookArgLog* family ─────────────────────────────────────────
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_hookArgLogEnable(JNIEnv*, jobject, jlong target) {
    return HookArgLogEnable((void*)(uintptr_t)target) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_hookArgLogDisable(JNIEnv*, jobject, jlong target) {
    return HookArgLogDisable((void*)(uintptr_t)target) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_hookArgLogReset(JNIEnv*, jobject, jlong target) {
    return HookArgLogReset((void*)(uintptr_t)target) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_hookArgLogLookup(JNIEnv*, jobject, jlong target) {
    HookArgLog tmp{};
    return HookArgLogSnapshot((void*)(uintptr_t)target, &tmp) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_hookArgLogCount(JNIEnv*, jobject, jlong target) {
    HookArgLog tmp{};
    if (!HookArgLogSnapshot((void*)(uintptr_t)target, &tmp)) return -1;
    return (jint)tmp.count;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_hookArgLogMemBytes(JNIEnv*, jobject, jlong target) {
    HookArgLog tmp{};
    if (!HookArgLogSnapshot((void*)(uintptr_t)target, &tmp)) return -1;
    return (jint)tmp.mem_bytes;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_hookArgLogCapturesFp(JNIEnv*, jobject, jlong target) {
    HookArgLog tmp{};
    if (!HookArgLogSnapshot((void*)(uintptr_t)target, &tmp)) return JNI_FALSE;
    return tmp.captures_fp ? JNI_TRUE : JNI_FALSE;
}

// Returns long[17] = { gp0..gp7, fp0..fp7, pc }. Null on out-of-range
// ring index or target not found.
extern "C" JNIEXPORT jlongArray JNICALL
Java_com_ims134_hookengine_NativeTestBridge_hookArgLogRead(
        JNIEnv* env, jobject, jlong target, jint ringIdx) {
    if (!env) return nullptr;
    if (ringIdx < 0 || ringIdx >= HOOK_ARGLOG_RING_SIZE) return nullptr;
    HookArgCapture cap{};
    if (!HookArgLogReadNth((void*)(uintptr_t)target, (uint32_t)ringIdx, &cap))
        return nullptr;
    jlong out[17];
    for (int i = 0; i < 8; i++) out[i]     = (jlong)cap.gp[i];
    for (int i = 0; i < 8; i++) out[8 + i] = (jlong)cap.fp[i];
    out[16] = (jlong)cap.pc;
    jlongArray arr = env->NewLongArray(17);
    if (!arr) return nullptr;
    env->SetLongArrayRegion(arr, 0, 17, out);
    return arr;
}