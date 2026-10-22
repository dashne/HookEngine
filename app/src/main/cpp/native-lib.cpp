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
#include <time.h>
#include <atomic>
#include <fcntl.h>          // ← ADD
#include "Hooks.h"
#include "RedirectTable.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

typedef int32_t (*GetCallingUidFunc)(JNIEnv*, jclass);
static GetCallingUidFunc _OriginalGetCallingUid = nullptr;

static JavaVM*    g_vm                  = nullptr;
static jclass     g_threadCls           = nullptr;
static jclass     g_elemCls             = nullptr;
static jmethodID  g_currentThreadMethod = nullptr;
static jmethodID  g_getStackTraceMethod = nullptr;
static jmethodID  g_getClassNameMethod  = nullptr;

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    if (!env) return JNI_ERR;

    jclass t = env->FindClass("java/lang/Thread");
    if (t) g_threadCls = (jclass)env->NewGlobalRef(t);
    if (env->ExceptionCheck()) env->ExceptionClear();

    jclass e = env->FindClass("java/lang/StackTraceElement");
    if (e) g_elemCls = (jclass)env->NewGlobalRef(e);
    if (env->ExceptionCheck()) env->ExceptionClear();

    if (g_threadCls) {
        g_currentThreadMethod = env->GetStaticMethodID(g_threadCls,
                                                       "currentThread", "()Ljava/lang/Thread;");
        g_getStackTraceMethod = env->GetMethodID(g_threadCls,
                                                 "getStackTrace", "()[Ljava/lang/StackTraceElement;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (g_elemCls) {
        g_getClassNameMethod = env->GetMethodID(g_elemCls,
                                                "getClassName", "()Ljava/lang/String;");
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    LOGD("JNI_OnLoad: cached Thread=%p Elem=%p (env=%p)",
         g_threadCls, g_elemCls, env);
    return JNI_VERSION_1_6;
}


static __thread int     g_inStackWalk  = 0;

static int64_t NowNs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static bool IsFrameworkClass(const char* cn) {
    if (!cn) return false;
    if (strncmp(cn, "android.app.",          12) == 0) return true;
    if (strncmp(cn, "android.content.",      15) == 0) return true;
    if (strncmp(cn, "android.os.",           11) == 0) return true;
    if (strncmp(cn, "android.view.",         13) == 0) return true;
    if (strncmp(cn, "android.server.",       14) == 0) return true;
    if (strncmp(cn, "com.android.server.",   19) == 0) return true;
    if (strncmp(cn, "com.android.internal.", 21) == 0) return true;
    if (strncmp(cn, "com.android.org.",      16) == 0) return true;
    if (strncmp(cn, "miui.",                  5) == 0) return true;
    if (strncmp(cn, "com.miui.",              9) == 0) return true;
    if (strncmp(cn, "com.mediatek.",         13) == 0) return true;
    if (strncmp(cn, "java.lang.reflect.",    18) == 0) return true;
    if (strncmp(cn, "sun.misc.",              9) == 0) return true;
    return false;
}

static int SafeStackWalk(JNIEnv* env) {
    if (!env) return -1;
    if (!g_threadCls || !g_elemCls || !g_currentThreadMethod ||
        !g_getStackTraceMethod || !g_getClassNameMethod) return -1;

    if (env->PushLocalFrame(64) != JNI_OK) return -1;

    int decision = -1;
    jobject thread = nullptr;
    jobjectArray stack = nullptr;

    thread = env->CallStaticObjectMethod(g_threadCls, g_currentThreadMethod);
    if (env->ExceptionCheck()) { env->ExceptionClear(); goto done; }
    if (!thread) goto done;

    stack = (jobjectArray)env->CallObjectMethod(thread, g_getStackTraceMethod);
    if (env->ExceptionCheck()) { env->ExceptionClear(); goto done; }
    if (!stack) goto done;

    {
        int len = env->GetArrayLength(stack);
        bool sawApp = false;
        bool sawFramework = false;
        const char* firstNonSelf = nullptr;
        char firstNonSelfBuf[128] = {0};

        for (int i = 0; i < len && i < 40; i++) {
            jobject elem = env->GetObjectArrayElement(stack, i);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (!elem) continue;

            jstring cnStr = (jstring)env->CallObjectMethod(elem, g_getClassNameMethod);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (cnStr) {
                const char* cn = env->GetStringUTFChars(cnStr, nullptr);
                if (env->ExceptionCheck()) { env->ExceptionClear(); cn = nullptr; }
                if (cn) {
                    // Record the first class we saw that's not our hook
                    if (!firstNonSelf &&
                        strstr(cn, "com.ims134.hookengine") == nullptr &&
                        strncmp(cn, "java.", 5) != 0 &&
                        strncmp(cn, "dalvik.", 7) != 0) {
                        firstNonSelf = firstNonSelfBuf;
                        strncpy(firstNonSelfBuf, cn, sizeof(firstNonSelfBuf) - 1);
                    }

                    if (IsFrameworkClass(cn)) {
                        sawFramework = true;
                        env->ReleaseStringUTFChars(cnStr, cn);
                        break;
                    }
                    if (strncmp(cn, "com.ims134.hookengine.", 22) == 0) {
                        sawApp = true;
                    }
                    env->ReleaseStringUTFChars(cnStr, cn);
                }
                env->DeleteLocalRef(cnStr);
            }
            env->DeleteLocalRef(elem);
        }

        if (sawFramework) {
            decision = 0;   // real UID
        } else if (sawApp) {
            decision = 1;   // fake UID
        } else {
            // Uncertain: neither framework nor app identified.
            // Default to real UID — safe. Framework keeps working.
            decision = -1;
            if (firstNonSelf) {
                LOGD("getCallingUid: uncertain caller (%s) — returning real UID", firstNonSelf);
            }
        }
    }

    done:
    env->PopLocalFrame(nullptr);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return decision;
}

extern "C" int32_t JNICALL FakeBinderGetCallingUid(JNIEnv*, jclass) {
    if (g_systemCallDepth.load(std::memory_order_acquire) > 0) {
        return (int32_t)syscall(SYS_getuid);
    }
    char tname[32] = {0};
    if (pthread_getname_np(pthread_self(), tname, sizeof(tname)) == 0) {
        if (strncmp(tname, "Binder:", 7) == 0 || strncmp(tname, "binder:", 7) == 0) {
            return (int32_t)syscall(SYS_getuid);
        }
    }
    return g_virtualUid;
}

static void** g_hooked_entry_point = nullptr;
static void*  g_hooked_page        = nullptr;
static int    g_hooked_orig_prot   = 0;
static long   g_hooked_page_size   = 0;

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

    // Read the ACTUAL current protection from /proc/self/maps
    int origProt = PROT_READ | PROT_WRITE;
    FILE* f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof(line), f)) {
            unsigned long start, end;
            char perms[8];
            if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) == 3) {
                if (start <= pg && pg < end) {
                    origProt = 0;
                    if (perms[0] == 'r') origProt |= PROT_READ;
                    if (perms[1] == 'w') origProt |= PROT_WRITE;
                    if (perms[2] == 'x') origProt |= PROT_EXEC;
                    break;
                }
            }
        }
        fclose(f);
    }
    LOGD("HookJavaMethod: page=%p origProt=0x%x (r=%d w=%d x=%d)",
         (void*)pg, origProt,
         (origProt&PROT_READ)?1:0, (origProt&PROT_WRITE)?1:0, (origProt&PROT_EXEC)?1:0);

    if (mprotect((void*)pg, ps, origProt | PROT_WRITE) != 0) {
        LOGE("HookJavaMethod: mprotect RW failed: %d", errno);
        return false;
    }
    *entryPoint = newFunc;
    __builtin___clear_cache((char*)artMethod, (char*)artMethod + 64);
    // ---- restore EXACTLY what was there before, no downgrade ----
    if (mprotect((void*)pg, ps, origProt) != 0) {
        LOGE("HookJavaMethod: restore mprotect failed: %d", errno);
    }

    g_hooked_page        = (void*)pg;
    g_hooked_entry_point = entryPoint;
    g_hooked_orig_prot   = origProt;
    g_hooked_page_size   = ps;

    LOGD("ArtMethod hook at offset %d, restored to 0x%x",
         (int)((uint8_t*)entryPoint - (uint8_t*)artMethod), origProt);
    return true;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_restoreBinderMethod(JNIEnv*, jobject) {
    if (!g_hooked_entry_point || !_OriginalGetCallingUid) {
        LOGD("restoreBinderMethod: nothing to restore");
        return;
    }
    if (mprotect(g_hooked_page, g_hooked_page_size,
                 g_hooked_orig_prot | PROT_WRITE) != 0) {
        LOGE("restoreBinderMethod: mprotect RW failed: %d", errno);
        return;
    }
    *g_hooked_entry_point = (void*)_OriginalGetCallingUid;
    __builtin___clear_cache((char*)g_hooked_entry_point,
                            (char*)g_hooked_entry_point + 8);
    mprotect(g_hooked_page, g_hooked_page_size, g_hooked_orig_prot);
    LOGD("restoreBinderMethod: entry point restored to %p",
         (void*)_OriginalGetCallingUid);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_hookBinderMethod(JNIEnv* env, jobject, jobject methodObj) {
    void* saved = nullptr;
    if (HookJavaMethod(env, methodObj, (void*)FakeBinderGetCallingUid, &saved)) {
        _OriginalGetCallingUid = (GetCallingUidFunc)saved;
        LOGD("JNI Hook installed, original=%p", saved);
    } else {
        LOGE("JNI Hook failed.");
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_setInSystemCall(JNIEnv*, jobject, jboolean on) {
    if (on) g_systemCallDepth.fetch_add(1, std::memory_order_acq_rel);
    else    g_systemCallDepth.fetch_sub(1, std::memory_order_acq_rel);
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
    if (!target) { LOGE("getpid not found"); return JNI_FALSE; }
    bool ok = InstallInlineHook(target, (void*)Fake_getpid, (void**)&_Original_getpid);
    LOGD("inline hook getpid: %s", ok ? "OK" : "FAILED");
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" int HiddenApi_Install();
extern "C" int HiddenApi_Probe();
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_hiddenApiProbe(JNIEnv*, jobject) { return HiddenApi_Probe(); }
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_hiddenApiInstall(JNIEnv*, jobject) { return HiddenApi_Install(); }

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_RemoteService_diagRawZip(JNIEnv* env, jobject, jstring path) {
    // Pure JNI check: open the APK as a ZIP via libziparchive without touching
    // Java PackageManager. If this crashes, the file itself is broken.
    const char* p = env->GetStringUTFChars(path, nullptr);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    env->ReleaseStringUTFChars(path, p);
    if (fd < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "open failed errno=%d", errno);
        return env->NewStringUTF(buf);
    }
    // Read first 4 bytes — ZIP magic PK\3\4
    unsigned char magic[4] = {0};
    ssize_t n = read(fd, magic, 4);
    close(fd);
    char buf[64];
    snprintf(buf, sizeof(buf), "read %zd bytes: %02x %02x %02x %02x",
             n, magic[0], magic[1], magic[2], magic[3]);
    return env->NewStringUTF(buf);
}