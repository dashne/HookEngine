#include "ServiceManagerJni.h"
#include "JavaHook.h"
#include <android/log.h>
#include <cstring>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" bool ParcelRewriter_IsNullOverridden(const char* name);
extern "C" bool ParcelRewriter_FindAlias(const char* name, char* out_target, size_t out_size);

#define MAX_NAME_LEN 128

static JavaHook* g_h_get   = nullptr;
static JavaHook* g_h_check = nullptr;

static jobject (*_orig_getService)(JNIEnv*, jclass, jstring)   = nullptr;
static jobject (*_orig_checkService)(JNIEnv*, jclass, jstring) = nullptr;

typedef jobject (*OrigFn)(JNIEnv*, jclass, jstring);

static jobject DispatchLookup(JNIEnv* env, jclass cls, jstring name, OrigFn orig) {
    if (!env || !orig) return nullptr;
    if (!name) return orig(env, cls, name);

    const char* cname = env->GetStringUTFChars(name, nullptr);
    if (!cname) return orig(env, cls, name);

    if (ParcelRewriter_IsNullOverridden(cname)) {
        LOGD("SvcMgrJni: '%s' → null override", cname);
        env->ReleaseStringUTFChars(name, cname);
        return nullptr;
    }

    char target[MAX_NAME_LEN];
    if (ParcelRewriter_FindAlias(cname, target, sizeof(target))) {
        LOGD("SvcMgrJni: '%s' → alias '%s'", cname, target);
        env->ReleaseStringUTFChars(name, cname);
        jstring jtarget = env->NewStringUTF(target);
        if (!jtarget) return orig(env, cls, name);
        jobject result = orig(env, cls, jtarget);
        env->DeleteLocalRef(jtarget);
        return result;
    }

    env->ReleaseStringUTFChars(name, cname);
    return orig(env, cls, name);
}

static jobject FakeGetService(JNIEnv* env, jclass cls, jstring name) {
    return DispatchLookup(env, cls, name, _orig_getService);
}

static jobject FakeCheckService(JNIEnv* env, jclass cls, jstring name) {
    return DispatchLookup(env, cls, name, _orig_checkService);
}

// Direct hook path — GetStaticMethodID returns the raw ArtMethod* on
// AOSP ART. If MIUI/API 37 returns a real pointer here (vs the 0x3d token
// FromReflectedMethod gave us), we can hook without touching reflection.
static JavaHook* TryInstallByMethodID(JNIEnv* env, jclass smCls, const char* methodName,
                                      void* replacement, void** out_orig,
                                      const char* label) {
    const char* sig = "(Ljava/lang/String;)Landroid/os/IBinder;";
    jmethodID mid = env->GetStaticMethodID(smCls, methodName, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    LOGD("SvcMgrJniAttach: %s '%s' GetStaticMethodID=0x%lx",
         label, methodName, (unsigned long)(uintptr_t)mid);
    if (!mid) return nullptr;
    JavaHook* h = JavaHookInstallByMethodID(env, mid, replacement, out_orig, label);
    if (h) {
        LOGD("SvcMgrJniAttach: %s installed via direct jmethodID", label);
        return h;
    }
    LOGD("SvcMgrJniAttach: %s direct install failed", label);
    return nullptr;
}

// Reflection fallback — same as the previous code path, but now clearly
// labeled so the log shows which stage produced what value.
static JavaHook* TryInstallByReflection(JNIEnv* env, jclass smCls, const char* methodName,
                                        void* replacement, void** out_orig,
                                        const char* label) {
    const char* sig = "(Ljava/lang/String;)Landroid/os/IBinder;";
    jmethodID mid = env->GetStaticMethodID(smCls, methodName, sig);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!mid) return nullptr;
    jobject refl = env->ToReflectedMethod(smCls, mid, JNI_TRUE);
    if (!refl) {
        LOGD("SvcMgrJniAttach: %s ToReflectedMethod null", label);
        return nullptr;
    }
    jmethodID mid2 = env->FromReflectedMethod(refl);
    LOGD("SvcMgrJniAttach: %s '%s' FromReflectedMethod(refl)=0x%lx",
         label, methodName, (unsigned long)(uintptr_t)mid2);
    JavaHook* h = JavaHookInstall(env, refl, replacement, out_orig, label);
    env->DeleteLocalRef(refl);
    if (h) {
        LOGD("SvcMgrJniAttach: %s installed via reflection", label);
        return h;
    }
    LOGD("SvcMgrJniAttach: %s reflection install failed", label);
    return nullptr;
}

static JavaHook* TryInstall(JNIEnv* env, jclass smCls, const char* methodName,
                            void* replacement, void** out_orig, const char* label) {
    JavaHook* h = TryInstallByMethodID(env, smCls, methodName, replacement, out_orig, label);
    if (h) return h;
    return TryInstallByReflection(env, smCls, methodName, replacement, out_orig, label);
}

extern "C" int SvcMgrJniAttach(JNIEnv* env) {
    if (!env) return -1;

    // Session 51 fix: only short-circuit when BOTH are installed. Previous
    // `(g_h_get || g_h_check)` reported success even when only one was
    // installed, hiding partial failures.
    if (g_h_get && g_h_check) {
        LOGD("SvcMgrJniAttach: both hooks already installed");
        return 0;
    }

    jclass smCls = env->FindClass("android/os/ServiceManager");
    if (!smCls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGE("SvcMgrJniAttach: FindClass(ServiceManager) failed");
        return -2;
    }

    int installed = 0;

    // Each install is guarded independently so a partial install is retried.
    if (!g_h_get) {
        g_h_get = TryInstall(env, smCls, "getService",
                             (void*)FakeGetService, (void**)&_orig_getService,
                             "getService");
        if (g_h_get) installed++;
    } else {
        installed++;
    }

    if (!g_h_check) {
        g_h_check = TryInstall(env, smCls, "checkService",
                               (void*)FakeCheckService, (void**)&_orig_checkService,
                               "checkService");
        if (g_h_check) installed++;
    } else {
        installed++;
    }

    env->DeleteLocalRef(smCls);

    if (installed == 0) {
        LOGE("SvcMgrJniAttach: no method hookable");
        return -3;
    }
    LOGD("SvcMgrJniAttach: %d method(s) hooked", installed);
    return 0;
}

extern "C" void SvcMgrJniDetach() {
    if (g_h_get)   { JavaHookUninstall(g_h_get);   g_h_get = nullptr; }
    if (g_h_check) { JavaHookUninstall(g_h_check); g_h_check = nullptr; }
    _orig_getService = nullptr;
    _orig_checkService = nullptr;
}

extern "C" int SvcMgrJniCount() {
    int n = 0;
    if (g_h_get)   n++;
    if (g_h_check) n++;
    return n;
}