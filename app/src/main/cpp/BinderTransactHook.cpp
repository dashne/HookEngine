#include <jni.h>
#include <android/log.h>
#include <atomic>
#include "JavaHook.h"

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// Reference: public static native boolean tnFIN(Object obj, int i, Parcel parcel, Parcel parcel2, int i2);
// Target   : BinderProxy.transactNative(int code, Parcel data, Parcel reply, int flags) -> boolean
typedef jboolean (*BinderTransact_t)(JNIEnv*, jobject, jint, jobject, jobject, jint);
static BinderTransact_t _orig_binder_transact = nullptr;
static JavaHook* g_binder_hook = nullptr;
static std::atomic<int> g_transact_count{0};
static std::atomic<int> g_transact_last_code{0};

static jboolean FakeBinderTransact(JNIEnv* env, jobject thiz, jint code,
                                   jobject data, jobject reply, jint flags) {
    g_transact_count.fetch_add(1, std::memory_order_relaxed);
    g_transact_last_code.store(code, std::memory_order_relaxed);

    if (!_orig_binder_transact) {
        LOGE("FakeBinderTransact: no original (code=%d)", (int)code);
        return JNI_FALSE;
    }
    return _orig_binder_transact(env, thiz, code, data, reply, flags);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_installNative(JNIEnv* env, jobject, jobject methodObj) {
    if (g_binder_hook) {
        LOGD("BinderTransactHook: already installed");
        return 0;
    }
    if (!env || !methodObj) return -1;
    JavaHook* h = JavaHookInstall(env, methodObj, (void*)FakeBinderTransact,
                                  (void**)&_orig_binder_transact,
                                  "BinderProxy.transactNative");
    if (!h) {
        LOGE("BinderTransactHook: JavaHookInstall returned null");
        return -2;
    }
    g_binder_hook = h;
    LOGD("BinderTransactHook: installed hook=%p orig=%p",
         (void*)g_binder_hook, (void*)_orig_binder_transact);
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_uninstallNative(JNIEnv*, jobject) {
    if (!g_binder_hook) return 0;
    HookResult r = JavaHookUninstall(g_binder_hook);
    g_binder_hook = nullptr;
    _orig_binder_transact = nullptr;
    LOGD("BinderTransactHook: uninstalled r=%d", (int)r);
    return (r == HOOK_OK) ? 1 : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_fireCountNative(JNIEnv*, jobject) {
    return g_transact_count.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_lastCodeNative(JNIEnv*, jobject) {
    return g_transact_last_code.load(std::memory_order_relaxed);
}