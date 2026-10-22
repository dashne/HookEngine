#include <jni.h>
#include <string>
#include <android/log.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include "Hooks.h"
#include "RedirectTable.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" int32_t JNICALL FakeBinderGetCallingUid(JNIEnv*, jclass) {
    return g_virtualUid;
}
typedef int32_t (*GetCallingUidFunc)(JNIEnv*, jclass);
GetCallingUidFunc _OriginalGetCallingUid = nullptr;

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

    *originalFunc = *entryPoint;
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
    if (HookJavaMethod(env, methodObj, (void*)FakeBinderGetCallingUid, (void**)&_OriginalGetCallingUid))
        LOGD("JNI Hook installed.");
    else
        LOGE("JNI Hook failed.");
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

// ---- Inline hook test ----
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

// ---- HiddenApi ----
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

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_RemoteService_setHiddenApiExemptionsNative(
        JNIEnv* env, jobject) {
    jclass vmRuntimeClass = env->FindClass("dalvik/system/VMRuntime");
    if (!vmRuntimeClass) {
        LOGE("VMRuntime class missing"); env->ExceptionClear(); return JNI_FALSE;
    }
    jmethodID getRuntime = env->GetStaticMethodID(
            vmRuntimeClass, "getRuntime", "()Ldalvik/system/VMRuntime;");
    if (!getRuntime) {
        LOGE("getRuntime not found"); env->ExceptionClear(); return JNI_FALSE;
    }
    jmethodID setExemptions = env->GetMethodID(
            vmRuntimeClass, "setHiddenApiExemptions", "([Ljava/lang/String;)V");
    if (!setExemptions) {
        LOGE("setHiddenApiExemptions not found"); env->ExceptionClear(); return JNI_FALSE;
    }
    jobject runtime = env->CallStaticObjectMethod(vmRuntimeClass, getRuntime);
    if (env->ExceptionCheck() || !runtime) {
        LOGE("getRuntime failed"); env->ExceptionDescribe(); env->ExceptionClear(); return JNI_FALSE;
    }
    jclass stringClass = env->FindClass("java/lang/String");
    jstring prefix = env->NewStringUTF("L");
    jobjectArray arr = env->NewObjectArray(1, stringClass, prefix);
    env->CallVoidMethod(runtime, setExemptions, arr);
    if (env->ExceptionCheck()) {
        LOGE("setHiddenApiExemptions threw"); env->ExceptionDescribe(); env->ExceptionClear(); return JNI_FALSE;
    }
    LOGD("setHiddenApiExemptions([\"L\"]) succeeded");
    return JNI_TRUE;
}