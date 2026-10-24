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
#include "ProcessHooks.h"

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

static void RestoreBinderMethodImpl() {
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
Java_com_ims134_hookengine_RemoteService_restoreBinderMethod(JNIEnv*, jobject) {
    RestoreBinderMethodImpl();
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
Java_com_ims134_hookengine_NativeTestBridge_runSuite(JNIEnv* env, jobject, jstring jsuite) {
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
Java_com_ims134_hookengine_NativeTestBridge_uninstallAll(JNIEnv*, jobject) {
    UninstallAllHooks();
}

extern "C" void SetItem4Paths(const char* realDir, const char* fakeDir);

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_setItem4Paths(
        JNIEnv* env, jobject , jstring jReal, jstring jFake) {
    if (!env || !jReal || !jFake) return;
    const char* real = env->GetStringUTFChars(jReal, nullptr);
    const char* fake = env->GetStringUTFChars(jFake, nullptr);
    SetItem4Paths(real, fake);
    if (real) env->ReleaseStringUTFChars(jReal, real);
    if (fake) env->ReleaseStringUTFChars(jFake, fake);
}

// ─────────────────────────────────────────────────────────────────────────
// Regression bug 1: HookJavaMethod destroyed page write permission.
// ─────────────────────────────────────────────────────────────────────────
static bool ReadPageProtReg(uintptr_t page, int* outProt) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3) {
            if (s <= page && page < e) {
                int p = 0;
                if (perms[0] == 'r') p |= PROT_READ;
                if (perms[1] == 'w') p |= PROT_WRITE;
                if (perms[2] == 'x') p |= PROT_EXEC;
                *outProt = p;
                ok = true;
                break;
            }
        }
    }
    fclose(f);
    return ok;
}

extern "C" int RunRegressionBug1(JNIEnv* env) {
    if (!env) {
        testreport::Report("bug1_page_prot_restored", false, "no JNIEnv");
        return 0;
    }
    jclass binderCls = env->FindClass("android/os/Binder");
    if (!binderCls) {
        testreport::Report("bug1_page_prot_restored", false, "FindClass failed");
        return 0;
    }
    jmethodID m = env->GetStaticMethodID(binderCls, "getCallingUid", "()I");
    if (!m) {
        env->DeleteLocalRef(binderCls);
        testreport::Report("bug1_page_prot_restored", false, "GetStaticMethodID failed");
        return 0;
    }
    jobject method = env->ToReflectedMethod(binderCls, m, JNI_TRUE);
    env->DeleteLocalRef(binderCls);
    if (!method) {
        testreport::Report("bug1_page_prot_restored", false, "ToReflectedMethod failed");
        return 0;
    }

    jmethodID methodId = env->FromReflectedMethod(method);
    uintptr_t artMethod = (uintptr_t)methodId;
    uintptr_t pg = artMethod & ~(uintptr_t)0xFFF;

    int prot_before = 0, prot_install = 0, prot_restore = 0;
    bool ok_before = ReadPageProtReg(pg, &prot_before);

    void* saved = nullptr;
    HookResult hr = HookJavaMethod(env, method, (void*)FakeBinderGetCallingUid, &saved);
    if (hr == HOOK_OK) {
        _OriginalGetCallingUid = (GetCallingUidFunc)saved;
        ReadPageProtReg(pg, &prot_install);
        RestoreBinderMethodImpl();
        ReadPageProtReg(pg, &prot_restore);
    }

    char d[160];
    snprintf(d, sizeof d,
             "before=0x%x install=0x%x restore=0x%x hr=%d",
             prot_before, prot_install, prot_restore, (int)hr);

    bool pass = ok_before
                && hr == HOOK_OK
                && prot_before == prot_install
                && prot_install == prot_restore;
    testreport::Report("bug1_page_prot_restored", pass, d);

    env->DeleteLocalRef(method);
    return pass ? 1 : 0;
}

// ─────────────────────────────────────────────────────────────────────────
// §7.7 — selftest.txt file channel. Called once per onStartCommand, after
// all 5 item suites have run (sink holds 23 lines). Writes each as
// "PASS|name|detail" or "FAIL|name|detail", then "END <pass> <fail>",
// then resets the sink for the next run.
// ─────────────────────────────────────────────────────────────────────────
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_writeSelftestFile(
        JNIEnv* env, jobject, jstring jpath) {
    if (!env || !jpath) return -1;
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    if (!path) return -1;
    auto lines = testreport::Snapshot();
    FILE* f = fopen(path, "w");
    env->ReleaseStringUTFChars(jpath, path);
    if (!f) return -1;
    int pass = 0, fail = 0;
    for (const auto& l : lines) {
        fprintf(f, "%s|%s|%s\n",
                l.pass ? "PASS" : "FAIL",
                l.name.c_str(), l.detail.c_str());
        if (l.pass) pass++; else fail++;
    }
    fprintf(f, "END %d %d\n", pass, fail);
    fclose(f);
    testreport::Reset();
    return pass;
}
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_installProcessHooks(JNIEnv* env, jobject) {
    return InstallProcessHooks(env);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_uninstallProcessHooks(JNIEnv*, jobject) {
    return UninstallProcessHooks();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_spoofProcessStatics(JNIEnv* env, jobject, jint uid, jint pid) {
    return SpoofProcessStatics(env, uid, pid);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_restoreProcessStatics(JNIEnv* env, jobject) {
    RestoreProcessStatics();
}


extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_installProcessHooks(JNIEnv* env, jobject) {
    return InstallProcessHooks(env);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_uninstallProcessHooks(JNIEnv*, jobject) {
    UninstallProcessHooks();
}

extern "C" int HiddenApi_Install();
extern "C" int HiddenApi_Probe();

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_HiddenApiBridge_install(JNIEnv*, jobject) {
    return HiddenApi_Install();
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_HiddenApiBridge_probe(JNIEnv*, jobject) {
    return HiddenApi_Probe();
}

// ─────────────────────────────────────────────────────────────────────────
// PmSpoof JNI bridge — bypasses hiddenapi (JNI GetMethodID/NewObject do not
// consult hiddenapi; only Java-level reflection does).
// ─────────────────────────────────────────────────────────────────────────



static jobject ReadMListFrom(JNIEnv* env, jclass c, jobject slice, const char* label) {
    if (!c) return nullptr;
    jfieldID f = env->GetFieldID(c, "mList", "Ljava/util/List;");
    if (!f) { if (env->ExceptionCheck()) env->ExceptionClear(); return nullptr; }
    jobject v = env->GetObjectField(slice, f);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    if (v) LOGD("getSliceListNative: OK via %s.mList", label);
    return v;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_PmSpoof_getSliceListNative(JNIEnv* env, jobject, jobject slice) {
    if (!slice) return nullptr;

    // 1. GetFieldID on leaf — walks superclasses (this is the key line).
    jclass leaf = env->GetObjectClass(slice);
    if (jobject v = ReadMListFrom(env, leaf, slice, "leaf")) { env->DeleteLocalRef(leaf); return v; }
    env->DeleteLocalRef(leaf);

    // 2. Explicit walk through known names.
    const char* names[] = {
            "android/content/pm/BaseParceledListSlice",
            "android/content/pm/ParceledListSlice",
            nullptr
    };
    for (int i = 0; names[i]; i++) {
        jclass c = env->FindClass(names[i]);
        if (!c) { if (env->ExceptionCheck()) env->ExceptionClear(); continue; }
        if (jobject v = ReadMListFrom(env, c, slice, names[i])) { env->DeleteLocalRef(c); return v; }
        env->DeleteLocalRef(c);
    }

    // 3. Fallback method names (in case ROM renamed getList).
    const char* methodNames[] = { "getList", "getElements", "elements", "asList", "toList", nullptr };
    jclass sliceCls = env->FindClass("android/content/pm/ParceledListSlice");
    if (sliceCls) {
        for (int i = 0; methodNames[i]; i++) {
            jmethodID m = env->GetMethodID(sliceCls, methodNames[i], "()Ljava/util/List;");
            if (!m) { if (env->ExceptionCheck()) env->ExceptionClear(); continue; }
            jobject v = env->CallObjectMethod(slice, m);
            if (env->ExceptionCheck()) { env->ExceptionClear(); continue; }
            if (v) { LOGD("getSliceListNative: OK via %s()", methodNames[i]); env->DeleteLocalRef(sliceCls); return v; }
        }
        env->DeleteLocalRef(sliceCls);
    }

    // 4. Definitive dump — shows every field & method via JNI (bypasses hidden API).
    jclass cls = env->GetObjectClass(slice);
    jclass classCls = env->FindClass("java/lang/Class");
    jmethodID getName = env->GetMethodID(classCls, "getName", "()Ljava/lang/String;");
    jmethodID getSuper = env->GetMethodID(classCls, "getSuperclass", "()Ljava/lang/Class;");
    jmethodID getFields = env->GetMethodID(classCls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;");
    jmethodID getMethods = env->GetMethodID(classCls, "getDeclaredMethods", "()[Ljava/lang/reflect/Method;");
    jclass fieldCls = env->FindClass("java/lang/reflect/Field");
    jmethodID fName = env->GetMethodID(fieldCls, "getName", "()Ljava/lang/String;");
    jmethodID fType = env->GetMethodID(fieldCls, "getType", "()Ljava/lang/Class;");
    jclass methodCls = env->FindClass("java/lang/reflect/Method");
    jmethodID mName = env->GetMethodID(methodCls, "getName", "()Ljava/lang/String;");

    while (cls) {
        jstring cn = (jstring) env->CallObjectMethod(cls, getName);
        const char* cns = cn ? env->GetStringUTFChars(cn, nullptr) : "<null>";
        LOGE("getSliceListNative dump: class %s", cns);
        if (cn) { env->ReleaseStringUTFChars(cn, cns); env->DeleteLocalRef(cn); }

        jobjectArray flds = (jobjectArray) env->CallObjectMethod(cls, getFields);
        if (flds) {
            jsize n = env->GetArrayLength(flds);
            for (jsize i = 0; i < n; i++) {
                jobject f = env->GetObjectArrayElement(flds, i);
                jstring fn = (jstring) env->CallObjectMethod(f, fName);
                jclass ft = (jclass) env->CallObjectMethod(f, fType);
                jstring ftn = ft ? (jstring) env->CallObjectMethod(ft, getName) : nullptr;
                const char* fns = fn ? env->GetStringUTFChars(fn, nullptr) : "?";
                const char* ftns = ftn ? env->GetStringUTFChars(ftn, nullptr) : "?";
                LOGE("getSliceListNative dump:   field %s %s", ftns, fns);
                if (fn) { env->ReleaseStringUTFChars(fn, fns); env->DeleteLocalRef(fn); }
                if (ftn) { env->ReleaseStringUTFChars(ftn, ftns); env->DeleteLocalRef(ftn); }
                if (ft) env->DeleteLocalRef(ft);
                env->DeleteLocalRef(f);
            }
            env->DeleteLocalRef(flds);
        }
        jobjectArray mths = (jobjectArray) env->CallObjectMethod(cls, getMethods);
        if (mths) {
            jsize n = env->GetArrayLength(mths);
            for (jsize i = 0; i < n; i++) {
                jobject m = env->GetObjectArrayElement(mths, i);
                jstring mn = (jstring) env->CallObjectMethod(m, mName);
                const char* mns = mn ? env->GetStringUTFChars(mn, nullptr) : "?";
                LOGE("getSliceListNative dump:   method %s", mns);
                if (mn) { env->ReleaseStringUTFChars(mn, mns); env->DeleteLocalRef(mn); }
                env->DeleteLocalRef(m);
            }
            env->DeleteLocalRef(mths);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        jclass sup = (jclass) env->CallObjectMethod(cls, getSuper);
        env->DeleteLocalRef(cls);
        cls = sup;
    }
    LOGE("getSliceListNative: all paths failed; see dump above");
    return nullptr;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_PmSpoof_replaceSliceListNative(JNIEnv* env, jobject, jobject slice, jobject newList) {
    if (!slice || !newList) return nullptr;

    jclass leaf = env->GetObjectClass(slice);
    if (leaf) {
        jfieldID f = env->GetFieldID(leaf, "mList", "Ljava/util/List;");
        if (f) {
            env->SetObjectField(slice, f, newList);
            if (!env->ExceptionCheck()) {
                LOGD("replaceSliceListNative: OK via leaf.mList");
                env->DeleteLocalRef(leaf);
                return slice;
            }
            env->ExceptionClear();
        }
        env->DeleteLocalRef(leaf);
    }

    const char* names[] = { "android/content/pm/BaseParceledListSlice", nullptr };
    for (int i = 0; names[i]; i++) {
        jclass c = env->FindClass(names[i]);
        if (!c) { if (env->ExceptionCheck()) env->ExceptionClear(); continue; }
        jfieldID f = env->GetFieldID(c, "mList", "Ljava/util/List;");
        if (f) {
            env->SetObjectField(slice, f, newList);
            if (!env->ExceptionCheck()) {
                LOGD("replaceSliceListNative: OK via BaseParceledListSlice.mList");
                env->DeleteLocalRef(c);
                return slice;
            }
            env->ExceptionClear();
        }
        env->DeleteLocalRef(c);
    }

    jclass sliceCls = env->FindClass("android/content/pm/ParceledListSlice");
    if (sliceCls) {
        jmethodID ctor = env->GetMethodID(sliceCls, "<init>", "(Ljava/util/List;)V");
        if (ctor) {
            jobject fresh = env->NewObject(sliceCls, ctor, newList);
            if (!env->ExceptionCheck() && fresh) {
                LOGD("replaceSliceListNative: OK via fresh ParceledListSlice ctor");
                env->DeleteLocalRef(sliceCls);
                return fresh;
            }
            env->ExceptionClear();
        }
        env->DeleteLocalRef(sliceCls);
    }

    LOGE("replaceSliceListNative: all paths failed");
    return nullptr;
}

#include <future>   // add to the top if not already present

// ─────────────────────────────────────────────────────────────────────────
// Hidden API bypass via native-attached-thread reflection.
// Modeled on org.chickenhook.restrictionbypass.NativeReflectionBypass:
// the filtered Java methods (Class.getDeclaredField/Method) skip the
// hidden-API caller check when invoked from a thread with no app
// classloader above the JNI boundary. AttachCurrentThread gives us
// exactly that context.
// ─────────────────────────────────────────────────────────────────────────

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_HiddenApiBridge_reflectGetDeclaredField(
        JNIEnv* env, jobject, jclass cls, jstring name) {
    if (!cls || !name) return nullptr;
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    jobject gCls = env->NewGlobalRef(cls);
    jstring gName = (jstring) env->NewGlobalRef(name);

    auto fut = std::async(std::launch::async, [vm, gCls, gName]() -> jobject {
        JNIEnv* e = nullptr;
        if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
        jclass classCls = e->FindClass("java/lang/Class");
        jmethodID m = e->GetMethodID(classCls, "getDeclaredField",
                                     "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
        jobject res = nullptr;
        if (m) {
            res = e->CallObjectMethod(gCls, m, gName);
            if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); res = nullptr; }
        }
        jobject g = res ? e->NewGlobalRef(res) : nullptr;
        vm->DetachCurrentThread();
        return g;
    });
    jobject r = fut.get();
    env->DeleteGlobalRef(gCls);
    env->DeleteGlobalRef(gName);
    LOGD("reflectGetDeclaredField: %s", r ? "OK" : "null");
    return r;
}

extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_HiddenApiBridge_reflectGetDeclaredMethod(
        JNIEnv* env, jobject, jclass cls, jstring name, jobjectArray paramTypes) {
    if (!cls || !name) return nullptr;
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    jobject gCls = env->NewGlobalRef(cls);
    jstring gName = (jstring) env->NewGlobalRef(name);
    jobjectArray gParams = paramTypes ? (jobjectArray) env->NewGlobalRef(paramTypes) : nullptr;

    auto fut = std::async(std::launch::async, [vm, gCls, gName, gParams]() -> jobject {
        JNIEnv* e = nullptr;
        if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
        jclass classCls = e->FindClass("java/lang/Class");
        jmethodID m = e->GetMethodID(classCls, "getDeclaredMethod",
                                     "(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;");
        jobject res = nullptr;
        if (m) {
            res = e->CallObjectMethod(gCls, m, gName, gParams);
            if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); res = nullptr; }
        }
        jobject g = res ? e->NewGlobalRef(res) : nullptr;
        vm->DetachCurrentThread();
        return g;
    });
    jobject r = fut.get();
    env->DeleteGlobalRef(gCls);
    env->DeleteGlobalRef(gName);
    if (gParams) env->DeleteGlobalRef(gParams);
    LOGD("reflectGetDeclaredMethod: %s", r ? "OK" : "null");
    return r;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_HiddenApiBridge_reflectSetAccessible(
        JNIEnv* env, jobject, jobject member) {
    if (!member) return JNI_FALSE;
    JavaVM* vm = nullptr;
    env->GetJavaVM(&vm);
    jobject gMember = env->NewGlobalRef(member);

    auto fut = std::async(std::launch::async, [vm, gMember]() -> jboolean {
        JNIEnv* e = nullptr;
        if (vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return JNI_FALSE;
        jclass accCls = e->FindClass("java/lang/reflect/AccessibleObject");
        jmethodID m = e->GetMethodID(accCls, "setAccessible", "(Z)V");
        jboolean ok = JNI_FALSE;
        if (m) {
            e->CallVoidMethod(gMember, m, JNI_TRUE);
            if (e->ExceptionCheck()) { e->ExceptionDescribe(); e->ExceptionClear(); }
            else ok = JNI_TRUE;
        }
        vm->DetachCurrentThread();
        return ok;
    });
    jboolean r = fut.get();
    env->DeleteGlobalRef(gMember);
    LOGD("reflectSetAccessible: %s", r ? "OK" : "false");
    return r;
}

// ─── Test bridge for E2E anti-detection tests ─────────────────────────
int InstallHooks();
extern "C" int InstallAntiDetection(JNIEnv*);

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_installFileHooks(JNIEnv*, jobject) {
    return (jint)InstallHooks();   // 0 = HOOK_OK, idempotent on repeat
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_installAntiDetection(JNIEnv* env, jobject) {
    return (jint)InstallAntiDetection(env);
}

// Raw file write via syscall — bypasses our own hooks (libhookengine is
// not GOT-patched). Lets the test create a file that ShouldHidePath will
// then hide from Java-side File.exists().
extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_writeFileRaw(
        JNIEnv* env, jobject, jstring jpath, jstring jcontent) {
    if (!jpath || !jcontent) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    const char* content = env->GetStringUTFChars(jcontent, nullptr);
    if (!path || !content) {
        if (path) env->ReleaseStringUTFChars(jpath, path);
        if (content) env->ReleaseStringUTFChars(jcontent, content);
        return JNI_FALSE;
    }
    int fd = (int)syscall(__NR_openat, AT_FDCWD, path,
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    jboolean ok = JNI_FALSE;
    if (fd >= 0) {
        ssize_t want = (ssize_t)strlen(content);
        ssize_t got = (ssize_t)syscall(__NR_write, fd, content, (size_t)want);
        syscall(__NR_close, fd);
        if (got == want) ok = JNI_TRUE;
    }
    env->ReleaseStringUTFChars(jpath, path);
    env->ReleaseStringUTFChars(jcontent, content);
    LOGD("writeFileRaw(%s): %d", path, ok);
    return ok;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_NativeTestBridge_readFileRaw(
        JNIEnv* env, jobject, jstring jpath) {
    if (!jpath) return env->NewStringUTF("");
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    if (!path) return env->NewStringUTF("");
    int fd = (int)syscall(__NR_openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        env->ReleaseStringUTFChars(jpath, path);
        return env->NewStringUTF("");
    }
    char buf[256] = {0};
    ssize_t n = (ssize_t)syscall(__NR_read, fd, buf, sizeof(buf) - 1);
    syscall(__NR_close, fd);
    env->ReleaseStringUTFChars(jpath, path);
    if (n < 0) n = 0;
    buf[n] = '\0';
    return env->NewStringUTF(buf);
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_deleteFileRaw(
        JNIEnv* env, jobject, jstring jpath) {
    if (!jpath) return JNI_FALSE;
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    if (!path) return JNI_FALSE;
    int rc = (int)syscall(__NR_unlinkat, AT_FDCWD, path, 0);
    jboolean ok = (rc == 0 || errno == ENOENT) ? JNI_TRUE : JNI_FALSE;
    env->ReleaseStringUTFChars(jpath, path);
    return ok;
}

extern "C" int BinderProbe();

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_binderProbe(JNIEnv*, jobject) {
    return BinderProbe();
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_parcelRewriterReady(
        JNIEnv* env, jobject);

// ─── tnFIN probe bridge ───────────────────────────────────────────────
// Returns FromReflectedMethod(method) as a long. 0 on failure.
// Kotlin side checks: < 0x1000 = fast-JNI token, >= 0x1000 = real ArtMethod*.
extern "C" JNIEXPORT jlong JNICALL
Java_com_ims134_hookengine_NativeTestBridge_probeFromReflectedMethod(
        JNIEnv* env, jobject, jobject methodObj) {
    if (!env || !methodObj) return 0;
    jmethodID mid = env->FromReflectedMethod(methodObj);
    if (!mid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return 0;
    }
    return (jlong)(uintptr_t)mid;
}

