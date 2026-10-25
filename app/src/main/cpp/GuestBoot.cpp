// GuestBoot.cpp
// Session 20 — native guest Application boot.
//
// Rule 15: engine behavior in native. Rule 2: pull-driven — matches the
// reference's ApplicationProxy.onCreate, which is a native method.
// ApplicationProxy delegates into libkxqpplatform.so's native onCreate,
// which reads the guest APK path, instantiates the real guest Application
// via reflection, calls attachBaseContext + onCreate on it.
//
// This function replicates that native boot path. It's called from
// GuestHost.nativeBootGuest after Kotlin has staged the APK. On success
// returns the booted guest Application; on failure returns null and the
// reason is logged via java.lang.Throwable.printStackTrace() to
// DualCore.Native.

#include <jni.h>
#include <android/log.h>
#include <string>

#define TAG "DualCore.Native"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static std::string CaptureExceptionStack(JNIEnv* env) {
    jthrowable ex = env->ExceptionOccurred();
    if (!ex) return "<no pending exception>";
    env->ExceptionClear();

    std::string out = "<failed to format>";
    jclass swCls = env->FindClass("java/io/StringWriter");
    jclass pwCls = env->FindClass("java/io/PrintWriter");
    jclass thCls = env->FindClass("java/lang/Throwable");

    if (swCls && pwCls && thCls) {
        jmethodID swInit = env->GetMethodID(swCls, "<init>", "()V");
        jmethodID swToStr = env->GetMethodID(swCls, "toString", "()Ljava/lang/String;");
        jmethodID pwInit = env->GetMethodID(pwCls, "<init>", "(Ljava/io/Writer;)V");
        jmethodID pwFlush = env->GetMethodID(pwCls, "flush", "()V");
        jmethodID printM = env->GetMethodID(thCls, "printStackTrace",
                                            "(Ljava/io/PrintWriter;)V");
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
    return out;
}

static void LogException(JNIEnv* env, const char* where) {
    if (!env->ExceptionCheck()) return;
    std::string trace = CaptureExceptionStack(env);
    LOGE("nativeBootGuest: exception at %s:\n%s", where, trace.c_str());
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_GuestHost_nativeBootGuest(
        JNIEnv* env, jobject /*thiz*/, jstring jApkPath, jobject guestContext,
        jobject guestLoader) {

    if (!env || !jApkPath || !guestContext) return nullptr;

    const char* apkPath = env->GetStringUTFChars(jApkPath, nullptr);
    if (!apkPath) return nullptr;

    jobject result = nullptr;
    LOGD("nativeBootGuest: apk=%s", apkPath);

    do {
        // ── 1. PackageManager from ActivityThread.getSystemContext() ──
        jclass atCls = env->FindClass("android/app/ActivityThread");
        if (!atCls) { LogException(env, "FindClass ActivityThread"); break; }

        jmethodID curAT = env->GetStaticMethodID(atCls, "currentActivityThread",
                                                 "()Landroid/app/ActivityThread;");
        jobject at = curAT ? env->CallStaticObjectMethod(atCls, curAT) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); at = nullptr; }
        if (!at) { LOGD("no currentActivityThread"); env->DeleteLocalRef(atCls); break; }

        jmethodID getSysCtx = env->GetMethodID(atCls, "getSystemContext",
                                               "()Landroid/app/ContextImpl;");
        jobject sysCtx = getSysCtx ? env->CallObjectMethod(at, getSysCtx) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); sysCtx = nullptr; }
        env->DeleteLocalRef(at);
        env->DeleteLocalRef(atCls);
        if (!sysCtx) { LOGD("no systemContext"); break; }

        jclass ctxCls = env->FindClass("android/content/Context");
        jmethodID getPm = env->GetMethodID(ctxCls, "getPackageManager",
                                           "()Landroid/content/pm/PackageManager;");
        jobject pm = getPm ? env->CallObjectMethod(sysCtx, getPm) : nullptr;
        env->DeleteLocalRef(ctxCls);
        env->DeleteLocalRef(sysCtx);
        if (!pm) { LOGD("no PackageManager"); break; }

        // ── 2. getPackageArchiveInfo(apkPath, 0) ──
        jclass pmCls = env->FindClass("android/content/pm/PackageManager");
        jmethodID getArchiveInfo = env->GetMethodID(pmCls, "getPackageArchiveInfo",
                                                    "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
        env->DeleteLocalRef(pmCls);
        if (!getArchiveInfo) { LogException(env, "GetMethodID getPackageArchiveInfo");
            env->DeleteLocalRef(pm); break; }

        jstring jApk = env->NewStringUTF(apkPath);
        jobject guestPi = env->CallObjectMethod(pm, getArchiveInfo, jApk, (jint)0);
        env->DeleteLocalRef(jApk);
        if (env->ExceptionCheck()) { env->ExceptionClear(); guestPi = nullptr; }
        if (!guestPi) { LOGD("getPackageArchiveInfo returned null");
            env->DeleteLocalRef(pm); break; }

        // ── 3. Extract ApplicationInfo, className ──
        jclass piCls = env->FindClass("android/content/pm/PackageInfo");
        jfieldID piApp = env->GetFieldID(piCls, "applicationInfo",
                                         "Landroid/content/pm/ApplicationInfo;");
        jfieldID piPkg = env->GetFieldID(piCls, "packageName", "Ljava/lang/String;");
        jobject ai = env->GetObjectField(guestPi, piApp);
        jstring jPkg = (jstring)env->GetObjectField(guestPi, piPkg);
        env->DeleteLocalRef(piCls);
        if (!ai) { LOGD("no ApplicationInfo");
            if (jPkg) env->DeleteLocalRef(jPkg);
            env->DeleteLocalRef(guestPi);
            env->DeleteLocalRef(pm); break; }

        jclass aiCls = env->FindClass("android/content/pm/ApplicationInfo");
        jfieldID aiName = env->GetFieldID(aiCls, "className", "Ljava/lang/String;");
        jfieldID aiSrc  = env->GetFieldID(aiCls, "sourceDir", "Ljava/lang/String;");
        jfieldID aiPub  = env->GetFieldID(aiCls, "publicSourceDir", "Ljava/lang/String;");
        jstring jClassName = (jstring)env->GetObjectField(ai, aiName);
        env->DeleteLocalRef(aiCls);

        jstring jApk2 = env->NewStringUTF(apkPath);
        env->SetObjectField(ai, aiSrc, jApk2);
        env->SetObjectField(ai, aiPub, jApk2);
        env->DeleteLocalRef(jApk2);

        if (!jClassName) { LOGD("no applicationInfo.className");
            env->DeleteLocalRef(ai);
            if (jPkg) env->DeleteLocalRef(jPkg);
            env->DeleteLocalRef(guestPi);
            env->DeleteLocalRef(pm); break; }

        const char* className = env->GetStringUTFChars(jClassName, nullptr);
        LOGD("nativeBootGuest: guestApplication=%s", className ? className : "?");
        if (className) env->ReleaseStringUTFChars(jClassName, className);

        // ── 4. PathClassLoader(apkPath, parent=guestContext.getClassLoader()) ──
        jclass ctxCls2 = env->FindClass("android/content/Context");
        jmethodID getCl = env->GetMethodID(ctxCls2, "getClassLoader",
                                           "()Ljava/lang/ClassLoader;");
        jobject parentCl = env->CallObjectMethod(guestContext, getCl);
        env->DeleteLocalRef(ctxCls2);
        if (env->ExceptionCheck()) { env->ExceptionClear(); parentCl = nullptr; }
        if (!parentCl) { LOGD("guestContext.getClassLoader() null");
            env->DeleteLocalRef(jClassName);
            env->DeleteLocalRef(ai);
            if (jPkg) env->DeleteLocalRef(jPkg);
            env->DeleteLocalRef(guestPi);
            env->DeleteLocalRef(pm); break; }

        jclass pclCls = env->FindClass("dalvik/system/PathClassLoader");
        jmethodID pclCtor = env->GetMethodID(pclCls, "<init>",
                                             "(Ljava/lang/String;Ljava/lang/ClassLoader;)V");
        jstring jApk3 = env->NewStringUTF(apkPath);
        jobject guestLoader = env->NewObject(pclCls, pclCtor, jApk3, parentCl);
        env->DeleteLocalRef(jApk3);
        env->DeleteLocalRef(pclCls);
        if (env->ExceptionCheck()) { LogException(env, "PathClassLoader ctor");
            guestLoader = nullptr; }
        if (!guestLoader) { env->DeleteLocalRef(parentCl);
            env->DeleteLocalRef(jClassName);
            env->DeleteLocalRef(ai);
            if (jPkg) env->DeleteLocalRef(jPkg);
            env->DeleteLocalRef(guestPi);
            env->DeleteLocalRef(pm); break; }
        LOGD("nativeBootGuest: PathClassLoader constructed");

        // ── 5. Load guest Application class ──
        jclass clCls = env->FindClass("java/lang/ClassLoader");
        jmethodID loadClassM = env->GetMethodID(clCls, "loadClass",
                                                "(Ljava/lang/String;)Ljava/lang/Class;");
        env->DeleteLocalRef(clCls);
        jobject guestAppCls = env->CallObjectMethod(guestLoader, loadClassM, jClassName);
        if (env->ExceptionCheck()) { LogException(env, "loadClass"); guestAppCls = nullptr; }
        if (!guestAppCls) { LOGD("loadClass null");
            env->DeleteLocalRef(guestLoader);
            env->DeleteLocalRef(parentCl);
            env->DeleteLocalRef(jClassName);
            env->DeleteLocalRef(ai);
            if (jPkg) env->DeleteLocalRef(jPkg);
            env->DeleteLocalRef(guestPi);
            env->DeleteLocalRef(pm); break; }
        LOGD("nativeBootGuest: guest app class loaded");

        // ── 6. Instantiate via reflection ──
        jclass clsCls = env->FindClass("java/lang/Class");
        jmethodID getCtor = env->GetMethodID(clsCls, "getDeclaredConstructor",
                                             "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;");
        env->DeleteLocalRef(clsCls);
        jclass classParamCls = env->FindClass("java/lang/Class");
        jobjectArray emptyParams = env->NewObjectArray(0, classParamCls, nullptr);
        env->DeleteLocalRef(classParamCls);
        jobject ctor = env->CallObjectMethod(guestAppCls, getCtor, emptyParams);
        env->DeleteLocalRef(emptyParams);
        if (env->ExceptionCheck()) { LogException(env, "getDeclaredConstructor");
            ctor = nullptr; }

        jobject guestApp = nullptr;
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
                if (env->ExceptionCheck()) { LogException(env, "Constructor.newInstance");
                    guestApp = nullptr; }
            }
            env->DeleteLocalRef(ctor);
        }

        // ── 7 & 8. attachBaseContext + onCreate ──
        if (guestApp) {
            LOGD("nativeBootGuest: guest Application instantiated");

            jclass appCls = env->FindClass("android/app/Application");
            jmethodID attachM = env->GetMethodID(appCls, "attachBaseContext",
                                                 "(Landroid/content/Context;)V");
            env->DeleteLocalRef(appCls);

            if (attachM) {
                env->CallVoidMethod(guestApp, attachM, guestContext);
                if (env->ExceptionCheck()) {
                    LogException(env, "attachBaseContext");
                    env->DeleteLocalRef(guestApp);
                    guestApp = nullptr;
                }
            } else {
                LOGD("nativeBootGuest: attachBaseContext method not found");
                env->DeleteLocalRef(guestApp);
                guestApp = nullptr;
            }
        }

        if (guestApp) {
            jclass appCls2 = env->FindClass("android/app/Application");
            jmethodID onCreateM = env->GetMethodID(appCls2, "onCreate", "()V");
            env->DeleteLocalRef(appCls2);
            if (onCreateM) {
                env->CallVoidMethod(guestApp, onCreateM);
                if (env->ExceptionCheck()) {
                    LogException(env, "onCreate");
                    env->DeleteLocalRef(guestApp);
                    guestApp = nullptr;
                }
            }
        }

        if (guestApp) {
            LOGD("nativeBootGuest: guest boot complete");
            result = guestApp;   // ownership to caller (Kotlin)
        }

        // Cleanup
        env->DeleteLocalRef(guestAppCls);
        env->DeleteLocalRef(jClassName);
        env->DeleteLocalRef(ai);
        if (jPkg) env->DeleteLocalRef(jPkg);
        env->DeleteLocalRef(guestPi);
        env->DeleteLocalRef(pm);
    } while (0);

    env->ReleaseStringUTFChars(jApkPath, apkPath);
    return result;
}