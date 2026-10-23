#include "ProcessHooks.h"
#include "JavaHook.h"
#include "HookRegistry.h"
#include <jni.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <atomic>
#include <android/log.h>
#include <bits/glibc-syscalls.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dlfcn.h>
#include "HiddenPaths.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern std::atomic<int> g_systemCallDepth;

// Hooker / registry externs — single copy each.
extern "C" HookResult   InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult   UninstallInlineHook(void*);
extern "C" void**       GetGotAddress(const char*, const char*);
extern "C" HookResult   HookGotEntryEx(void**, void*, void**, const char*, const char*, HookHandle**);
extern "C" HookResult   UnhookGotEntry(void*);


static JavaVM* g_vm = nullptr;
static int     g_virtualPid = 10000;

// ─── Process.MY_UID / MY_PID static-field spoofing ──────────────────
static jint g_orig_my_uid = -1;
static jint g_orig_my_pid = -1;
static bool g_statics_spoofed = false;

static bool WriteStaticInt(JNIEnv* env, jclass cls, jfieldID fid, jint v, const char* label) {
    if (!cls || !fid) return false;
    env->SetStaticIntField(cls, fid, v);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGE("SpoofProcessStatics: SetStaticIntField(%s) threw", label);
        return false;
    }
    return true;
}

int SpoofProcessStatics(JNIEnv* env, jint fake_uid, jint fake_pid) {
    if (!env) return -4;
    jclass proc = env->FindClass("android/os/Process");
    if (!proc) { env->ExceptionClear(); LOGE("SpoofProcessStatics: FindClass null"); return -1; }

    jfieldID uid = env->GetStaticFieldID(proc, "MY_UID", "I");
    if (!uid) { env->ExceptionClear(); LOGE("SpoofProcessStatics: no MY_UID"); env->DeleteLocalRef(proc); return -2; }
    jfieldID pid = env->GetStaticFieldID(proc, "MY_PID", "I");
    if (!pid) { env->ExceptionClear(); LOGE("SpoofProcessStatics: no MY_PID"); env->DeleteLocalRef(proc); return -2; }

    if (!g_statics_spoofed) {
        g_orig_my_uid = env->GetStaticIntField(proc, uid);
        g_orig_my_pid = env->GetStaticIntField(proc, pid);
    }

    bool okU = WriteStaticInt(env, proc, uid, fake_uid, "MY_UID");
    bool okP = WriteStaticInt(env, proc, pid, fake_pid, "MY_PID");

    jint rbU = env->GetStaticIntField(proc, uid);
    jint rbP = env->GetStaticIntField(proc, pid);
    LOGD("SpoofProcessStatics: wrote uid=%d pid=%d; readback uid=%d pid=%d (orig uid=%d pid=%d)",
         fake_uid, fake_pid, rbU, rbP, g_orig_my_uid, g_orig_my_pid);

    g_statics_spoofed = okU && okP;
    env->DeleteLocalRef(proc);
    return g_statics_spoofed ? 0 : -3;
}

void RestoreProcessStatics() {
    if (!g_statics_spoofed) return;

    JavaVM* vm = g_vm;
    if (!vm) {
        LOGE("RestoreProcessStatics: no cached JavaVM; skipping");
        g_statics_spoofed = false;
        return;
    }

    JNIEnv* env = nullptr;
    bool attached = false;
    jint r = vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (r == JNI_EDETACHED) {
        if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("RestoreProcessStatics: AttachCurrentThread failed");
            return;
        }
        attached = true;
    } else if (r != JNI_OK) {
        LOGE("RestoreProcessStatics: GetEnv failed: %d", r);
        return;
    }

    jclass proc = env->FindClass("android/os/Process");
    if (!proc) {
        env->ExceptionClear();
    } else {
        jfieldID uid = env->GetStaticFieldID(proc, "MY_UID", "I");
        jfieldID pid = env->GetStaticFieldID(proc, "MY_PID", "I");
        if (uid) WriteStaticInt(env, proc, uid, g_orig_my_uid, "MY_UID(restore)");
        if (pid) WriteStaticInt(env, proc, pid, g_orig_my_pid, "MY_PID(restore)");
        env->DeleteLocalRef(proc);
    }

    if (attached) vm->DetachCurrentThread();
    g_statics_spoofed = false;
}

// ─── Java replacement bodies ────────────────────────────────────────
// Only myPpid is ArtMethod-hookable on API 37. myUid/myPid/myTid/myTgid
// are fast-JNI (ArtMethod.data_ holds an int token, not a pointer) and
// cannot be hooked. The MY_UID / MY_PID statics are also not what the
// myXxx() accessors read — see ProcessHooksTest.processMyUidIsNative.
static jint FakeMyPpid(JNIEnv*, jclass) { return 1; }

// ─── Native originals ────────────────────────────────────────────────
typedef int  (*execve_t)(const char*, char* const[], char* const[]);
typedef int  (*execv_t)(const char*, char* const[]);
typedef int  (*execvp_t)(const char*, char* const[]);
typedef int  (*system_t)(const char*);
typedef long (*ptrace_t)(int, int, void*, void*);
typedef int  (*kill_t)(int, int);
typedef int  (*getpid_t)();
typedef int  (*getppid_t)();

static execve_t  _execve  = nullptr;
static execv_t   _execv   = nullptr;
static execvp_t  _execvp  = nullptr;
static system_t  _system  = nullptr;
static ptrace_t  _ptrace  = nullptr;
static kill_t    _kill    = nullptr;
static getpid_t  _getpid  = nullptr;
static getppid_t _getppid = nullptr;

// ─── Hook handles (populated on install, cleared on uninstall) ──────
static HookHandle* g_h_getpid  = nullptr;
static HookHandle* g_h_getppid = nullptr;
static HookHandle* g_h_execve  = nullptr;
static HookHandle* g_h_execv   = nullptr;
static HookHandle* g_h_execvp  = nullptr;
static HookHandle* g_h_system  = nullptr;
static HookHandle* g_h_ptrace  = nullptr;
static HookHandle* g_h_kill    = nullptr;

// ─── Path checks ─────────────────────────────────────────────────────
static bool IsSuPath(const char* p) {
    return HiddenPaths::IsBlockedExecPath(p);
}

extern "C" int IsSuPathForTest(const char* path) {
    return IsSuPath(path) ? 1 : 0;
}

// ─── Fake execs ──────────────────────────────────────────────────────
static int Fake_execve(const char* path, char* const argv[], char* const envp[]) {
    if (IsSuPath(path)) { LOGE("execve DENIED: %s", path); errno = ENOENT; return -1; }
    if (!_execve) { errno = ENOSYS; return -1; }
    return _execve(path, argv, envp);
}
static int Fake_execv(const char* path, char* const argv[]) {
    if (IsSuPath(path)) { errno = ENOENT; return -1; }
    if (!_execv) { errno = ENOSYS; return -1; }
    return _execv(path, argv);
}
static int Fake_execvp(const char* file, char* const argv[]) {
    if (IsSuPath(file)) { errno = ENOENT; return -1; }
    if (!_execvp) { errno = ENOSYS; return -1; }
    return _execvp(file, argv);
}
static int Fake_system(const char* cmd) {
    if (cmd && (strstr(cmd, "su ") || strcmp(cmd, "su") == 0)) {
        LOGE("system DENIED: %s", cmd);
        return -1;
    }
    if (!_system) return -1;
    return _system(cmd);
}
static long Fake_ptrace(int request, int pid, void* addr, void* data) {
    if (request == 0 /*TRACEME*/ || request == 16 /*ATTACH*/) { errno = EPERM; return -1; }
    if (!_ptrace) { errno = ENOSYS; return -1; }
    return _ptrace(request, pid, addr, data);
}
static int Fake_kill(int pid, int sig) {
    if (!_kill) { errno = ENOSYS; return -1; }
    return _kill(pid, sig);
}
static int Fake_getpid() {
    if (g_systemCallDepth.load(std::memory_order_acquire) > 0)
        return (int)syscall(__NR_getpid);
    return g_virtualPid;
}
static int Fake_getppid() { return 1; }

// ─── Install helpers ─────────────────────────────────────────────────
static bool InstallInlineHookFor(const char* symName, void* replacement,
                                 void** outOriginal, HookHandle** outHandle) {
    void* target = dlsym(RTLD_DEFAULT, symName);
    if (!target) { LOGE("InstallInlineHookFor: dlsym(%s) failed", symName); return false; }
    HookResult r = InstallInlineHookEx(target, replacement, outOriginal, symName);
    if (r != HOOK_OK && r != HOOK_ALREADY_HOOKED) return false;
    if (outHandle) *outHandle = FindHandle(target);
    return true;
}

static bool InstallGotHook(const char* libName, const char* symName,
                           void* replacement, void** outOriginal, HookHandle** outHandle) {
    void** got = GetGotAddress(libName, symName);
    if (!got) return false;
    HookHandle* h = nullptr;
    HookResult r = HookGotEntryEx(got, replacement, outOriginal, libName, symName, &h);
    if (r != HOOK_OK && r != HOOK_ALREADY_HOOKED) return false;
    if (outHandle) *outHandle = h;
    return true;
}

// ─── Installer ───────────────────────────────────────────────────────
extern "C" int InstallProcessHooks(JNIEnv* env) {
    int n = 0;
    if (!g_vm && env) env->GetJavaVM(&g_vm);

    struct Attempt {
        const char*  sym;
        void*        fn;
        void**       orig;
        HookHandle** handle;
    };
    Attempt attempts[] = {
            {"getpid",  (void*)Fake_getpid,  (void**)&_getpid,  &g_h_getpid},
            {"getppid", (void*)Fake_getppid, (void**)&_getppid, &g_h_getppid},
            {"execve",  (void*)Fake_execve,  (void**)&_execve,  &g_h_execve},
            {"execv",   (void*)Fake_execv,   (void**)&_execv,   &g_h_execv},
            {"execvp",  (void*)Fake_execvp,  (void**)&_execvp,  &g_h_execvp},
            {"system",  (void*)Fake_system,  (void**)&_system,  &g_h_system},
            {"ptrace",  (void*)Fake_ptrace,  (void**)&_ptrace,  &g_h_ptrace},
            {"kill",    (void*)Fake_kill,    (void**)&_kill,    &g_h_kill},
            {nullptr, nullptr, nullptr, nullptr}
    };

    for (int i = 0; attempts[i].sym; i++) {
        if (*attempts[i].handle) { n++; continue; }   // already installed

        // Inline FIRST — GOT only patches call sites in already-loaded libs.
        // Any caller via dlsym (or a later-loaded lib) bypasses GOT and
        // reaches the real function body; inline patches the body itself.
        if (InstallInlineHookFor(attempts[i].sym, attempts[i].fn,
                                 attempts[i].orig, attempts[i].handle)) {
            n++;
            LOGD("InstallProcessHooks: INLINE %s installed", attempts[i].sym);
            continue;
        }

        if (InstallGotHook("libc.so", attempts[i].sym, attempts[i].fn,
                           attempts[i].orig, attempts[i].handle)) {
            n++;
            LOGD("InstallProcessHooks: GOT %s installed (inline failed — dlsym callers bypass)",
                 attempts[i].sym);
            continue;
        }

        if (!strcmp(attempts[i].sym, "ptrace") &&
            InstallInlineHookFor("__ptrace", attempts[i].fn,
                                 attempts[i].orig, attempts[i].handle)) {
            n++;
            LOGD("InstallProcessHooks: INLINE __ptrace installed (alias)");
            continue;
        }

        LOGE("InstallProcessHooks: %s could not be hooked (neither inline nor GOT)",
             attempts[i].sym);
    }

    // Java hooks. Only myPpid is ArtMethod-hookable; myUid/myPid/myTid/
    // myTgid are fast-JNI and cannot be.
    if (env) {
        jclass processCls = env->FindClass("android/os/Process");
        if (processCls) {
            jmethodID m = env->GetStaticMethodID(processCls, "myPpid", "()I");
            if (m && (uintptr_t)m >= 0x1000) {
                jobject methodObj = env->ToReflectedMethod(processCls, m, JNI_TRUE);
                if (methodObj) {
                    JavaHook* h = JavaHookInstall(env, methodObj,
                                                  (void*)FakeMyPpid, nullptr, "myPpid");
                    env->DeleteLocalRef(methodObj);
                    if (h) { n++; LOGD("InstallProcessHooks: Java myPpid installed"); }
                }
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(processCls);
        }
        // Fast-JNI statics: cannot be ArtMethod-hooked; spoof the fields.
        // NOTE: this does NOT change Process.myUid() / myPid() return values —
        // those call native directly. See ProcessHooksTest.processMyUidIsNative.
        SpoofProcessStatics(env, /*fake_uid=*/10000, /*fake_pid=*/10000);
    }

    LOGD("InstallProcessHooks: %d hooks installed", n);
    return n;
}

extern "C" int UninstallProcessHooks() {
    RestoreProcessStatics();
    int n = JavaHookUninstallAll();

    auto unhookSlot = [&](HookHandle*& h, void** orig) {
        if (h) {
            HookResult r = (h->type == HT_GOT)
                           ? UnhookGotEntry(h->target)
                           : UninstallInlineHook(h->target);
            if (r == HOOK_OK || r == HOOK_NOT_HOOKED) n++;
            else LOGE("UninstallProcessHooks: %s unhook failed r=%d", h->name, (int)r);
            h = nullptr;
        }
        if (orig) *orig = nullptr;
    };

    unhookSlot(g_h_getpid,  (void**)&_getpid);
    unhookSlot(g_h_getppid, (void**)&_getppid);
    unhookSlot(g_h_execve,  (void**)&_execve);
    unhookSlot(g_h_execv,   (void**)&_execv);
    unhookSlot(g_h_execvp,  (void**)&_execvp);
    unhookSlot(g_h_system,  (void**)&_system);
    unhookSlot(g_h_ptrace,  (void**)&_ptrace);
    unhookSlot(g_h_kill,    (void**)&_kill);

    LOGD("UninstallProcessHooks: %d hooks removed", n);
    return n;
}

extern "C" int ProcessHookCount() {
    return JavaHookCount();
}