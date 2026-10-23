#include <jni.h>
#include <android/log.h>
#include "Hooks.h"
#include "ProcessHooks.h"
#include "AntiDetection.h"

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" int UninstallAllHooks();

static bool g_engine_loaded = false;
static char g_dex_path[1024] = {0};
static char g_lib_path[1024] = {0};

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_DualApplication_nativeOnLoad(
        JNIEnv* env, jobject, jstring dexPath, jstring libPath) {
    if (g_engine_loaded) { LOGD("nativeOnLoad: already loaded"); return; }

    const char* d = env->GetStringUTFChars(dexPath, nullptr);
    const char* l = env->GetStringUTFChars(libPath, nullptr);
    if (d) { strncpy(g_dex_path, d, sizeof(g_dex_path) - 1); env->ReleaseStringUTFChars(dexPath, d); }
    if (l) { strncpy(g_lib_path, l, sizeof(g_lib_path) - 1); env->ReleaseStringUTFChars(libPath, l); }

    LOGD("nativeOnLoad: dex=%s lib=%s", g_dex_path, g_lib_path);
    g_engine_loaded = true;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_DualApplication_nativeAttachBaseContext(
        JNIEnv* env, jobject, jobject base) {
    if (!g_engine_loaded) {
        LOGE("nativeAttachBaseContext: engine not loaded");
        return;
    }

    // 1. Native GOT+inline hooks (file io, uid, dlopen, properties)
    int rc = InstallHooks();
    LOGD("nativeAttachBaseContext: InstallHooks rc=%d", rc);
    if (rc != 0) {
        LOGE("nativeAttachBaseContext: InstallHooks failed, rolling back");
        UninstallAllHooks();
        return;
    }

    // 2. Process hooks (getpid, getppid, exec*, system, ptrace, kill, myPpid)
    int n = InstallProcessHooks(env);
    // 3. Anti-detection (connect-port blocking, status filter via Hooks.cpp)

    LOGD("nativeAttachBaseContext: InstallProcessHooks installed=%d", n);
    int a = InstallAntiDetection(env);
    LOGD("nativeAttachBaseContext: InstallAntiDetection installed=%d", a);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_DualApplication_nativeOnCreate(
        JNIEnv*, jobject) {
    LOGD("nativeOnCreate: engine fully initialized");
}