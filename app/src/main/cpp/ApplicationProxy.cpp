// ApplicationProxy.cpp
// Session 20 — ApplicationProxy-shape native lifecycle.
//
// Mirrors reference com.excelliance.kxqp.platform.ApplicationProxy:
// native onCreate / onTerminate / onConfigurationChanged / onDestroy,
// registered as instance methods. Container drives them via the Java
// object; native side boots the guest Application on onCreate.
//
// No LoadedApk. No ActivityThread.getPackageInfoNoCheck. Reference has
// none of those in its native layer (verified via nativeInit /
// nativeOnLoad / sub_335FC / sub_29F08 decomp), and ApplicationProxy's
// mLoadedApk is null because it's constructed via getInstance().
//
// Guest Context carries the info: getBaseContext().getApplicationInfo()
// .sourceDir is the APK path, .className is the guest Application class,
// .getClassLoader() is the guest loader. Native boot reads all from
// that Context — nothing is passed separately.

#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <string>
#include <mutex>

#define TAG "DualCore.Native"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static std::mutex g_guest_mutex;
static jobject g_guest_app = nullptr;   // global ref

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

// ─── Read guestCtx from ApplicationProxy.getBaseContext() ──────────
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

// ─── Boot guest Application from guestCtx (native, Rule 15) ────────
static jobject BootGuestFromContext(JNIEnv* env, jobject guestCtx) {
    // 1. Read packageName, applicationInfo, classLoader from guestCtx
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

    // 2. Read sourceDir + className from ApplicationInfo
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

    // 3. Load guest Application class
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClassM = env->GetMethodID(clCls, "loadClass",
                                            "(Ljava/lang/String;)Ljava/lang/Class;");
    env->DeleteLocalRef(clCls);
    jobject guestCls = env->CallObjectMethod(loader, loadClassM, jCls);
    if (env->ExceptionCheck()) { LogAndClearException(env, "loadClass"); guestCls = nullptr; }

    jobject guestApp = nullptr;
    if (guestCls) {
        // 4. Instantiate
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

    // 5. attachBaseContext + onCreate on guest
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

// ─── JNI: ApplicationProxy.onCreate — boots guest ──────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnCreate(JNIEnv* env, jobject thiz) {
    LOGD("ApplicationProxy.onCreate: entry");

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

// ─── JNI: ApplicationProxy.onTerminate ─────────────────────────────
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

// ─── JNI: ApplicationProxy.onConfigurationChanged ─────────────────
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

// ─── JNI: ApplicationProxy.onDestroy ──────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnDestroy(JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    if (g_guest_app) {
        env->DeleteGlobalRef(g_guest_app);
        g_guest_app = nullptr;
    }
    LOGD("ApplicationProxy.onDestroy: guest ref cleared");
}

// ─── JNI: ApplicationProxy.getGuestApplication ────────────────────
extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ApplicationProxy_getGuestApplication(
        JNIEnv* /*env*/, jclass /*cls*/) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    return g_guest_app;
}