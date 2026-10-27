#include "DexLoader.h"
#include <android/log.h>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" jobject DexLoaderCreate(JNIEnv* env, jstring dexPath,
                                   jstring optimizedDir, jobject parent) {
    if (!env || !dexPath || !optimizedDir) return nullptr;

    jclass dclCls = env->FindClass("dalvik/system/DexClassLoader");
    if (!dclCls) {
        env->ExceptionClear();
        LOGE("DexLoader: DexClassLoader class not found");
        return nullptr;
    }

    // Signature: DexClassLoader(String dexPath, String optimizedDirectory,
    //                           String librarySearchPath, ClassLoader parent)
    jmethodID ctor = env->GetMethodID(dclCls, "<init>",
                                      "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/ClassLoader;)V");
    if (!ctor) {
        env->ExceptionClear();
        env->DeleteLocalRef(dclCls);
        LOGE("DexLoader: DexClassLoader ctor not found");
        return nullptr;
    }

    // librarySearchPath = nullptr (no native libs to load from this dex)
    jobject result = env->NewObject(dclCls, ctor, dexPath, optimizedDir, nullptr, parent);

    if (env->ExceptionCheck()) {
        // Common failure: "Attempt to register dex file ... with multiple class loaders"
        // when the dex path is already loaded by another loader. Logged so caller
        // knows which wall they hit.
        jthrowable ex = env->ExceptionOccurred();
        env->ExceptionClear();
        LOGE("DexLoader: DexClassLoader ctor threw (dex may already be loaded by another loader)");
        if (ex) env->DeleteLocalRef(ex);
        result = nullptr;
    }

    env->DeleteLocalRef(dclCls);
    if (result) LOGD("DexLoader: DexClassLoader created successfully");
    return result;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_DexLoader_nativeCreate(
        JNIEnv* env, jobject /*this*/, jstring dexPath, jstring optDir, jobject parent) {
    return DexLoaderCreate(env, dexPath, optDir, parent);
}