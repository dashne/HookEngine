#include <jni.h>
#include <cstdint>
#include <android/log.h>
#include "PltResolver.h"

#define TAG "DualCore"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_pltResolve(
        JNIEnv* env, jobject, jstring jlib, jstring jsym) {
    if (!env || !jlib || !jsym) return 0;
    const char* lib = env->GetStringUTFChars(jlib, nullptr);
    const char* sym = env->GetStringUTFChars(jsym, nullptr);
    if (!lib || !sym) {
        if (lib) env->ReleaseStringUTFChars(jlib, lib);
        if (sym) env->ReleaseStringUTFChars(jsym, sym);
        return 0;
    }
    uint64_t addr = PltResolveForSymbol(lib, sym);
    env->ReleaseStringUTFChars(jlib, lib);
    env->ReleaseStringUTFChars(jsym, sym);
    return (jlong)addr;
}

// Calls the resolved PLT entry as if it were the imported function.
// For `open(const char* pathname, int flags, ...)` with flags=O_RDONLY (0),
// passing mode=0 is legal (mode ignored when O_CREAT/O_TMPFILE not set).
//
// If the PLT entry address is correct (points to `open@plt`), calling it
// returns a valid fd. If the address is wrong (different symbol's stub),
// the call dispatches to a different function with (const char*, int, int)
// arguments — virtually always returning -1 or a nonsensical fd. Any offset
// error is caught by test `pltEntryCallableForOpen`.
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_pltCallOpen(
        JNIEnv* env, jobject, jlong pltAddr, jstring jpath, jint flags) {
    if (!env || pltAddr <= 0 || !jpath) return -2;
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    if (!path) return -2;

    typedef int (*OpenFn)(const char*, int, int);
    OpenFn fn = (OpenFn)(uintptr_t)pltAddr;
    int fd = fn(path, (int)flags, 0);

    env->ReleaseStringUTFChars(jpath, path);
    return (jint)fd;
}