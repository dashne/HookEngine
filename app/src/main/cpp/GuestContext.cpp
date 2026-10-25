// GuestContext.cpp
//
// Session 19 — native guest context initialization.
//
// Rule 15: engine behavior in native. Rule 2: pull-driven — this
// implementation exists because the Kotlin GuestContext wall was
// logged (Session 19b), not because decomp predicted it.
//
// Flow:
//   1. Get guest ApplicationInfo; fix sourceDir to point at staged APK.
//   2. Call ActivityThread.getPackageInfoNoCheck(ai, ci) → LoadedApk.
//   3. Call LoadedApk.makeApplication(false, instrumentation) → Application.
//      makeApplication internally calls Application.attach(context) →
//      attachBaseContext with REAL guest Resources.
//   4. Kotlin calls app.onCreate() separately.
//
// Session 19c: HiddenApiBridge is a Kotlin `object`; its external funs
// are INSTANCE methods (GetMethodID on INSTANCE, not GetStaticMethodID).
//
// Session 19d: makeApplication(false, null) failed with an exception
// that was swallowed by ExceptionDescribe (writes to System.err, not
// our tag). Fix: capture the full Java stack trace as a string, retry
// with explicit ActivityThread.mInstrumentation, and use forceDefault=
// true as a probe variant to distinguish LoadedApk+instr broken vs
// Viber's Application class broken.

#include <jni.h>
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
#include <initializer_list>

#define TAG "DualCore.Native"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static JavaVM* g_vm = nullptr;
static std::mutex g_state_mutex;
static jobject g_loaded_apk = nullptr;
static std::string g_last_diag;

static void AppendDiag(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> lk(g_state_mutex);
    g_last_diag.append(buf);
    g_last_diag.push_back('\n');
    LOGD("%s", buf);
}

// ─── Capture pending exception as a full Java stack trace string ────
static std::string CaptureExceptionStackTrace(JNIEnv* env) {
    jthrowable ex = env->ExceptionOccurred();
    if (!ex) return "<no pending exception>";
    env->ExceptionClear();

    std::string out = "<failed to format stack>";
    jclass swCls = env->FindClass("java/io/StringWriter");
    jclass pwCls = env->FindClass("java/io/PrintWriter");
    jclass thCls = env->FindClass("java/lang/Throwable");

    if (swCls && pwCls && thCls) {
        jmethodID swInit  = env->GetMethodID(swCls, "<init>", "()V");
        jmethodID swToStr = env->GetMethodID(swCls, "toString", "()Ljava/lang/String;");
        jmethodID pwInit  = env->GetMethodID(pwCls, "<init>", "(Ljava/io/Writer;)V");
        jmethodID pwFlush = env->GetMethodID(pwCls, "flush", "()V");
        jmethodID printM  = env->GetMethodID(thCls, "printStackTrace",
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
        if (env->ExceptionCheck()) env->ExceptionClear();
    }

    if (swCls) env->DeleteLocalRef(swCls);
    if (pwCls) env->DeleteLocalRef(pwCls);
    if (thCls) env->DeleteLocalRef(thCls);
    env->DeleteLocalRef(ex);
    return out;
}

// ─── Fetch HiddenApiBridge.INSTANCE (Kotlin object singleton) ────────
static jobject GetHiddenApiBridgeInstance(JNIEnv* env, jclass* outCls) {
    *outCls = nullptr;
    jclass habCls = env->FindClass("com/ims134/hookengine/HiddenApiBridge");
    if (!habCls) { env->ExceptionClear(); AppendDiag("no HiddenApiBridge"); return nullptr; }

    jfieldID fInstance = env->GetStaticFieldID(habCls, "INSTANCE",
                                               "Lcom/ims134/hookengine/HiddenApiBridge;");
    if (!fInstance) {
        env->ExceptionClear(); AppendDiag("no HiddenApiBridge.INSTANCE field");
        env->DeleteLocalRef(habCls); return nullptr;
    }
    jobject habInst = env->GetStaticObjectField(habCls, fInstance);
    if (!habInst) {
        env->ExceptionClear(); AppendDiag("INSTANCE field null");
        env->DeleteLocalRef(habCls); return nullptr;
    }
    *outCls = habCls;
    return habInst;
}

// ─── Reflective lookup (fixed for Kotlin object this iteration) ──────
static jobject ReflectGetDeclaredMethod(
        JNIEnv* env, jclass targetCls,
        const char* name, std::initializer_list<jclass> paramTypes) {

    jclass habCls = nullptr;
    jobject habInst = GetHiddenApiBridgeInstance(env, &habCls);
    if (!habInst) return nullptr;

    jmethodID mid = env->GetMethodID(habCls, "reflectGetDeclaredMethod",
                                     "(Ljava/lang/Class;Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
    if (!mid) {
        env->ExceptionClear(); AppendDiag("no reflectGetDeclaredMethod method");
        env->DeleteLocalRef(habInst); env->DeleteLocalRef(habCls);
        return nullptr;
    }

    jclass classCls = env->FindClass("java/lang/Class");
    jobjectArray params = env->NewObjectArray((jsize)paramTypes.size(), classCls, nullptr);
    jsize i = 0;
    for (jclass c : paramTypes) env->SetObjectArrayElement(params, i++, c);
    env->DeleteLocalRef(classCls);

    jstring jname = env->NewStringUTF(name);
    jobject m = env->CallObjectMethod(habInst, mid, targetCls, jname, params);
    if (env->ExceptionCheck()) { env->ExceptionClear(); m = nullptr; }

    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(params);
    env->DeleteLocalRef(habInst);
    env->DeleteLocalRef(habCls);
    return m;
}

static jobject ReadStaticObjectField(
        JNIEnv* env, jclass targetCls, const char* fieldName) {

    jclass habCls = nullptr;
    jobject habInst = GetHiddenApiBridgeInstance(env, &habCls);
    if (!habInst) return nullptr;

    jmethodID mid = env->GetMethodID(habCls, "reflectGetDeclaredField",
                                     "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Field;");
    if (!mid) {
        env->ExceptionClear(); AppendDiag("no reflectGetDeclaredField method");
        env->DeleteLocalRef(habInst); env->DeleteLocalRef(habCls);
        return nullptr;
    }

    jstring jname = env->NewStringUTF(fieldName);
    jobject f = env->CallObjectMethod(habInst, mid, targetCls, jname);
    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(habInst);
    env->DeleteLocalRef(habCls);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (!f) return nullptr;

    jclass fieldCls = env->FindClass("java/lang/reflect/Field");
    jmethodID getId = env->GetMethodID(fieldCls, "get",
                                       "(Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(fieldCls);
    jobject val = getId ? env->CallObjectMethod(f, getId, nullptr) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); val = nullptr; }
    env->DeleteLocalRef(f);
    return val;
}

// ─── JNI: nativeInitGuest (unchanged from 19c) ───────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_GuestHost_nativeInitGuest(
        JNIEnv* env, jobject /*thiz*/, jstring jGuestPkg, jstring jApkPath) {

    if (!env || !jGuestPkg || !jApkPath) return -1;
    if (!g_vm) env->GetJavaVM(&g_vm);

    { std::lock_guard<std::mutex> lk(g_state_mutex); g_last_diag.clear(); }

    const char* guestPkg = env->GetStringUTFChars(jGuestPkg, nullptr);
    const char* apkPath  = env->GetStringUTFChars(jApkPath, nullptr);
    if (!guestPkg || !apkPath) {
        if (guestPkg) env->ReleaseStringUTFChars(jGuestPkg, guestPkg);
        if (apkPath)  env->ReleaseStringUTFChars(jApkPath, apkPath);
        return -2;
    }

    AppendDiag("init: pkg=%s apk=%s", guestPkg, apkPath);

    jint rc = [&]() -> jint {
        jclass atCls = env->FindClass("android/app/ActivityThread");
        if (!atCls) { env->ExceptionClear(); AppendDiag("no ActivityThread"); return -3; }

        jobject currentAT = nullptr;
        {
            jmethodID cur = env->GetStaticMethodID(atCls, "currentActivityThread",
                                                   "()Landroid/app/ActivityThread;");
            if (cur) currentAT = env->CallStaticObjectMethod(atCls, cur);
            if (env->ExceptionCheck()) { env->ExceptionClear(); currentAT = nullptr; }
        }
        if (!currentAT) { AppendDiag("no currentActivityThread"); env->DeleteLocalRef(atCls); return -3; }

        jobject sysCtx = nullptr;
        {
            jmethodID getSysCtx = env->GetMethodID(atCls, "getSystemContext",
                                                   "()Landroid/app/ContextImpl;");
            if (getSysCtx) sysCtx = env->CallObjectMethod(currentAT, getSysCtx);
            if (env->ExceptionCheck()) { env->ExceptionClear(); sysCtx = nullptr; }
        }
        if (!sysCtx) { AppendDiag("no systemContext"); env->DeleteLocalRef(atCls); return -3; }

        jobject pm = nullptr;
        {
            jclass ctxCls = env->FindClass("android/content/Context");
            jmethodID getPm = env->GetMethodID(ctxCls, "getPackageManager",
                                               "()Landroid/content/pm/PackageManager;");
            env->DeleteLocalRef(ctxCls);
            if (getPm) pm = env->CallObjectMethod(sysCtx, getPm);
            if (env->ExceptionCheck()) { env->ExceptionClear(); pm = nullptr; }
        }
        if (!pm) { AppendDiag("no PackageManager"); env->DeleteLocalRef(atCls); return -3; }

        jobject guestPi = nullptr;
        {
            jclass pmCls = env->FindClass("android/content/pm/PackageManager");
            jmethodID getPi = env->GetMethodID(pmCls, "getPackageInfo",
                                               "(Ljava/lang/String;I)Landroid/content/pm/PackageInfo;");
            env->DeleteLocalRef(pmCls);
            if (!getPi) { env->ExceptionClear(); AppendDiag("no getPackageInfo"); env->DeleteLocalRef(atCls); return -3; }
            jstring jpkg = env->NewStringUTF(guestPkg);
            guestPi = env->CallObjectMethod(pm, getPi, jpkg, (jint)0);
            env->DeleteLocalRef(jpkg);
            if (env->ExceptionCheck()) {
                std::string ex = CaptureExceptionStackTrace(env);
                AppendDiag("getPackageInfo threw: %s", ex.c_str());
                guestPi = nullptr;
            }
        }
        if (!guestPi) { AppendDiag("getPackageInfo null"); env->DeleteLocalRef(atCls); return -3; }

        jobject ai = nullptr;
        {
            jclass piCls = env->FindClass("android/content/pm/PackageInfo");
            jfieldID fApp = env->GetFieldID(piCls, "applicationInfo",
                                            "Landroid/content/pm/ApplicationInfo;");
            env->DeleteLocalRef(piCls);
            if (fApp) ai = env->GetObjectField(guestPi, fApp);
            if (env->ExceptionCheck()) { env->ExceptionClear(); ai = nullptr; }
        }
        if (!ai) { AppendDiag("no applicationInfo"); env->DeleteLocalRef(atCls); return -3; }

        {
            jclass aiCls = env->FindClass("android/content/pm/ApplicationInfo");
            jfieldID fSrc = env->GetFieldID(aiCls, "sourceDir", "Ljava/lang/String;");
            jfieldID fPub = env->GetFieldID(aiCls, "publicSourceDir", "Ljava/lang/String;");
            env->DeleteLocalRef(aiCls);
            jstring japk = env->NewStringUTF(apkPath);
            if (fSrc) env->SetObjectField(ai, fSrc, japk);
            if (fPub) env->SetObjectField(ai, fPub, japk);
            env->DeleteLocalRef(japk);
            if (env->ExceptionCheck()) {
                env->ExceptionClear(); AppendDiag("set sourceDir failed");
                env->DeleteLocalRef(atCls); return -3;
            }
        }
        AppendDiag("ai ready, sourceDir fixed");

        jobject compatInfo = nullptr;
        {
            jclass ciCls = env->FindClass("android/content/res/CompatibilityInfo");
            if (ciCls) compatInfo = ReadStaticObjectField(env, ciCls, "DEFAULT_COMPATIBILITY_INFO");
            env->DeleteLocalRef(ciCls);
        }
        if (!compatInfo) {
            env->ExceptionClear();
            AppendDiag("no DEFAULT_COMPATIBILITY_INFO — trying null");
        } else {
            AppendDiag("DEFAULT_COMPATIBILITY_INFO obtained");
        }

        jobject loadedApk = nullptr;
        {
            jclass aiCls = env->FindClass("android/content/pm/ApplicationInfo");
            jclass ciCls = env->FindClass("android/content/res/CompatibilityInfo");
            jobject m = ReflectGetDeclaredMethod(env, atCls, "getPackageInfoNoCheck",
                                                 { aiCls, ciCls });
            env->DeleteLocalRef(aiCls);
            env->DeleteLocalRef(ciCls);
            if (m) {
                jmethodID mid = env->FromReflectedMethod(m);
                if (mid) {
                    loadedApk = env->CallObjectMethod(currentAT, mid, ai, compatInfo);
                    if (env->ExceptionCheck()) {
                        std::string ex = CaptureExceptionStackTrace(env);
                        AppendDiag("getPackageInfoNoCheck threw: %s", ex.c_str());
                        loadedApk = nullptr;
                    }
                } else {
                    AppendDiag("FromReflectedMethod failed");
                }
                env->DeleteLocalRef(m);
            } else {
                AppendDiag("no getPackageInfoNoCheck method");
            }
        }
        env->DeleteLocalRef(atCls);

        if (!loadedApk) { AppendDiag("loadedApk null"); return -3; }

        {
            std::lock_guard<std::mutex> lk(g_state_mutex);
            if (g_loaded_apk) env->DeleteGlobalRef(g_loaded_apk);
            g_loaded_apk = env->NewGlobalRef(loadedApk);
        }
        AppendDiag("loadedApk ready");
        return 0;
    }();

    env->ReleaseStringUTFChars(jGuestPkg, guestPkg);
    env->ReleaseStringUTFChars(jApkPath, apkPath);
    return rc;
}

// ─── JNI: nativeMakeApplication — multi-variant with stack capture ───
extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_GuestHost_nativeMakeApplication(
        JNIEnv* env, jobject /*thiz*/) {

    jobject loadedApk;
    { std::lock_guard<std::mutex> lk(g_state_mutex); loadedApk = g_loaded_apk; }
    if (!loadedApk) { AppendDiag("no loadedApk — call nativeInitGuest first"); return nullptr; }

    jclass laCls = env->GetObjectClass(loadedApk);
    if (!laCls) { env->ExceptionClear(); return nullptr; }

    // Fetch ActivityThread.mInstrumentation (the real one set by JUnit runner).
    jobject instrumentation = nullptr;
    {
        jclass atCls = env->FindClass("android/app/ActivityThread");
        if (atCls) {
            jmethodID curM = env->GetStaticMethodID(atCls, "currentActivityThread",
                                                    "()Landroid/app/ActivityThread;");
            jobject at = curM ? env->CallStaticObjectMethod(atCls, curM) : nullptr;
            if (env->ExceptionCheck()) { env->ExceptionClear(); at = nullptr; }
            if (at) {
                jfieldID fInstr = env->GetFieldID(atCls, "mInstrumentation",
                                                  "Landroid/app/Instrumentation;");
                if (fInstr) instrumentation = env->GetObjectField(at, fInstr);
                if (env->ExceptionCheck()) { env->ExceptionClear(); instrumentation = nullptr; }
                env->DeleteLocalRef(at);
            }
            env->DeleteLocalRef(atCls);
        }
    }
    AppendDiag("instrumentation=%s", instrumentation ? "set" : "null");

    // Types needed for both candidates
    jclass boolCls = env->FindClass("java/lang/Boolean");
    jfieldID fBoolType = boolCls ? env->GetStaticFieldID(boolCls, "TYPE", "Ljava/lang/Class;") : nullptr;
    jobject boolType = fBoolType ? env->GetStaticObjectField(boolCls, fBoolType) : nullptr;
    jclass instrCls = env->FindClass("android/app/Instrumentation");
    jclass methodCls = env->FindClass("java/lang/reflect/Method");
    jmethodID invokeM = methodCls ? env->GetMethodID(methodCls, "invoke",
                                                     "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;") : nullptr;
    jclass objCls = env->FindClass("java/lang/Object");
    jmethodID bVal = boolCls ? env->GetStaticMethodID(boolCls, "valueOf",
                                                      "(Z)Ljava/lang/Boolean;") : nullptr;

    struct Variant { const char* method; bool forceDefault; bool passInstr; };
    static const Variant kVariants[] = {
            { "makeApplication",      false, true  },   // preferred
            { "makeApplication",      false, false },   // baseline (already failed, log ex)
            { "makeApplication",      true,  true  },   // probe: force base Application class
            { "makeApplicationInner", false, true  },
            { "makeApplicationInner", false, false },
            { nullptr,                false, false },
    };

    jobject app = nullptr;

    for (int i = 0; kVariants[i].method && !app; i++) {
        const Variant& v = kVariants[i];

        // reflect: find method(boolean, Instrumentation) on LoadedApk
        jobject m = nullptr;
        if (boolType && instrCls && methodCls) {
            jclass classCls = env->FindClass("java/lang/Class");
            jobjectArray params = env->NewObjectArray(2, classCls, nullptr);
            env->SetObjectArrayElement(params, 0, boolType);
            env->SetObjectArrayElement(params, 1, instrCls);
            env->DeleteLocalRef(classCls);

            jclass habCls = nullptr;
            jobject habInst = GetHiddenApiBridgeInstance(env, &habCls);
            if (habInst) {
                jmethodID refM = env->GetMethodID(habCls, "reflectGetDeclaredMethod",
                                                  "(Ljava/lang/Class;Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
                if (refM) {
                    jstring jn = env->NewStringUTF(v.method);
                    m = env->CallObjectMethod(habInst, refM, laCls, jn, params);
                    if (env->ExceptionCheck()) { env->ExceptionClear(); m = nullptr; }
                    env->DeleteLocalRef(jn);
                }
                env->DeleteLocalRef(habInst);
                env->DeleteLocalRef(habCls);
            }
            env->DeleteLocalRef(params);
        }

        if (!m) {
            AppendDiag("%s(force=%d,instr=%d): method not found",
                       v.method, v.forceDefault ? 1 : 0, v.passInstr ? 1 : 0);
            continue;
        }

        // invoke(loadedApk, [Boolean(forceDefault), instr_or_null])
        jobjectArray args = env->NewObjectArray(2, objCls, nullptr);
        jobject jForce = env->CallStaticObjectMethod(boolCls, bVal,
                                                     (jboolean)(v.forceDefault ? JNI_TRUE : JNI_FALSE));
        env->SetObjectArrayElement(args, 0, jForce);
        jobject jInstr = (v.passInstr && instrumentation) ? instrumentation : nullptr;
        env->SetObjectArrayElement(args, 1, jInstr);
        env->DeleteLocalRef(jForce);

        jobject res = env->CallObjectMethod(m, invokeM, loadedApk, args);
        if (env->ExceptionCheck()) {
            std::string ex = CaptureExceptionStackTrace(env);
            AppendDiag("%s(force=%d,instr=%d) threw:\n%s",
                       v.method, v.forceDefault ? 1 : 0, v.passInstr ? 1 : 0, ex.c_str());
            res = nullptr;
        } else if (!res) {
            AppendDiag("%s(force=%d,instr=%d) returned null (no exception)",
                       v.method, v.forceDefault ? 1 : 0, v.passInstr ? 1 : 0);
        } else {
            AppendDiag("%s(force=%d,instr=%d) OK",
                       v.method, v.forceDefault ? 1 : 0, v.passInstr ? 1 : 0);
        }

        env->DeleteLocalRef(args);
        env->DeleteLocalRef(m);

        if (res) app = res;
    }

    if (instrumentation) env->DeleteLocalRef(instrumentation);
    if (boolType) env->DeleteLocalRef(boolType);
    if (boolCls) env->DeleteLocalRef(boolCls);
    if (instrCls) env->DeleteLocalRef(instrCls);
    if (methodCls) env->DeleteLocalRef(methodCls);
    if (objCls) env->DeleteLocalRef(objCls);
    env->DeleteLocalRef(laCls);
    return app;
}

// ─── JNI: nativeGetDiag ──────────────────────────────────────────────
extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_GuestHost_nativeGetDiag(
        JNIEnv* env, jobject /*thiz*/) {
    std::lock_guard<std::mutex> lk(g_state_mutex);
    return env->NewStringUTF(g_last_diag.c_str());
}