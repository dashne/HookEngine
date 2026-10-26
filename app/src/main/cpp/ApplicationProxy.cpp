// ApplicationProxy.cpp
// Session 21 — ApplicationProxy-shape native lifecycle.
//
// ApplicationProxy is an Application subclass with native onCreate /
// onTerminate / onConfigurationChanged / onDestroy. Container creates
// it via getInstance(), calls setBaseContext(guestCtx), then onCreate().
// Native onCreate reads getBaseContext(), instantiates the guest
// Application from its class name, calls attachBaseContext and onCreate
// on it. No LoadedApk, no ActivityThread — matching the reference whose
// ApplicationProxy.mLoadedApk is null.

#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <string>
#include <mutex>

#define TAG "DualCore.Native"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static std::mutex g_guest_mutex;
static jobject    g_guest_app = nullptr;
static JavaVM*    g_vm        = nullptr;

// ─── Exception formatting ──────────────────────────────────────────
static void LogAndClearException(JNIEnv* env, const char* where) {
    if (!env->ExceptionCheck()) return;
    jthrowable ex = env->ExceptionOccurred();
    env->ExceptionClear();
    if (!ex) { LOGE("%s: exception (no throwable)", where); return; }

    jclass swCls = env->FindClass("java/io/StringWriter");
    jclass pwCls = env->FindClass("java/io/PrintWriter");
    jclass thCls = env->FindClass("java/lang/Throwable");
    std::string out = "<failed>";
    if (swCls && pwCls && thCls) {
        jmethodID swInit  = env->GetMethodID(swCls, "<init>", "()V");
        jmethodID swToStr = env->GetMethodID(swCls, "toString", "()Ljava/lang/String;");
        jmethodID pwInit  = env->GetMethodID(pwCls, "<init>", "(Ljava/io/Writer;)V");
        jmethodID pwFlush = env->GetMethodID(pwCls, "flush", "()V");
        jmethodID printM  = env->GetMethodID(thCls, "printStackTrace", "(Ljava/io/PrintWriter;)V");
        jobject sw = swInit ? env->NewObject(swCls, swInit) : nullptr;
        jobject pw = (sw && pwInit) ? env->NewObject(pwCls, pwInit, sw) : nullptr;
        if (pw && printM) {
            env->CallVoidMethod(ex, printM, pw);
            if (pwFlush) env->CallVoidMethod(pw, pwFlush, nullptr);
        }
        if (sw && swToStr) {
            jstring trace = (jstring)env->CallObjectMethod(sw, swToStr);
            if (trace) {
                const char* c = env->GetStringUTFChars(trace, nullptr);
                if (c) { out = c; env->ReleaseStringUTFChars(trace, c); }
                env->DeleteLocalRef(trace);
            }
        }
        if (pw) env->DeleteLocalRef(pw);
        if (sw) env->DeleteLocalRef(sw);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (swCls) env->DeleteLocalRef(swCls);
    if (pwCls) env->DeleteLocalRef(pwCls);
    if (thCls) env->DeleteLocalRef(thCls);
    env->DeleteLocalRef(ex);
    LOGE("%s: %s", where, out.c_str());
}

// ─── Get guestCtx from ApplicationProxy.getBaseContext() ───────────
static jobject GetGuestContext(JNIEnv* env, jobject proxy) {
    jclass appCls = env->FindClass("android/app/Application");
    jmethodID getBaseM = env->GetMethodID(appCls, "getBaseContext",
                                          "()Landroid/content/Context;");
    env->DeleteLocalRef(appCls);
    if (!getBaseM) return nullptr;
    jobject ctx = env->CallObjectMethod(proxy, getBaseM);
    if (env->ExceptionCheck()) { env->ExceptionClear(); ctx = nullptr; }
    return ctx;
}

// ─── Boot guest Application from guestCtx (attach + create) ────────
static jobject BootGuestFromContext(JNIEnv* env, jobject guestCtx) {
    jclass ctxCls = env->FindClass("android/content/Context");
    jmethodID getAiM = env->GetMethodID(ctxCls, "getApplicationInfo",
                                        "()Landroid/content/pm/ApplicationInfo;");
    jmethodID getClM = env->GetMethodID(ctxCls, "getClassLoader",
                                        "()Ljava/lang/ClassLoader;");
    env->DeleteLocalRef(ctxCls);

    jobject ai = env->CallObjectMethod(guestCtx, getAiM);
    jobject loader = env->CallObjectMethod(guestCtx, getClM);
    if (!ai || !loader) {
        LOGD("BootGuest: guestCtx missing applicationInfo or classLoader");
        if (ai) env->DeleteLocalRef(ai);
        if (loader) env->DeleteLocalRef(loader);
        return nullptr;
    }

    jclass aiCls = env->FindClass("android/content/pm/ApplicationInfo");
    jfieldID srcF = env->GetFieldID(aiCls, "sourceDir", "Ljava/lang/String;");
    jfieldID clsF = env->GetFieldID(aiCls, "className", "Ljava/lang/String;");
    env->DeleteLocalRef(aiCls);
    jstring jApk = (jstring)env->GetObjectField(ai, srcF);
    jstring jCls = (jstring)env->GetObjectField(ai, clsF);
    if (!jApk || !jCls) {
        LOGD("BootGuest: sourceDir or className null");
        if (jApk) env->DeleteLocalRef(jApk);
        if (jCls) env->DeleteLocalRef(jCls);
        env->DeleteLocalRef(ai);
        env->DeleteLocalRef(loader);
        return nullptr;
    }
    const char* cs = env->GetStringUTFChars(jCls, nullptr);
    LOGD("BootGuest: guestApplication=%s", cs ? cs : "?");
    if (cs) env->ReleaseStringUTFChars(jCls, cs);

    // Load guest Application class
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClassM = env->GetMethodID(clCls, "loadClass",
                                            "(Ljava/lang/String;)Ljava/lang/Class;");
    env->DeleteLocalRef(clCls);
    jobject guestCls = env->CallObjectMethod(loader, loadClassM, jCls);
    if (env->ExceptionCheck()) { LogAndClearException(env, "loadClass"); guestCls = nullptr; }

    jobject guestApp = nullptr;
    if (guestCls) {
        // Instantiate via no-arg constructor
        jclass clsCls = env->FindClass("java/lang/Class");
        jmethodID getCtorM = env->GetMethodID(clsCls, "getDeclaredConstructor",
                                              "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;");
        env->DeleteLocalRef(clsCls);
        jclass emptyCls = env->FindClass("java/lang/Class");
        jobjectArray emptyParams = env->NewObjectArray(0, emptyCls, nullptr);
        env->DeleteLocalRef(emptyCls);
        jobject ctor = env->CallObjectMethod(guestCls, getCtorM, emptyParams);
        env->DeleteLocalRef(emptyParams);

        if (ctor) {
            jclass ctorCls = env->FindClass("java/lang/reflect/Constructor");
            jmethodID setAccM = env->GetMethodID(ctorCls, "setAccessible", "(Z)V");
            jmethodID newInstM = env->GetMethodID(ctorCls, "newInstance",
                                                  "([Ljava/lang/Object;)Ljava/lang/Object;");
            env->DeleteLocalRef(ctorCls);
            if (setAccM && newInstM) {
                env->CallVoidMethod(ctor, setAccM, (jboolean)JNI_TRUE);
                if (env->ExceptionCheck()) env->ExceptionClear();
                jclass objCls = env->FindClass("java/lang/Object");
                jobjectArray emptyArgs = env->NewObjectArray(0, objCls, nullptr);
                env->DeleteLocalRef(objCls);
                guestApp = env->CallObjectMethod(ctor, newInstM, emptyArgs);
                env->DeleteLocalRef(emptyArgs);
                if (env->ExceptionCheck()) {
                    LogAndClearException(env, "Constructor.newInstance");
                    guestApp = nullptr;
                }
            }
            env->DeleteLocalRef(ctor);
        }
        env->DeleteLocalRef(guestCls);
    }

    // attachBaseContext + onCreate
    if (guestApp) {
        jclass appCls = env->FindClass("android/app/Application");
        jmethodID attachM = env->GetMethodID(appCls, "attachBaseContext",
                                             "(Landroid/content/Context;)V");
        jmethodID onCreateM = env->GetMethodID(appCls, "onCreate", "()V");
        env->DeleteLocalRef(appCls);
        if (attachM) {
            env->CallVoidMethod(guestApp, attachM, guestCtx);
            if (env->ExceptionCheck()) {
                LogAndClearException(env, "guest attachBaseContext");
                env->DeleteLocalRef(guestApp);
                guestApp = nullptr;
            }
        }
        if (guestApp && onCreateM) {
            env->CallVoidMethod(guestApp, onCreateM);
            if (env->ExceptionCheck()) {
                LogAndClearException(env, "guest onCreate");
                env->DeleteLocalRef(guestApp);
                guestApp = nullptr;
            }
        }
    }

    env->DeleteLocalRef(jApk);
    env->DeleteLocalRef(jCls);
    env->DeleteLocalRef(ai);
    env->DeleteLocalRef(loader);
    return guestApp;
}

// ─── JNI: ApplicationProxy.nativeOnCreate — boots guest ────────────
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnCreate(JNIEnv* env, jobject thiz) {
    LOGD("ApplicationProxy.onCreate: entry");
    if (!env) return;
    if (!g_vm) env->GetJavaVM(&g_vm);

    jobject guestCtx = GetGuestContext(env, thiz);
    if (!guestCtx) {
        LOGE("ApplicationProxy.onCreate: no guestCtx (setBaseContext not called?)");
        return;
    }

    jobject guestApp = BootGuestFromContext(env, guestCtx);
    env->DeleteLocalRef(guestCtx);
    if (!guestApp) {
        LOGE("ApplicationProxy.onCreate: guest boot failed");
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_guest_mutex);
        if (g_guest_app) env->DeleteGlobalRef(g_guest_app);
        g_guest_app = env->NewGlobalRef(guestApp);
        env->DeleteLocalRef(guestApp);
    }
    LOGD("ApplicationProxy.onCreate: guest boot complete");
}

// ─── JNI: ApplicationProxy.nativeOnTerminate ───────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnTerminate(JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    if (!g_guest_app) return;
    jclass appCls = env->FindClass("android/app/Application");
    jmethodID m = env->GetMethodID(appCls, "onTerminate", "()V");
    env->DeleteLocalRef(appCls);
    if (m) {
        env->CallVoidMethod(g_guest_app, m);
        if (env->ExceptionCheck()) LogAndClearException(env, "guest onTerminate");
    }
}

// ─── JNI: ApplicationProxy.nativeOnConfigurationChanged ────────────
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnConfigurationChanged(
        JNIEnv* env, jobject /*thiz*/, jobject newConfig) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    if (!g_guest_app) return;
    jclass appCls = env->FindClass("android/app/Application");
    jmethodID m = env->GetMethodID(appCls, "onConfigurationChanged",
                                   "(Landroid/content/res/Configuration;)V");
    env->DeleteLocalRef(appCls);
    if (m) {
        env->CallVoidMethod(g_guest_app, m, newConfig);
        if (env->ExceptionCheck()) LogAndClearException(env, "guest onConfigurationChanged");
    }
}

// ─── JNI: ApplicationProxy.nativeOnDestroy ─────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnDestroy(JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    if (g_guest_app) {
        env->DeleteGlobalRef(g_guest_app);
        g_guest_app = nullptr;
    }
    LOGD("ApplicationProxy.onDestroy: guest ref cleared");
}

// ─── JNI: ApplicationProxy.nativeGetGuestApplication ───────────────
extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeGetGuestApplication(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    return g_guest_app;
}

// ─── JNI: ApplicationProxy.nativeBuildGuestResources ───────────────
// Builds Resources whose AssetManager has the guest APK loaded.
// AssetManager.addAssetPath and Resources(AM, DM, Config) are @hide
// but JNI bypasses hidden-API gating.
extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeBuildGuestResources(
        JNIEnv* env, jobject /*thiz*/, jstring jApkPath, jobject jHostContext) {
    if (!jApkPath || !jHostContext) return nullptr;

    const char* apkPath = env->GetStringUTFChars(jApkPath, nullptr);
    if (!apkPath) return nullptr;

    jobject resources = nullptr;
    jobject am = nullptr;
    jobject hostRes = nullptr;
    jobject metrics = nullptr;
    jobject config = nullptr;

    do {
        jclass amCls = env->FindClass("android/content/res/AssetManager");
        if (!amCls) { LOGE("buildGuestRes: no AssetManager"); break; }
        jmethodID amCtor = env->GetMethodID(amCls, "<init>", "()V");
        if (!amCtor) { LOGE("buildGuestRes: no AM ctor"); env->DeleteLocalRef(amCls); break; }
        am = env->NewObject(amCls, amCtor);
        if (env->ExceptionCheck()) { LogAndClearException(env, "new AssetManager"); am = nullptr; }
        env->DeleteLocalRef(amCls);
        if (!am) break;

        jclass amCls2 = env->FindClass("android/content/res/AssetManager");
        jmethodID addPathM = env->GetMethodID(amCls2, "addAssetPath",
                                              "(Ljava/lang/String;)I");
        env->DeleteLocalRef(amCls2);
        if (!addPathM) { LOGE("buildGuestRes: no addAssetPath"); break; }
        jstring jApk = env->NewStringUTF(apkPath);
        jint addRc = env->CallIntMethod(am, addPathM, jApk);
        env->DeleteLocalRef(jApk);
        if (env->ExceptionCheck()) { LogAndClearException(env, "addAssetPath"); break; }
        LOGD("buildGuestRes: addAssetPath(%s) -> %d", apkPath, (int)addRc);
        if (addRc == 0) { LOGE("buildGuestRes: addAssetPath returned 0"); break; }

        jclass ctxCls = env->FindClass("android/content/Context");
        jmethodID getResM = env->GetMethodID(ctxCls, "getResources",
                                             "()Landroid/content/res/Resources;");
        env->DeleteLocalRef(ctxCls);
        if (!getResM) { LOGE("buildGuestRes: no getResources"); break; }
        hostRes = env->CallObjectMethod(jHostContext, getResM);
        if (env->ExceptionCheck()) { env->ExceptionClear(); hostRes = nullptr; }
        if (!hostRes) { LOGE("buildGuestRes: hostRes null"); break; }

        jclass resCls = env->FindClass("android/content/res/Resources");
        if (!resCls) { LOGE("buildGuestRes: no Resources"); break; }
        jmethodID getMetricsM = env->GetMethodID(resCls, "getDisplayMetrics",
                                                 "()Landroid/util/DisplayMetrics;");
        jmethodID getConfigM = env->GetMethodID(resCls, "getConfiguration",
                                                "()Landroid/content/res/Configuration;");
        jmethodID resCtor = env->GetMethodID(resCls, "<init>",
                                             "(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V");
        if (!getMetricsM || !getConfigM || !resCtor) {
            LOGE("buildGuestRes: missing Resources methods");
            env->DeleteLocalRef(resCls);
            break;
        }
        metrics = env->CallObjectMethod(hostRes, getMetricsM);
        config = env->CallObjectMethod(hostRes, getConfigM);
        if (env->ExceptionCheck()) { env->ExceptionClear(); }

        resources = env->NewObject(resCls, resCtor, am, metrics, config);
        env->DeleteLocalRef(resCls);
        if (env->ExceptionCheck()) {
            LogAndClearException(env, "new Resources");
            resources = nullptr;
        }
        LOGD("buildGuestRes: Resources=%p", (void*)resources);
    } while (0);

    if (metrics) env->DeleteLocalRef(metrics);
    if (config) env->DeleteLocalRef(config);
    if (hostRes) env->DeleteLocalRef(hostRes);
    if (am) env->DeleteLocalRef(am);

    env->ReleaseStringUTFChars(jApkPath, apkPath);
    return resources;
}