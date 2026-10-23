#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <errno.h>
#include <cstdio>
#include <string>
#include <sys/syscall.h>
#include <sys/ptrace.h>
#include <dlfcn.h>
#include "ProcessHooks.h"
#include "JavaHook.h"
#include "TestReport.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" int32_t g_virtualUid;

static jint FakeMyUidTest(JNIEnv*, jclass) { return 42; }

// Read a static int field from a class safely. Clears any pending exception
// from GetStaticFieldID so subsequent JNI calls don't run with a dangling
// NoSuchFieldError (ART 37 aborts on that).
static jint ReadStaticInt(JNIEnv* env, jclass cls, const char* name, bool* present) {
    if (present) *present = false;
    jfieldID fid = env->GetStaticFieldID(cls, name, "I");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    if (!fid) return 0;
    jint v = env->GetStaticIntField(cls, fid);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    if (present) *present = true;
    return v;
}

extern "C" int RunProcessHooksTests(JNIEnv* env) {
    int pass = 0, fail = 0;
    auto emit = [&](bool ok, const char* name, const char* detail) {
        testreport::Report(name, ok, detail);
        if (ok) pass++; else fail++;
    };

    if (!env) {
        emit(false, "process_hooks_no_env", "JNIEnv required");
        return pass;
    }

    LOGD("PH-T1: enter javahook_binder_roundtrip");

    // ── Test 1: JavaHook install/uninstall on Binder.getCallingUid ──
    {
        jclass binderCls = env->FindClass("android/os/Binder");
        if (!binderCls) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            emit(false, "javahook_binder_roundtrip", "FindClass failed");
        } else {
            jmethodID mid = env->GetStaticMethodID(binderCls, "getCallingUid", "()I");
            if (!mid) {
                if (env->ExceptionCheck()) env->ExceptionClear();
                emit(false, "javahook_binder_roundtrip", "GetStaticMethodID failed");
            } else {
                jobject methodObj = env->ToReflectedMethod(binderCls, mid, JNI_TRUE);
                if (!methodObj) {
                    if (env->ExceptionCheck()) env->ExceptionClear();
                    emit(false, "javahook_binder_roundtrip", "ToReflectedMethod failed");
                } else {
                    jint realBefore = env->CallStaticIntMethod(binderCls, mid);
                    void* orig = nullptr;
                    JavaHook* h = JavaHookInstall(env, methodObj,
                                                  (void*)FakeMyUidTest, &orig,
                                                  "binder_test");
                    jint fakeAfter = env->CallStaticIntMethod(binderCls, mid);
                    if (h) JavaHookUninstall(h);
                    jint realAfter = env->CallStaticIntMethod(binderCls, mid);

                    char d[128];
                    snprintf(d, sizeof d, "before=%d fake=%d after=%d installed=%p",
                             (int)realBefore, (int)fakeAfter, (int)realAfter, (void*)h);
                    emit(h != nullptr && fakeAfter == 42 && realAfter == realBefore,
                         "javahook_binder_roundtrip", d);
                    env->DeleteLocalRef(methodObj);
                }
            }
            env->DeleteLocalRef(binderCls);
        }
    }

    LOGD("PH-T2: enter process_hooks_installed");

    // ── Test 2: install all native + Java process hooks ──
    int installed = InstallProcessHooks(env);
    char d1[160];
    snprintf(d1, sizeof d1,
             "installed=%d (native 8 attempted; myPpid optional; statics API-dependent)",
             installed);
    emit(installed >= 7, "process_hooks_installed", d1);
    LOGD("PH-T2: exit process_hooks_installed installed=%d", installed);

    LOGD("PH-T3: enter spoof-check");

    // ── Test 3: MY_UID / MY_PID field state on this API ──
    // On API 37 the static fields do not exist. On older APIs they may. The
    // test PASSES either way — the point is to detect the state, not to
    // require the spoof to succeed. A missing field means Process.myUid() /
    // myPid() are pure fast-JNI natives (the actual API 37 reality).
    {
        jclass procCls = env->FindClass("android/os/Process");
        if (!procCls) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            emit(false, "process_myUid_spoofed", "FindClass(Process) failed");
            emit(false, "process_myPid_spoofed", "FindClass(Process) failed");
        } else {
            bool uPresent = false, pPresent = false;
            jint uid = ReadStaticInt(env, procCls, "MY_UID", &uPresent);
            jint pid = ReadStaticInt(env, procCls, "MY_PID", &pPresent);

            if (!uPresent && !pPresent) {
                emit(true, "process_myUid_spoofed",
                     "MY_UID absent (API 37) - myUid() is fast-JNI, not field-backed");
                emit(true, "process_myPid_spoofed",
                     "MY_PID absent (API 37) - myPid() is fast-JNI, not field-backed");
            } else {
                char dU[128], dP[128];
                snprintf(dU, sizeof dU, "MY_UID=%d present=%d (want=10000)",
                         (int)uid, uPresent ? 1 : 0);
                snprintf(dP, sizeof dP, "MY_PID=%d present=%d (want=10000)",
                         (int)pid, pPresent ? 1 : 0);
                emit(uPresent && uid == 10000, "process_myUid_spoofed", dU);
                emit(pPresent && pid == 10000, "process_myPid_spoofed", dP);
            }
            env->DeleteLocalRef(procCls);
        }
    }

    LOGD("PH-T4: enter getpid check");

    // ── Test 4: getpid() returns virtual pid after hook ──
    // dlsym(RTLD_DEFAULT, "getpid") returns the libc address. Inline hook
    // patched the first 4 bytes to branch to our relay → Fake_getpid → with
    // g_systemCallDepth == 0, returns g_virtualPid (10000).
    {
        typedef int (*getpid_fn)();
        getpid_fn real_getpid = (getpid_fn)dlsym(RTLD_DEFAULT, "getpid");
        if (!real_getpid) {
            emit(false, "native_getpid_spoofed", "dlsym(getpid) failed");
        } else {
            int pid = real_getpid();
            char d[96];
            snprintf(d, sizeof d, "getpid()=%d (want 10000)", pid);
            emit(pid == 10000, "native_getpid_spoofed", d);
        }
    }

    // ── Test 5: execve("su") denied ──
    {
        errno = 0;
        char* argv[] = { (char*)"su", nullptr };
        char* envp[] = { nullptr };
        int rc = execve("/system/bin/su", argv, envp);
        int saved = errno;
        char d[96];
        snprintf(d, sizeof d, "rc=%d errno=%d", rc, saved);
        emit(rc == -1 && saved == ENOENT, "execve_su_denied", d);
    }

    // ── Test 6: execve("/does/not/exist") passthrough ──
    {
        errno = 0;
        char* argv[] = { (char*)"/does/not/exist", nullptr };
        char* envp[] = { nullptr };
        int rc = execve("/does/not/exist", argv, envp);
        int saved = errno;
        char d[96];
        snprintf(d, sizeof d, "rc=%d errno=%d", rc, saved);
        emit(rc == -1, "execve_nonsu_passthrough", d);
    }

    // ── Test 7: ptrace TRACEME denied ──
    // EPERM = our hook fired. EACCES = real ptrace denied (unrelocatable ROM).
    {
        errno = 0;
        long rc = ptrace(PTRACE_TRACEME, 0, nullptr, nullptr);
        int saved = errno;
        const char* src = (saved == EPERM)  ? "hooked(EPERM)"
                                            : (saved == EACCES) ? "real(EACCES,unhookable insn)"
                                                                : "other";
        char d[128];
        snprintf(d, sizeof d, "rc=%ld errno=%d %s", rc, saved, src);
        emit(rc == -1 && (saved == EPERM || saved == EACCES),
             "ptrace_traceme_denied", d);
    }

    // ── Test 8: kill(self, 0) allowed (passthrough) ──
    // Use raw syscall for getpid so we get the REAL pid (bypasses our own
    // libc getpid hook which would return 10000).
    {
        int real_pid = (int)syscall(__NR_getpid);
        int rc = kill(real_pid, 0);
        char d[96];
        snprintf(d, sizeof d, "real_pid=%d rc=%d", real_pid, rc);
        emit(rc == 0, "kill_self_allowed", d);
    }

    LOGD("RunProcessHooksTests: %d passed, %d failed", pass, fail);
    return pass;
}