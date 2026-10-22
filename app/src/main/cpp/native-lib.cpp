#include <jni.h>
#include <string>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <pthread.h>
#include "Hooks.h"
#include "RedirectTable.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// =============================================================
// Binder.getCallingUid — caller-context-aware spoof
//
// The saved "original" pointer from ArtMethod+24 is NOT reliable
// on this ROM — calling it faults. So we never call it. Instead:
//
//   1) If tls_inSystemCall is set → we're inside our own proxy
//      calling into the framework. Return real UID (syscall).
//   2) If the current thread's name starts with "Binder:" → we're
//      a binder callback thread (framework calling into us).
//      Return real UID.
//   3) Otherwise → guest code. Return the virtual UID.
//
// Thread-name detection is the standard pattern used by
// VirtualApp / Island / Shelter for exactly this reason.
// =============================================================

static __thread int tls_inSystemCall = 0;
static bool _binderCallbackLogged = false;

extern "C" int32_t JNICALL FakeBinderGetCallingUid(JNIEnv*, jclass) {
    // (1) Explicit system-call scope
    if (tls_inSystemCall) {
        return (int32_t)syscall(SYS_getuid);
    }

    // (2) Binder callback thread — name is "Binder:NNNN_X"
    char tname[32] = {0};
    if (pthread_getname_np(pthread_self(), tname, sizeof(tname)) == 0 &&
        strncmp(tname, "Binder:", 7) == 0) {
        if (!_binderCallbackLogged) {
            _binderCallbackLogged = true;
            LOGD("Binder.getCallingUid: callback thread (tname=%s) -> real UID", tname);
        }
        return (int32_t)syscall(SYS_getuid);
    }

    // (3) Guest app path
    return g_virtualUid;
}

bool HookJavaMethod(JNIEnv* env, jobject methodObj, void* newFunc, void** originalFunc) {
    jmethodID methodId = env->FromReflectedMethod(methodObj);
    if (!methodId) { LOGE("jmethodID null"); return false; }
    void* artMethod = (void*)methodId;

    void** entryPoint = nullptr;
    for (int offset = 24; offset <= 48; offset += 8) {
        void* val = *(void**)((uint8_t*)artMethod + offset);
        if (val) {
            Dl_info info;
            if (dladdr(val, &info) && info.dli_fname &&
                (strstr(info.dli_fname, "libandroid_runtime.so") ||
                 strstr(info.dli_fname, "libart.so"))) {
                entryPoint = (void**)((uint8_t*)artMethod + offset);
                break;
            }
        }
    }
    if (!entryPoint) { LOGE("no native entry in ArtMethod"); return false; }

    *originalFunc = *entryPoint;   // saved for logging only — NOT called
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t pg = (uintptr_t)entryPoint & ~(ps - 1);
    if (mprotect((void*)pg, ps, PROT_READ | PROT_WRITE) != 0) return false;
    *entryPoint = newFunc;
    mprotect((void*)pg, ps, PROT_READ);
    __builtin___clear_cache((char*)artMethod, (char*)artMethod + 64);

    LOGD("ArtMethod hook at offset %d", (int)((uint8_t*)entryPoint - (uint8_t*)artMethod));
    return true;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_hookBinderMethod(JNIEnv* env, jobject, jobject methodObj) {
    void* saved = nullptr;
    if (HookJavaMethod(env, methodObj, (void*)FakeBinderGetCallingUid, &saved))
        LOGD("JNI Hook installed.");
    else
        LOGE("JNI Hook failed.");
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_setInSystemCall(JNIEnv*, jobject, jboolean on) {
    tls_inSystemCall = on ? 1 : 0;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_setVirtualUid(JNIEnv*, jobject, jint uid) {
    SetVirtualUid((int32_t)uid);
    LOGD("Virtual UID set to %d", (int)uid);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_installNativeHooks(JNIEnv*, jobject) {
    InstallHooks();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_addRedirectRule(
        JNIEnv* env, jobject, jstring from, jstring to) {
    const char* f = env->GetStringUTFChars(from, nullptr);
    const char* t = env->GetStringUTFChars(to, nullptr);
    RedirectTable::GetInstance().AddRule(f, t);
    LOGD("Redirect rule: %s -> %s", f, t);
    env->ReleaseStringUTFChars(from, f);
    env->ReleaseStringUTFChars(to, t);
}

// =============================================================
// Inline hook (Item 4)
// =============================================================
extern "C" bool InstallInlineHook(void*, void*, void**);
static pid_t (*_Original_getpid)() = nullptr;
static pid_t Fake_getpid() { return 12345; }

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_nativeGetpid(JNIEnv*, jobject) {
    return (jint)getpid();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_RemoteService_inlineHookGetpid(JNIEnv*, jobject) {
    void* target = dlsym(RTLD_DEFAULT, "getpid");
    if (!target) { LOGE("getpid symbol not found"); return JNI_FALSE; }
    bool ok = InstallInlineHook(target, (void*)Fake_getpid, (void**)&_Original_getpid);
    LOGD("inline hook getpid: %s, trampoline=%p", ok ? "OK" : "FAILED", _Original_getpid);
    return ok ? JNI_TRUE : JNI_FALSE;
}

// =============================================================
// HiddenApi probes (0 on API 37 — greylist @hide works anyway)
// =============================================================
extern "C" int HiddenApi_Install();
extern "C" int HiddenApi_Probe();

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_hiddenApiProbe(JNIEnv*, jobject) {
    return HiddenApi_Probe();
}
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_hiddenApiInstall(JNIEnv*, jobject) {
    return HiddenApi_Install();
}