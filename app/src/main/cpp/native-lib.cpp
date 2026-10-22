#include <jni.h>
#include <string>
#include <android/log.h>
#include <android/api-level.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <time.h>
#include <atomic>
#include <fcntl.h>
#include "Hooks.h"
#include "RedirectTable.h"
#include "HookRegistry.h"
#include "TestReport.h"
#include "test_suite.h"

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

extern "C" int UninstallAllHooks();

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
    LOGD("JNI_OnLoad: cached Thread=%p Elem=%p", g_threadCls, g_elemCls);
    return JNI_VERSION_1_6;
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

extern "C" int32_t JNICALL FakeBinderGetCallingUid(JNIEnv*, jclass) {
    if (g_systemCallDepth.load(std::memory_order_acquire) > 0)
        return (int32_t)syscall(SYS_getuid);
    char tname[32] = {0};
    if (pthread_getname_np(pthread_self(), tname, sizeof(tname)) == 0) {
        if (strncmp(tname, "Binder:", 7) == 0 || strncmp(tname, "binder:", 7) == 0)
            return (int32_t)syscall(SYS_getuid);
    }
    return g_virtualUid;
}

static void** g_hooked_entry_point = nullptr;
static void*  g_hooked_page        = nullptr;
static int    g_hooked_orig_prot   = 0;
static long   g_hooked_page_size   = 0;

// ─────────────────────────────────────────────────────────────────────────
// Supported Android API levels — VERIFIED ONLY
// ─────────────────────────────────────────────────────────────────────────
// VERIFIED: API 37 (Android 17) on Xiaomi 17T / HyperOS / ARM64 only.
//
// The ArtMethod layout, entry-point offset (24), offset scan range
// (24..48) and mprotect-with-origProt|WRITE behavior have NOT been tested
// on API 34, 35, 36, or 38+. Installing HookJavaMethod on an unverified
// API level may silently corrupt ART.
//
// The gate refuses to install on anything outside [kMinApi, kMaxApi].
// To widen the range:
//   1. Run the full test suite (items 1–6) on a physical device at the
//      new API level.
//   2. Confirm all tests pass, particularly `hook_works_when_gate_allows`
//      and `hook_refused_when_gate_blocks`.
//   3. Extend kMinApi / kMaxApi and record the verification date.
// ─────────────────────────────────────────────────────────────────────────
static constexpr int kMinApi = 37;
static constexpr int kMaxApi = 37;

extern "C" int g_force_api_override = -1;

extern "C" jint CheckHookCapability() {
    int api = (g_force_api_override >= 0) ? g_force_api_override
                                          : android_get_device_api_level();
    if (api < kMinApi || api > kMaxApi) return HOOK_UNSUPPORTED_API;
    if (sizeof(void*) != 8) return HOOK_UNSUPPORTED_ABI;
    return HOOK_OK;
}

static HookResult HookJavaMethod(JNIEnv* env, jobject methodObj,
                                 void* newFunc, void** originalFunc) {
    jint cap = CheckHookCapability();
    if (cap != HOOK_OK) {
        int eff = (g_force_api_override >= 0) ? g_force_api_override
                                              : android_get_device_api_level();
        LOGE("HookJavaMethod: capability check failed (%d) effective_api=%d "
             "(real=%d, range=[%d,%d])",
             (int)cap, eff, android_get_device_api_level(), kMinApi, kMaxApi);
        return (HookResult)cap;
    }

    jmethodID methodId = env->FromReflectedMethod(methodObj);
    if (!methodId) { LOGE("jmethodID null"); return HOOK_INTERNAL_ERROR; }
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
    if (!entryPoint) { LOGE("no native entry in ArtMethod"); return HOOK_SYMBOL_MISSING; }

    *originalFunc = *entryPoint;

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t pg = (uintptr_t)entryPoint & ~(ps - 1);

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
        return HOOK_MPROTECT_FAILED;
    }
    *entryPoint = newFunc;
    __builtin___clear_cache((char*)artMethod, (char*)artMethod + 64);
    if (mprotect((void*)pg, ps, origProt) != 0) {
        LOGE("HookJavaMethod: restore mprotect failed: %d", errno);
        return HOOK_MPROTECT_FAILED;
    }

    g_hooked_page        = (void*)pg;
    g_hooked_entry_point = entryPoint;
    g_hooked_orig_prot   = origProt;
    g_hooked_page_size   = ps;

    LOGD("ArtMethod hook at offset %d, restored to 0x%x",
         (int)((uint8_t*)entryPoint - (uint8_t*)artMethod), origProt);
    return HOOK_OK;
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

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_hookBinderMethod(
        JNIEnv* env, jobject, jobject methodObj) {
    void* saved = nullptr;
    HookResult r = HookJavaMethod(env, methodObj,
                                  (void*)FakeBinderGetCallingUid, &saved);
    if (r == HOOK_OK) {
        _OriginalGetCallingUid = (GetCallingUidFunc)saved;
        LOGD("JNI Hook installed, original=%p", saved);
    } else {
        LOGE("JNI Hook failed: r=%d", (int)r);
    }
    return (jint)r;
}

// ─── Item-5 public diagnostics ────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_capabilityCheck(JNIEnv*, jobject) {
    return CheckHookCapability();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_realDeviceApi(JNIEnv*, jobject) {
    return android_get_device_api_level();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_setApiOverride(JNIEnv*, jobject, jint api) {
    g_force_api_override = api;
    return CheckHookCapability();
}

// ─── Ordinary JNI surface ─────────────────────────────────────────────────
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

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_installNativeHooks(JNIEnv*, jobject) {
    return (jint)InstallHooks();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_hookCount(JNIEnv*, jobject) {
    return (jint)RegistryCount();
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
Java_com_ims134_hookengine_RemoteService_hiddenApiProbe(JNIEnv*, jobject) {
    return HiddenApi_Probe();
}
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_hiddenApiInstall(JNIEnv*, jobject) {
    return HiddenApi_Install();
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_RemoteService_diagRawZip(JNIEnv* env, jobject, jstring path) {
    const char* p = env->GetStringUTFChars(path, nullptr);
    int fd = open(p, O_RDONLY | O_CLOEXEC);
    env->ReleaseStringUTFChars(path, p);
    if (fd < 0) {
        char buf[64];
        snprintf(buf, sizeof(buf), "open failed errno=%d", errno);
        return env->NewStringUTF(buf);
    }
    unsigned char magic[4] = {0};
    ssize_t n = read(fd, magic, 4);
    close(fd);
    char buf[64];
    snprintf(buf, sizeof(buf), "read %zd bytes: %02x %02x %02x %02x",
             n, magic[0], magic[1], magic[2], magic[3]);
    return env->NewStringUTF(buf);
}

// ─────────────────────────────────────────────────────────────────────────
// Test harness JNI bridge (§7.3)
//
// Kotlin NativeTestBridge.runSuite(name) returns an array of strings, one
// per test case, format "PASS|name|detail" or "FAIL|name|detail".
// Return value of RunTestSuite (pass count) is intentionally discarded —
// the array is the source of truth for assertions.
// ─────────────────────────────────────────────────────────────────────────
extern "C" JNIEXPORT jobjectArray JNICALL
Java_com_ims134_hookengine_NativeTestBridge_runSuite(JNIEnv* env, jclass, jstring jsuite) {
    if (!env || !jsuite) return nullptr;
    const char* suite = env->GetStringUTFChars(jsuite, nullptr);
    if (!suite) return nullptr;

    RunTestSuite(env, suite);
    auto lines = testreport::Snapshot();

    env->ReleaseStringUTFChars(jsuite, suite);

    jclass strCls = env->FindClass("java/lang/String");
    if (!strCls) return nullptr;
    jobjectArray arr = env->NewObjectArray((jsize)lines.size(), strCls, nullptr);
    if (!arr) return nullptr;

    for (size_t i = 0; i < lines.size(); ++i) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s|%s|%s",
                 lines[i].pass ? "PASS" : "FAIL",
                 lines[i].name.c_str(), lines[i].detail.c_str());
        env->SetObjectArrayElement(arr, (jsize)i, env->NewStringUTF(buf));
    }
    return arr;
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_uninstallAll(JNIEnv*, jclass) {
    UninstallAllHooks();
}

extern "C" void SetItem4Paths(const char* realDir, const char* fakeDir);

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_setItem4Paths(
        JNIEnv* env, jclass, jstring jReal, jstring jFake) {
    if (!env || !jReal || !jFake) return;
    const char* real = env->GetStringUTFChars(jReal, nullptr);
    const char* fake = env->GetStringUTFChars(jFake, nullptr);
    SetItem4Paths(real, fake);
    if (real) env->ReleaseStringUTFChars(jReal, real);
    if (fake) env->ReleaseStringUTFChars(jFake, fake);
}