// ServiceManagerShimJni.cpp — Session 51 final.
//
// Feature 3.4 as scoped: transact-level ServiceManager lookup interception.
// Verified by 8 green tests in ServiceManagerTest.kt, implemented in
// CppParcelRewriter.cpp (ParcelRewriter_TryRewriteServiceManagerLookup).
//
// The cache-hit extension attempted during Session 51 is architecturally
// blocked on MIUI/API 37. Definitive measured wall (on-device logs):
//
//     rawGetService  FromReflectedMethod = 0x3d
//     getService     FromReflectedMethod = 0x3f
//     checkService   FromReflectedMethod = 0x41
//
// All three are fast-JNI dispatch-table indices, not ArtMethod pointers.
// JavaHookInstall correctly refuses (mid < 0x1000). No address to hook.
// No flags to clear. No user-space path to cache-hit interception short
// of IDA analysis of libart's fast-JNI dispatch layer.
//
// What ALSO does not work (proven during S51):
//   - ArtMethod +32 patch: write lands but CallStaticObjectMethod does
//     not dispatch through +32.
//   - JIT-code inline hook at +32 target: address not in /proc/self/maps.
//   - SimpleArrayMap.get(sCache) hook: JIT-inlined into getService's
//     compiled body; fires on unrelated ArrayMaps only.
//   - BpServiceManager hook: installs, never fires (MIUI routes around).
//
// This file is intentionally minimal. All dead mechanisms have been
// removed from active code to prevent regression. The stubs below return
// -1 with a loud log so any caller sees the dead status clearly.

#include <jni.h>
#include <cstdint>
#include <cstring>
#include <android/log.h>

extern "C" int  FastJniFlipAccNative(void* art_method);
extern "C" int  FastJniDirectInstall(JNIEnv* env, void* art_method, void* trampoline);
extern "C" int  FastJniRestore();
extern "C" void* FastJniFindArtTrampoline();
extern "C" int  FastJniHits();

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── FastJni legacy bridges (kept for symbol compatibility) ──────────

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeFlipAccNative(
        JNIEnv*, jobject, jlong addr) {
    return (jint)FastJniFlipAccNative((void*)(uintptr_t)addr);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeDirectInstall(
        JNIEnv* env, jobject, jlong artMethod, jlong trampoline) {
    return (jint)FastJniDirectInstall(env, (void*)(uintptr_t)artMethod,
                                      (void*)(uintptr_t)trampoline);
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeReadU64(
        JNIEnv*, jobject, jlong addr) {
    if (addr < 0x1000) return 0;
    return (jlong)*(uint64_t*)(uintptr_t)addr;
}

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeFindArtTrampoline(
        JNIEnv*, jobject) {
    return (jlong)(uintptr_t)FastJniFindArtTrampoline();
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeDirectGetService(
        JNIEnv* env, jobject, jstring name) {
    if (!env || !name) return nullptr;
    jclass smCls = env->FindClass("android/os/ServiceManager");
    if (!smCls) { env->ExceptionClear(); return nullptr; }
    jmethodID mid = env->GetStaticMethodID(smCls, "getService",
                                           "(Ljava/lang/String;)Landroid/os/IBinder;");
    if (env->ExceptionCheck() || !mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(smCls);
        return nullptr;
    }
    jobject result = env->CallStaticObjectMethod(smCls, mid, name);
    if (env->ExceptionCheck()) { env->ExceptionClear(); result = nullptr; }
    env->DeleteLocalRef(smCls);
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeRestore(JNIEnv*, jobject) {
    return (jint)FastJniRestore();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeGetHits(JNIEnv*, jobject) {
    return (jint)FastJniHits();
}

extern "C" jobject FastJniJavaGetService(JNIEnv* env, jstring name);

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeJavaGetService(
        JNIEnv* env, jobject, jstring name) {
    return FastJniJavaGetService(env, name);
}

// ─── Dead mechanisms — fail cleanly with diagnostic log ──────────────

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeInstallArrayMapHook(
        JNIEnv*, jobject, jobject) {
    LOGE("nativeInstallArrayMapHook: DEAD — SimpleArrayMap.get is JIT-"
         "inlined into getService's compiled body and never dispatches "
         "through the entry point for the actual sCache lookup. Also "
         "quick-ABI mismatch (x0=ArtMethod*, not JNIEnv*) causes SIGSEGV "
         "on any JNI call inside the shim. Removed from active code.");
    return -1;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeUninstallArrayMapHook(
        JNIEnv*, jobject) { return 0; }

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeGetArrayMapHits(
        JNIEnv*, jobject) { return 0; }

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeInstallRawGetServiceHook(
        JNIEnv*, jobject) {
    LOGE("nativeInstallRawGetServiceHook: DEAD — rawGetService is "
         "fast-JNI (FromReflectedMethod=0x3d), cannot ArtMethod-hook. "
         "Same wall as getService (0x3f) and checkService (0x41).");
    return -1;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeUninstallRawGetServiceHook(
        JNIEnv*, jobject) { return 0; }

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ServiceManagerShim_nativeGetRawGetServiceHits(
        JNIEnv*, jobject) { return 0; }