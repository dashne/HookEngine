// ApplicationProxy.cpp
// Session 22 — BlackBox-fidelity native boot.
// Session 32 — GetMyUid uses syscall(SYS_getuid).
// Session 38 — nativeInit(structural port): 3-param signature matching
//              reference decomp; extracts .so + classes*.dex from staged
//              APK to libPath. Uses JNI java.util.zip.ZipFile for the ZIP
//              walk. Returns 0 on success, -1 on failure.
//              Rule 5 partial — signature faithful, body inferred from
//              reference architecture (Session 19+20 decomp of JNI_OnLoad
//              + ApplicationProxy flow); sub_2A998 body not decomp'd.

#include <jni.h>
#include <android/log.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <mutex>
#include <initializer_list>

#define TAG "DualCore.Native"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static std::mutex g_guest_mutex;
static jobject    g_guest_app     = nullptr;
static JavaVM*    g_vm            = nullptr;
static bool       g_guest_created = false;

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

// ─── Session 38 — nativeInit (structural port) ───────────────────────
// Signature (decomp-verified Session 20): nativeInit(String, String, int) -> int
// Body (inferred): walk APK entries; extract lib/arm64-v8a/*.so and
// classes*.dex into libPath. Return 0 on success, -1 on any failure.
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeInit(
        JNIEnv* env, jobject /*thiz*/,
        jstring jApkPath, jstring jLibPath, jint flags) {
    if (!env || !jApkPath || !jLibPath) return -1;

    const char* apkPath = env->GetStringUTFChars(jApkPath, nullptr);
    const char* libPath = env->GetStringUTFChars(jLibPath, nullptr);
    if (!apkPath || !libPath) {
        if (apkPath) env->ReleaseStringUTFChars(jApkPath, apkPath);
        if (libPath) env->ReleaseStringUTFChars(jLibPath, libPath);
        return -1;
    }

    LOGD("nativeInit: apk=%s lib=%s flags=%d", apkPath, libPath, (int)flags);

    int extracted_so  = 0;
    int extracted_dex = 0;
    int result        = 0;

    struct stat st{};
    if (stat(apkPath, &st) != 0 || st.st_size == 0) {
        LOGE("nativeInit: apk missing or empty: %s", apkPath);
        result = -1;
        goto done;
    }

    // Ensure libPath directory exists.
    mkdir(libPath, 0755);

    do {
        jclass zipCls = env->FindClass("java/util/zip/ZipFile");
        if (!zipCls) { env->ExceptionClear(); result = -1; break; }
        jmethodID zipCtor = env->GetMethodID(zipCls, "<init>", "(Ljava/lang/String;)V");
        if (!zipCtor) { env->ExceptionClear(); result = -1; break; }

        jobject zipFile = env->NewObject(zipCls, zipCtor, jApkPath);
        if (!zipFile) { env->ExceptionClear(); result = -1; break; }

        jmethodID entriesM   = env->GetMethodID(zipCls, "entries", "()Ljava/util/Enumeration;");
        jmethodID closeM     = env->GetMethodID(zipCls, "close", "()V");
        jmethodID getIsM     = env->GetMethodID(zipCls, "getInputStream",
                                                "(Ljava/util/zip/ZipEntry;)Ljava/io/InputStream;");

        jclass enumCls       = env->FindClass("java/util/Enumeration");
        jmethodID hasMoreM   = env->GetMethodID(enumCls, "hasMoreElements", "()Z");
        jmethodID nextM      = env->GetMethodID(enumCls, "nextElement", "()Ljava/lang/Object;");

        jclass entryCls      = env->FindClass("java/util/zip/ZipEntry");
        jmethodID getNameM   = env->GetMethodID(entryCls, "getName", "()Ljava/lang/String;");
        jmethodID getSizeM   = env->GetMethodID(entryCls, "getSize", "()J");

        jclass isCls         = env->FindClass("java/io/InputStream");
        jmethodID readM      = env->GetMethodID(isCls, "read", "([B)I");
        jmethodID closeIsM   = env->GetMethodID(isCls, "close", "()V");

        jobject enumeration = env->CallObjectMethod(zipFile, entriesM);
        if (!enumeration) { env->ExceptionClear(); result = -1;
            if (closeM) env->CallVoidMethod(zipFile, closeM);
            env->DeleteLocalRef(zipFile); break; }

        while (env->CallBooleanMethod(enumeration, hasMoreM)) {
            jobject entry = env->CallObjectMethod(enumeration, nextM);
            if (!entry) break;

            jstring nameJ = (jstring)env->CallObjectMethod(entry, getNameM);
            if (!nameJ) { env->DeleteLocalRef(entry); break; }
            const char* name = env->GetStringUTFChars(nameJ, nullptr);
            if (!name) { env->DeleteLocalRef(nameJ); env->DeleteLocalRef(entry); break; }
            jlong entrySize = env->CallLongMethod(entry, getSizeM);

            size_t nlen = strlen(name);
            bool isSo  = (strncmp(name, "lib/arm64-v8a/", 14) == 0 && nlen > 3 &&
                          strcmp(name + nlen - 3, ".so") == 0);
            bool isDex = (strncmp(name, "classes", 7) == 0 && nlen > 4 &&
                          strcmp(name + nlen - 4, ".dex") == 0);

            if (isSo || isDex) {
                const char* base = strrchr(name, '/');
                base = base ? base + 1 : name;
                std::string outPath = std::string(libPath) + "/" + base;

                struct stat ost{};
                bool already = (stat(outPath.c_str(), &ost) == 0 &&
                                ost.st_size == (off_t)entrySize);

                if (!already) {
                    jobject is = env->CallObjectMethod(zipFile, getIsM, entry);
                    if (is) {
                        FILE* out = fopen(outPath.c_str(), "wb");
                        if (out) {
                            jbyteArray buf = env->NewByteArray(65536);
                            while (true) {
                                jint n = env->CallIntMethod(is, readM, buf);
                                if (n <= 0) break;
                                jbyte* b = env->GetByteArrayElements(buf, nullptr);
                                if (b) {
                                    fwrite(b, 1, (size_t)n, out);
                                    env->ReleaseByteArrayElements(buf, b, JNI_ABORT);
                                }
                            }
                            env->DeleteLocalRef(buf);
                            fclose(out);
                        }
                        env->CallVoidMethod(is, closeIsM);
                        env->DeleteLocalRef(is);
                    }
                    if (env->ExceptionCheck()) { env->ExceptionClear(); }
                }

                if (isSo)  extracted_so++;
                if (isDex) extracted_dex++;
            }

            env->ReleaseStringUTFChars(nameJ, name);
            env->DeleteLocalRef(nameJ);
            env->DeleteLocalRef(entry);
        }

        env->DeleteLocalRef(enumeration);
        if (closeM) env->CallVoidMethod(zipFile, closeM);
        env->DeleteLocalRef(zipFile);

        if (extracted_so == 0 || extracted_dex == 0) {
            LOGE("nativeInit: incomplete extraction so=%d dex=%d",
                 extracted_so, extracted_dex);
            result = -1;
        }
    } while (0);

    done:
    env->ReleaseStringUTFChars(jApkPath, apkPath);
    env->ReleaseStringUTFChars(jLibPath, libPath);
    LOGD("nativeInit: extracted so=%d dex=%d result=%d",
         extracted_so, extracted_dex, result);
    return result;
}

// ─── (unchanged from Session 32) ─────────────────────────────────────

static jobject GetHiddenApiBridgeInstance(JNIEnv* env, jclass* outCls) {
    *outCls = nullptr;
    jclass habCls = env->FindClass("com/ims134/hookengine/HiddenApiBridge");
    if (!habCls) { env->ExceptionClear(); return nullptr; }
    jfieldID fInstance = env->GetStaticFieldID(habCls, "INSTANCE",
                                               "Lcom/ims134/hookengine/HiddenApiBridge;");
    if (!fInstance) {
        env->ExceptionClear(); env->DeleteLocalRef(habCls); return nullptr;
    }
    jobject inst = env->GetStaticObjectField(habCls, fInstance);
    if (!inst) { env->ExceptionClear(); env->DeleteLocalRef(habCls); return nullptr; }
    *outCls = habCls;
    return inst;
}

static jobject ReflectGetDeclaredMethod(
        JNIEnv* env, jclass targetCls,
        const char* name, std::initializer_list<jclass> paramTypes) {
    jclass habCls = nullptr;
    jobject habInst = GetHiddenApiBridgeInstance(env, &habCls);
    if (!habInst) return nullptr;
    jmethodID mid = env->GetMethodID(habCls, "reflectGetDeclaredMethod",
                                     "(Ljava/lang/Class;Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
    if (!mid) {
        env->ExceptionClear(); env->DeleteLocalRef(habInst); env->DeleteLocalRef(habCls);
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
        env->ExceptionClear(); env->DeleteLocalRef(habInst); env->DeleteLocalRef(habCls);
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
    jmethodID getM = env->GetMethodID(fieldCls, "get",
                                      "(Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(fieldCls);
    jobject val = getM ? env->CallObjectMethod(f, getM, nullptr) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); val = nullptr; }
    env->DeleteLocalRef(f);
    return val;
}

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

static jstring ReadGuestDataDir(JNIEnv* env, jobject proxy) {
    jclass pc = env->GetObjectClass(proxy);
    jfieldID f = env->GetFieldID(pc, "guestDataDir", "Ljava/lang/String;");
    env->DeleteLocalRef(pc);
    if (!f) { env->ExceptionClear(); return nullptr; }
    return (jstring)env->GetObjectField(proxy, f);
}

static bool SetProcessArgV0(JNIEnv* env, const char* name) {
    jclass procCls = env->FindClass("android/os/Process");
    if (!procCls) { env->ExceptionClear(); return false; }
    jmethodID m = env->GetStaticMethodID(procCls, "setArgV0", "(Ljava/lang/String;)V");
    if (m) {
        jstring jName = env->NewStringUTF(name);
        env->CallStaticVoidMethod(procCls, m, jName);
        env->DeleteLocalRef(jName);
        env->DeleteLocalRef(procCls);
        if (env->ExceptionCheck()) { LogAndClearException(env, "setArgV0 direct"); return false; }
        LOGD("setArgV0(%s) OK (direct)", name);
        return true;
    }
    env->ExceptionClear();
    jclass strCls = env->FindClass("java/lang/String");
    jobject reflected = ReflectGetDeclaredMethod(env, procCls, "setArgV0", {strCls});
    env->DeleteLocalRef(strCls);
    env->DeleteLocalRef(procCls);
    if (!reflected) { LOGE("setArgV0: not found"); return false; }
    jclass methodCls = env->FindClass("java/lang/reflect/Method");
    jmethodID invokeM = env->GetMethodID(methodCls, "invoke",
                                         "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(methodCls);
    jclass objCls = env->FindClass("java/lang/Object");
    jobjectArray args = env->NewObjectArray(1, objCls, nullptr);
    env->DeleteLocalRef(objCls);
    jstring jName = env->NewStringUTF(name);
    env->SetObjectArrayElement(args, 0, jName);
    env->CallObjectMethod(reflected, invokeM, nullptr, args);
    bool ok = !env->ExceptionCheck();
    if (!ok) LogAndClearException(env, "setArgV0 reflection");
    else LOGD("setArgV0(%s) OK (reflection)", name);
    env->DeleteLocalRef(jName);
    env->DeleteLocalRef(args);
    env->DeleteLocalRef(reflected);
    return ok;
}

static void PatchActivityThreadState(JNIEnv* env, jobject guestApp,
                                     jobject ai, const char* guestPkg) {
    jclass atCls = env->FindClass("android/app/ActivityThread");
    if (!atCls) { env->ExceptionClear(); return; }
    jmethodID curM = env->GetStaticMethodID(atCls, "currentActivityThread",
                                            "()Landroid/app/ActivityThread;");
    jobject at = curM ? env->CallStaticObjectMethod(atCls, curM) : nullptr;
    if (env->ExceptionCheck() || !at) {
        env->ExceptionClear(); env->DeleteLocalRef(atCls);
        LOGE("PatchAT: no currentActivityThread"); return;
    }
    jfieldID fBoundApp = env->GetFieldID(atCls, "mBoundApplication",
                                         "Landroid/app/ActivityThread$AppBindData;");
    if (fBoundApp) {
        jobject boundApp = env->GetObjectField(at, fBoundApp);
        if (boundApp) {
            jclass baCls = env->GetObjectClass(boundApp);
            jfieldID fProcessName = env->GetFieldID(baCls, "processName", "Ljava/lang/String;");
            if (fProcessName) {
                jstring jPkg = env->NewStringUTF(guestPkg);
                env->SetObjectField(boundApp, fProcessName, jPkg);
                env->DeleteLocalRef(jPkg);
                if (!env->ExceptionCheck())
                    LOGD("PatchAT: mBoundApplication.processName = %s", guestPkg);
                else env->ExceptionClear();
            } else env->ExceptionClear();
            jfieldID fAppInfo = env->GetFieldID(baCls, "appInfo",
                                                "Landroid/content/pm/ApplicationInfo;");
            if (fAppInfo) {
                env->SetObjectField(boundApp, fAppInfo, ai);
                if (!env->ExceptionCheck())
                    LOGD("PatchAT: mBoundApplication.appInfo patched");
                else env->ExceptionClear();
            } else env->ExceptionClear();
            env->DeleteLocalRef(baCls);
            env->DeleteLocalRef(boundApp);
        }
    } else env->ExceptionClear();
    jfieldID fInitApp = env->GetFieldID(atCls, "mInitialApplication",
                                        "Landroid/app/Application;");
    if (fInitApp) {
        env->SetObjectField(at, fInitApp, guestApp);
        if (!env->ExceptionCheck())
            LOGD("PatchAT: mInitialApplication patched");
        else env->ExceptionClear();
    } else env->ExceptionClear();
    env->DeleteLocalRef(at);
    env->DeleteLocalRef(atCls);
}

static bool FixApplicationInfo(
        JNIEnv* env, jobject ai,
        const char* apkPath, const char* libDir,
        const char* dataDir, jint uid) {
    jclass aiCls = env->FindClass("android/content/pm/ApplicationInfo");
    if (!aiCls) { env->ExceptionClear(); return false; }
    jfieldID fSrc   = env->GetFieldID(aiCls, "sourceDir",        "Ljava/lang/String;");
    jfieldID fPub   = env->GetFieldID(aiCls, "publicSourceDir",  "Ljava/lang/String;");
    jfieldID fData  = env->GetFieldID(aiCls, "dataDir",          "Ljava/lang/String;");
    jfieldID fLib   = env->GetFieldID(aiCls, "nativeLibraryDir", "Ljava/lang/String;");
    jfieldID fUid   = env->GetFieldID(aiCls, "uid",              "I");
    jfieldID fFlags = env->GetFieldID(aiCls, "flags",            "I");
    env->DeleteLocalRef(aiCls);
    jstring jApk  = env->NewStringUTF(apkPath);
    jstring jLib  = env->NewStringUTF(libDir);
    jstring jData = env->NewStringUTF(dataDir);
    if (fSrc)  env->SetObjectField(ai, fSrc,  jApk);
    if (fPub)  env->SetObjectField(ai, fPub,  jApk);
    if (fLib)  env->SetObjectField(ai, fLib,  jLib);
    if (fData) env->SetObjectField(ai, fData, jData);
    if (fUid)  env->SetIntField(ai, fUid, uid);
    if (fFlags) {
        jint flags = env->GetIntField(ai, fFlags);
        flags |= 0x00000002;
        env->SetIntField(ai, fFlags, flags);
        LOGD("FixApplicationInfo: flags=0x%08x", flags);
    }
    env->DeleteLocalRef(jApk);
    env->DeleteLocalRef(jLib);
    env->DeleteLocalRef(jData);
    if (env->ExceptionCheck()) { LogAndClearException(env, "FixApplicationInfo"); return false; }
    LOGD("FixApplicationInfo: apk=%s lib=%s data=%s uid=%d", apkPath, libDir, dataDir, uid);
    return true;
}

static jint GetMyUid(JNIEnv* /*env*/) {
    return (jint)syscall(SYS_getuid);
}

static jobject BootViaLoadedApk(JNIEnv* env, jobject ai, jobject guestCtx) {
    jclass ciCls = env->FindClass("android/content/res/CompatibilityInfo");
    if (!ciCls) { env->ExceptionClear(); return nullptr; }
    jobject ci = ReadStaticObjectField(env, ciCls, "DEFAULT_COMPATIBILITY_INFO");
    env->DeleteLocalRef(ciCls);
    if (!ci) {
        env->ExceptionClear(); LOGE("BootLoadedApk: no DEFAULT_COMPATIBILITY_INFO");
        return nullptr;
    }
    jclass atCls = env->FindClass("android/app/ActivityThread");
    jmethodID curM = env->GetStaticMethodID(atCls, "currentActivityThread",
                                            "()Landroid/app/ActivityThread;");
    jobject at = curM ? env->CallStaticObjectMethod(atCls, curM) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); at = nullptr; }
    if (!at) { LOGE("BootLoadedApk: no ActivityThread"); env->DeleteLocalRef(ci); env->DeleteLocalRef(atCls); return nullptr; }
    jclass aiCls = env->FindClass("android/content/pm/ApplicationInfo");
    jclass ciCls2 = env->FindClass("android/content/res/CompatibilityInfo");
    jobject method = ReflectGetDeclaredMethod(env, atCls, "getPackageInfoNoCheck", {aiCls, ciCls2});
    env->DeleteLocalRef(aiCls);
    env->DeleteLocalRef(ciCls2);
    jobject loadedApk = nullptr;
    if (method) {
        jmethodID mid = env->FromReflectedMethod(method);
        if (mid) {
            loadedApk = env->CallObjectMethod(at, mid, ai, ci);
            if (env->ExceptionCheck()) { LogAndClearException(env, "getPackageInfoNoCheck"); loadedApk = nullptr; }
        }
        env->DeleteLocalRef(method);
    }
    env->DeleteLocalRef(ci);
    env->DeleteLocalRef(at);
    env->DeleteLocalRef(atCls);
    if (!loadedApk) { LOGE("BootLoadedApk: getPackageInfoNoCheck failed"); return nullptr; }
    LOGD("BootLoadedApk: got LoadedApk=%p", (void*)loadedApk);
    jclass laCls = env->GetObjectClass(loadedApk);
    jfieldID fSec = env->GetFieldID(laCls, "mSecurityViolation", "Z");
    if (fSec) { env->SetBooleanField(loadedApk, fSec, JNI_FALSE); if (env->ExceptionCheck()) env->ExceptionClear(); }
    else env->ExceptionClear();
    jfieldID fAppInfo = env->GetFieldID(laCls, "mApplicationInfo", "Landroid/content/pm/ApplicationInfo;");
    if (fAppInfo) { env->SetObjectField(loadedApk, fAppInfo, ai); if (env->ExceptionCheck()) env->ExceptionClear(); }
    else env->ExceptionClear();
    env->DeleteLocalRef(laCls);
    LOGD("BootLoadedApk: flags cleared");
    if (guestCtx) {
        jclass ctxCls = env->FindClass("android/content/Context");
        jmethodID getClM = env->GetMethodID(ctxCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
        jmethodID getResM = env->GetMethodID(ctxCls, "getResources", "()Landroid/content/res/Resources;");
        env->DeleteLocalRef(ctxCls);
        jobject loader = getClM ? env->CallObjectMethod(guestCtx, getClM) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); loader = nullptr; }
        jobject resources = getResM ? env->CallObjectMethod(guestCtx, getResM) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); resources = nullptr; }
        jclass laCls3 = env->GetObjectClass(loadedApk);
        if (loader) {
            jfieldID fMClassLoader = env->GetFieldID(laCls3, "mClassLoader", "Ljava/lang/ClassLoader;");
            if (fMClassLoader) {
                env->SetObjectField(loadedApk, fMClassLoader, loader);
                if (env->ExceptionCheck()) env->ExceptionClear();
                else LOGD("BootLoadedApk: mClassLoader pre-set");
            } else { env->ExceptionClear(); }
            env->DeleteLocalRef(loader);
        }
        if (resources) {
            jfieldID fMResources = env->GetFieldID(laCls3, "mResources", "Landroid/content/res/Resources;");
            if (fMResources) {
                env->SetObjectField(loadedApk, fMResources, resources);
                if (env->ExceptionCheck()) env->ExceptionClear();
                else LOGD("BootLoadedApk: mResources pre-set");
            } else { env->ExceptionClear(); }
            env->DeleteLocalRef(resources);
        }
        env->DeleteLocalRef(laCls3);
    }
    jclass laCls2 = env->GetObjectClass(loadedApk);
    jclass boolCls = env->FindClass("java/lang/Boolean");
    jfieldID fBoolType = env->GetStaticFieldID(boolCls, "TYPE", "Ljava/lang/Class;");
    jobject boolType = env->GetStaticObjectField(boolCls, fBoolType);
    env->DeleteLocalRef(boolCls);
    jclass instrCls = env->FindClass("android/app/Instrumentation");
    jclass habCls = nullptr;
    jobject habInst = GetHiddenApiBridgeInstance(env, &habCls);
    jobject makeAppMethod = nullptr;
    if (habInst) {
        jmethodID refM = env->GetMethodID(habCls, "reflectGetDeclaredMethod",
                                          "(Ljava/lang/Class;Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
        if (refM) {
            jclass classCls = env->FindClass("java/lang/Class");
            jobjectArray params = env->NewObjectArray(2, classCls, nullptr);
            env->SetObjectArrayElement(params, 0, boolType);
            env->SetObjectArrayElement(params, 1, instrCls);
            env->DeleteLocalRef(classCls);
            jstring jName = env->NewStringUTF("makeApplication");
            makeAppMethod = env->CallObjectMethod(habInst, refM, laCls2, jName, params);
            if (env->ExceptionCheck()) { env->ExceptionClear(); makeAppMethod = nullptr; }
            env->DeleteLocalRef(jName);
            env->DeleteLocalRef(params);
        }
        env->DeleteLocalRef(habInst);
        env->DeleteLocalRef(habCls);
    }
    env->DeleteLocalRef(boolType);
    env->DeleteLocalRef(instrCls);
    env->DeleteLocalRef(laCls2);
    jobject app = nullptr;
    if (makeAppMethod) {
        jclass methodCls = env->FindClass("java/lang/reflect/Method");
        jmethodID invokeM = env->GetMethodID(methodCls, "invoke",
                                             "(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;");
        env->DeleteLocalRef(methodCls);
        jclass objCls = env->FindClass("java/lang/Object");
        jobjectArray args = env->NewObjectArray(2, objCls, nullptr);
        env->DeleteLocalRef(objCls);
        jclass bCls = env->FindClass("java/lang/Boolean");
        jmethodID bVal = env->GetStaticMethodID(bCls, "valueOf", "(Z)Ljava/lang/Boolean;");
        jobject jFalse = env->CallStaticObjectMethod(bCls, bVal, (jboolean)JNI_FALSE);
        env->DeleteLocalRef(bCls);
        env->SetObjectArrayElement(args, 0, jFalse);
        env->SetObjectArrayElement(args, 1, nullptr);
        env->DeleteLocalRef(jFalse);
        LOGD("BootLoadedApk: invoking makeApplication");
        app = env->CallObjectMethod(makeAppMethod, invokeM, loadedApk, args);
        bool hadEx = env->ExceptionCheck() == JNI_TRUE;
        if (hadEx) { LogAndClearException(env, "LoadedApk.makeApplication"); }
        env->DeleteLocalRef(args);
        env->DeleteLocalRef(makeAppMethod);
        if (hadEx) app = nullptr;
    }
    env->DeleteLocalRef(loadedApk);
    if (app) {
        LOGD("BootLoadedApk: Application created=%p", (void*)app);
        jclass aiCls2 = env->FindClass("android/content/pm/ApplicationInfo");
        jfieldID fPkgName = env->GetFieldID(aiCls2, "packageName", "Ljava/lang/String;");
        env->DeleteLocalRef(aiCls2);
        if (fPkgName) {
            jstring jGuestPkg = (jstring)env->GetObjectField(ai, fPkgName);
            if (jGuestPkg) {
                const char* pkg = env->GetStringUTFChars(jGuestPkg, nullptr);
                if (pkg) {
                    PatchActivityThreadState(env, app, ai, pkg);
                    env->ReleaseStringUTFChars(jGuestPkg, pkg);
                }
                env->DeleteLocalRef(jGuestPkg);
            }
        } else env->ExceptionClear();
    }
    return app;
}

static jobject BootManual(JNIEnv* env, jobject guestCtx) {
    jclass ctxCls = env->FindClass("android/content/Context");
    jmethodID getAiM = env->GetMethodID(ctxCls, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jmethodID getClM = env->GetMethodID(ctxCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    env->DeleteLocalRef(ctxCls);
    jobject ai = env->CallObjectMethod(guestCtx, getAiM);
    jobject loader = env->CallObjectMethod(guestCtx, getClM);
    if (!ai || !loader) {
        if (ai) env->DeleteLocalRef(ai);
        if (loader) env->DeleteLocalRef(loader);
        return nullptr;
    }
    jclass aiCls = env->FindClass("android/content/pm/ApplicationInfo");
    jfieldID clsF = env->GetFieldID(aiCls, "className", "Ljava/lang/String;");
    env->DeleteLocalRef(aiCls);
    jstring jCls = (jstring)env->GetObjectField(ai, clsF);
    if (!jCls) { env->DeleteLocalRef(ai); env->DeleteLocalRef(loader); return nullptr; }
    jclass clCls = env->FindClass("java/lang/ClassLoader");
    jmethodID loadClassM = env->GetMethodID(clCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
    env->DeleteLocalRef(clCls);
    jobject guestCls = env->CallObjectMethod(loader, loadClassM, jCls);
    env->DeleteLocalRef(jCls);
    jobject guestApp = nullptr;
    if (guestCls) {
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
                if (env->ExceptionCheck()) { LogAndClearException(env, "manual newInstance"); guestApp = nullptr; }
            }
            env->DeleteLocalRef(ctor);
        }
        env->DeleteLocalRef(guestCls);
    }
    if (guestApp) {
        jclass appCls = env->FindClass("android/app/Application");
        jmethodID attachM = env->GetMethodID(appCls, "attachBaseContext",
                                             "(Landroid/content/Context;)V");
        env->DeleteLocalRef(appCls);
        if (attachM) {
            env->CallVoidMethod(guestApp, attachM, guestCtx);
            if (env->ExceptionCheck()) { LogAndClearException(env, "manual attach"); env->DeleteLocalRef(guestApp); guestApp = nullptr; }
        }
    }
    env->DeleteLocalRef(ai);
    env->DeleteLocalRef(loader);
    return guestApp;
}

static jobject BootGuest(JNIEnv* env, jobject proxy) {
    if (!g_vm) env->GetJavaVM(&g_vm);
    jobject guestCtx = GetGuestContext(env, proxy);
    if (!guestCtx) { LOGE("BootGuest: no guestCtx"); return nullptr; }
    jclass ctxCls = env->FindClass("android/content/Context");
    jmethodID getAiM   = env->GetMethodID(ctxCls, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    jmethodID getPkgM  = env->GetMethodID(ctxCls, "getPackageName", "()Ljava/lang/String;");
    env->DeleteLocalRef(ctxCls);
    jobject ai = env->CallObjectMethod(guestCtx, getAiM);
    jstring jPkg = (jstring)env->CallObjectMethod(guestCtx, getPkgM);
    if (!ai || !jPkg) {
        LOGE("BootGuest: no ai or pkg");
        if (ai) env->DeleteLocalRef(ai);
        if (jPkg) env->DeleteLocalRef(jPkg);
        env->DeleteLocalRef(guestCtx);
        return nullptr;
    }
    const char* guestPkg = env->GetStringUTFChars(jPkg, nullptr);
    if (!guestPkg) { env->DeleteLocalRef(jPkg); env->DeleteLocalRef(ai); env->DeleteLocalRef(guestCtx); return nullptr; }
    jclass aiCls = env->FindClass("android/content/pm/ApplicationInfo");
    jfieldID fSrc = env->GetFieldID(aiCls, "sourceDir", "Ljava/lang/String;");
    env->DeleteLocalRef(aiCls);
    jstring jApk = (jstring)env->GetObjectField(ai, fSrc);
    if (!jApk) {
        LOGE("BootGuest: no sourceDir");
        env->ReleaseStringUTFChars(jPkg, guestPkg);
        env->DeleteLocalRef(jPkg); env->DeleteLocalRef(ai); env->DeleteLocalRef(guestCtx);
        return nullptr;
    }
    const char* apkPath = env->GetStringUTFChars(jApk, nullptr);
    if (!apkPath) {
        env->ReleaseStringUTFChars(jPkg, guestPkg);
        env->DeleteLocalRef(jApk); env->DeleteLocalRef(jPkg);
        env->DeleteLocalRef(ai); env->DeleteLocalRef(guestCtx);
        return nullptr;
    }
    LOGD("BootGuest: pkg=%s apk=%s", guestPkg, apkPath);
    SetProcessArgV0(env, guestPkg);
    jobject guestApp = nullptr;
    jint myUid = GetMyUid(env);
    jstring jDataDir = ReadGuestDataDir(env, proxy);
    std::string dataDirStr;
    if (jDataDir) {
        const char* c = env->GetStringUTFChars(jDataDir, nullptr);
        if (c) { dataDirStr = c; env->ReleaseStringUTFChars(jDataDir, c); }
        env->DeleteLocalRef(jDataDir);
    }
    if (dataDirStr.empty()) {
        dataDirStr = std::string("/data/user/0/com.ims134.hookengine/files/guest-data/") + guestPkg;
    }
    jclass aiCls2 = env->FindClass("android/content/pm/ApplicationInfo");
    jfieldID fLib = env->GetFieldID(aiCls2, "nativeLibraryDir", "Ljava/lang/String;");
    env->DeleteLocalRef(aiCls2);
    jstring jLib = (jstring)env->GetObjectField(ai, fLib);
    const char* libDir = jLib ? env->GetStringUTFChars(jLib, nullptr) : "";
    if (myUid >= 0 && !dataDirStr.empty()) {
        if (FixApplicationInfo(env, ai, apkPath, libDir ? libDir : "", dataDirStr.c_str(), myUid)) {
            LOGD("BootGuest: trying LoadedApk path");
            guestApp = BootViaLoadedApk(env, ai, guestCtx);
            if (guestApp) LOGD("BootGuest: LoadedApk path SUCCEEDED");
            else LOGE("BootGuest: LoadedApk path failed");
        }
    }
    if (jLib) env->ReleaseStringUTFChars(jLib, libDir);
    env->DeleteLocalRef(jLib);
    if (!guestApp) {
        LOGD("BootGuest: trying manual path");
        guestApp = BootManual(env, guestCtx);
        if (guestApp) LOGD("BootGuest: manual path SUCCEEDED");
        else LOGE("BootGuest: manual path failed");
    }
    env->ReleaseStringUTFChars(jPkg, guestPkg);
    env->ReleaseStringUTFChars(jApk, apkPath);
    env->DeleteLocalRef(jApk); env->DeleteLocalRef(jPkg);
    env->DeleteLocalRef(ai); env->DeleteLocalRef(guestCtx);
    return guestApp;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnCreate(JNIEnv* env, jobject thiz) {
    LOGD("nativeOnCreate: entry");
    if (!env) return;
    jobject guestApp = BootGuest(env, thiz);
    if (!guestApp) { LOGE("nativeOnCreate: boot failed"); return; }
    {
        std::lock_guard<std::mutex> lk(g_guest_mutex);
        if (g_guest_app) env->DeleteGlobalRef(g_guest_app);
        g_guest_app = env->NewGlobalRef(guestApp);
        env->DeleteLocalRef(guestApp);
        g_guest_created = false;
    }
    LOGD("nativeOnCreate: complete");
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeAttachGuest(JNIEnv* env, jobject thiz) {
    if (!env) return JNI_FALSE;
    jobject guestApp = BootGuest(env, thiz);
    if (!guestApp) return JNI_FALSE;
    {
        std::lock_guard<std::mutex> lk(g_guest_mutex);
        if (g_guest_app) env->DeleteGlobalRef(g_guest_app);
        g_guest_app = env->NewGlobalRef(guestApp);
        env->DeleteLocalRef(guestApp);
        g_guest_created = false;
    }
    return JNI_TRUE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeCreateGuest(JNIEnv* env, jobject) {
    jobject guestApp;
    { std::lock_guard<std::mutex> lk(g_guest_mutex); guestApp = g_guest_app; }
    if (!guestApp) { LOGE("nativeCreateGuest: no guest"); return JNI_FALSE; }
    jclass threadCls = env->FindClass("java/lang/Thread");
    jmethodID curThM = env->GetStaticMethodID(threadCls, "currentThread", "()Ljava/lang/Thread;");
    jmethodID getClM = env->GetMethodID(threadCls, "getContextClassLoader", "()Ljava/lang/ClassLoader;");
    jmethodID setClM = env->GetMethodID(threadCls, "setContextClassLoader", "(Ljava/lang/ClassLoader;)V");
    jobject thread = curThM ? env->CallStaticObjectMethod(threadCls, curThM) : nullptr;
    jobject oldCL = (thread && getClM) ? env->CallObjectMethod(thread, getClM) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject guestCL = nullptr;
    {
        jclass ctxCls2 = env->FindClass("android/content/Context");
        jmethodID getCLM = env->GetMethodID(ctxCls2, "getClassLoader", "()Ljava/lang/ClassLoader;");
        if (getCLM) guestCL = env->CallObjectMethod(guestApp, getCLM);
        env->DeleteLocalRef(ctxCls2);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (thread && guestCL && setClM) {
        env->CallVoidMethod(thread, setClM, guestCL);
        if (env->ExceptionCheck()) env->ExceptionClear();
        LOGD("nativeCreateGuest: contextClassLoader set");
    }
    jclass appCls = env->FindClass("android/app/Application");
    jmethodID onCreateM = appCls ? env->GetMethodID(appCls, "onCreate", "()V") : nullptr;
    if (appCls) env->DeleteLocalRef(appCls);
    bool ok = false;
    if (onCreateM) {
        env->CallVoidMethod(guestApp, onCreateM);
        if (!env->ExceptionCheck()) {
            ok = true;
            LOGD("nativeCreateGuest: onCreate succeeded");
        } else {
            jthrowable ex = env->ExceptionOccurred();
            env->ExceptionClear();
            bool isViberComponentError = false;
            if (ex) {
                jclass illArgCls = env->FindClass("java/lang/IllegalArgumentException");
                if (illArgCls) {
                    if (env->IsInstanceOf(ex, illArgCls)) {
                        jclass thCls = env->FindClass("java/lang/Throwable");
                        if (thCls) {
                            jmethodID getMsgM = env->GetMethodID(thCls, "getMessage", "()Ljava/lang/String;");
                            if (getMsgM) {
                                jstring msg = (jstring)env->CallObjectMethod(ex, getMsgM);
                                if (msg) {
                                    const char* m = env->GetStringUTFChars(msg, nullptr);
                                    if (m) {
                                        if (strstr(m, "unknown component class")) isViberComponentError = true;
                                        env->ReleaseStringUTFChars(msg, m);
                                    }
                                    env->DeleteLocalRef(msg);
                                }
                                if (env->ExceptionCheck()) env->ExceptionClear();
                            }
                            env->DeleteLocalRef(thCls);
                        }
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    env->DeleteLocalRef(illArgCls);
                }
                env->DeleteLocalRef(ex);
            }
            if (isViberComponentError) {
                LOGD("nativeCreateGuest: Viber DI swallowed");
                ok = true;
            } else {
                LOGE("nativeCreateGuest: unexpected exception");
                ok = false;
            }
        }
    }
    if (thread && oldCL && setClM) {
        env->CallVoidMethod(thread, setClM, oldCL);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (guestCL) env->DeleteLocalRef(guestCL);
    if (oldCL) env->DeleteLocalRef(oldCL);
    if (thread) env->DeleteLocalRef(thread);
    env->DeleteLocalRef(threadCls);
    if (ok) {
        std::lock_guard<std::mutex> lk(g_guest_mutex);
        g_guest_created = true;
        LOGD("nativeCreateGuest: done (created=true)");
        return JNI_TRUE;
    }
    return JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeIsGuestCreated(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    return g_guest_created ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnTerminate(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    if (!g_guest_app) return;
    jclass appCls = env->FindClass("android/app/Application");
    jmethodID m = env->GetMethodID(appCls, "onTerminate", "()V");
    env->DeleteLocalRef(appCls);
    if (m) { env->CallVoidMethod(g_guest_app, m); if (env->ExceptionCheck()) LogAndClearException(env, "onTerminate"); }
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnConfigurationChanged(JNIEnv* env, jobject, jobject newConfig) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    if (!g_guest_app) return;
    jclass appCls = env->FindClass("android/app/Application");
    jmethodID m = env->GetMethodID(appCls, "onConfigurationChanged", "(Landroid/content/res/Configuration;)V");
    env->DeleteLocalRef(appCls);
    if (m) { env->CallVoidMethod(g_guest_app, m, newConfig); if (env->ExceptionCheck()) LogAndClearException(env, "onConfigurationChanged"); }
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeOnDestroy(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    if (g_guest_app) { env->DeleteGlobalRef(g_guest_app); g_guest_app = nullptr; }
    g_guest_created = false;
    LOGD("onDestroy: ref cleared");
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeGetGuestApplication(JNIEnv*, jobject) {
    std::lock_guard<std::mutex> lk(g_guest_mutex);
    return g_guest_app;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ApplicationProxy_nativeBuildGuestResources(JNIEnv* env, jobject, jstring jApkPath, jobject jHostContext) {
    if (!jApkPath || !jHostContext) return nullptr;
    const char* apkPath = env->GetStringUTFChars(jApkPath, nullptr);
    if (!apkPath) return nullptr;
    jobject resources = nullptr, am = nullptr, hostRes = nullptr, metrics = nullptr, config = nullptr;
    do {
        jclass amCls = env->FindClass("android/content/res/AssetManager");
        if (!amCls) break;
        jmethodID amCtor = env->GetMethodID(amCls, "<init>", "()V");
        if (!amCtor) { env->DeleteLocalRef(amCls); break; }
        am = env->NewObject(amCls, amCtor);
        jmethodID addPathM = env->GetMethodID(amCls, "addAssetPath", "(Ljava/lang/String;)I");
        env->DeleteLocalRef(amCls);
        if (env->ExceptionCheck() || !am || !addPathM) { env->ExceptionClear(); break; }
        jstring jApk = env->NewStringUTF(apkPath);
        jint addRc = env->CallIntMethod(am, addPathM, jApk);
        env->DeleteLocalRef(jApk);
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        LOGD("buildGuestRes: addAssetPath -> %d", (int)addRc);
        if (addRc == 0) break;
        jclass ctxCls = env->FindClass("android/content/Context");
        jmethodID getResM = env->GetMethodID(ctxCls, "getResources", "()Landroid/content/res/Resources;");
        env->DeleteLocalRef(ctxCls);
        if (!getResM) break;
        hostRes = env->CallObjectMethod(jHostContext, getResM);
        if (env->ExceptionCheck()) { env->ExceptionClear(); hostRes = nullptr; }
        if (!hostRes) break;
        jclass resCls = env->FindClass("android/content/res/Resources");
        jmethodID getMetricsM = env->GetMethodID(resCls, "getDisplayMetrics", "()Landroid/util/DisplayMetrics;");
        jmethodID getConfigM = env->GetMethodID(resCls, "getConfiguration", "()Landroid/content/res/Configuration;");
        jmethodID resCtor = env->GetMethodID(resCls, "<init>",
                                             "(Landroid/content/res/AssetManager;Landroid/util/DisplayMetrics;Landroid/content/res/Configuration;)V");
        if (!getMetricsM || !getConfigM || !resCtor) { env->DeleteLocalRef(resCls); break; }
        metrics = env->CallObjectMethod(hostRes, getMetricsM);
        config = env->CallObjectMethod(hostRes, getConfigM);
        if (env->ExceptionCheck()) env->ExceptionClear();
        resources = env->NewObject(resCls, resCtor, am, metrics, config);
        env->DeleteLocalRef(resCls);
        if (env->ExceptionCheck()) { LogAndClearException(env, "new Resources"); resources = nullptr; }
    } while (0);
    if (metrics) env->DeleteLocalRef(metrics);
    if (config) env->DeleteLocalRef(config);
    if (hostRes) env->DeleteLocalRef(hostRes);
    if (am) env->DeleteLocalRef(am);
    env->ReleaseStringUTFChars(jApkPath, apkPath);
    return resources;
}