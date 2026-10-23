#include "ProcessHooks.h"
#include "JavaHook.h"
#include "HookRegistry.h"
#include <jni.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <android/log.h>
#include <bits/glibc-syscalls.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dlfcn.h>

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);

extern "C" int32_t g_virtualUid;

extern "C" void** GetGotAddress(const char*, const char*);
extern "C" HookResult HookGotEntryEx(void**, void*, void**, const char*, const char*, HookHandle**);
extern "C" void** GetGotAddress(const char*, const char*);
extern "C" HookResult HookGotEntryEx(void**, void*, void**, const char*, const char*, HookHandle**);
extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);

static int g_virtualPid = 10000;

extern "C" void SetVirtualPid(int pid) { g_virtualPid = pid; }
extern "C" int  GetVirtualPid() { return g_virtualPid; }

// ─── Java replacement bodies ────────────────────────────────────────
static jint FakeMyUid(JNIEnv*, jclass)   { return (jint)g_virtualUid; }
static jint FakeMyPid(JNIEnv*, jclass)   { return (jint)g_virtualPid; }
static jint FakeMyTid(JNIEnv*, jclass)   { return (jint)g_virtualPid; }
static jint FakeMyTgid(JNIEnv*, jclass)  { return (jint)g_virtualPid; }
static jint FakeMyPpid(JNIEnv*, jclass)  { return 1; }

// ─── Native hook handles ─────────────────────────────────────────────
static HookHandle* g_h_execve   = nullptr;
static HookHandle* g_h_execv    = nullptr;
static HookHandle* g_h_execvp   = nullptr;
static HookHandle* g_h_execl    = nullptr;
static HookHandle* g_h_execlp   = nullptr;
static HookHandle* g_h_system   = nullptr;
static HookHandle* g_h_ptrace   = nullptr;
static HookHandle* g_h_kill     = nullptr;
static HookHandle* g_h_getpid   = nullptr;
static HookHandle* g_h_getppid  = nullptr;

// Originals
typedef int (*execve_t)(const char*, char* const[], char* const[]);
typedef int (*execv_t)(const char*, char* const[]);
typedef int (*execvp_t)(const char*, char* const[]);
typedef int (*execl_t)(const char*, const char*, ...);
typedef int (*execlp_t)(const char*, const char*, ...);
typedef int (*system_t)(const char*);
typedef long (*ptrace_t)(int, int, void*, void*);
typedef int (*kill_t)(int, int);
typedef int (*getpid_t)();
typedef int (*getppid_t)();

static execve_t  _execve  = nullptr;
static execv_t   _execv   = nullptr;
static execvp_t  _execvp  = nullptr;
static execl_t   _execl   = nullptr;
static execlp_t  _execlp  = nullptr;
static system_t  _system  = nullptr;
static ptrace_t  _ptrace  = nullptr;
static kill_t    _kill    = nullptr;
static getpid_t  _getpid  = nullptr;
static getppid_t _getppid = nullptr;

// ─── Path checks ─────────────────────────────────────────────────────
static bool IsSuPath(const char* p) {
    if (!p) return false;
    static const char* deny[] = {
            "su", "/system/bin/su", "/system/xbin/su", "/sbin/su",
            "/system/sd/xbin/su", "/system/bin/failsafe/su",
            "/system/xbin/daemonsu", "/system/bin/.ext/.su",
            "/system/usr/we-need-root/su-backup", "/su/bin/su",
            "/magisk", "/sbin/.magisk", nullptr
    };
    for (int i = 0; deny[i]; i++) {
        if (strstr(p, deny[i])) return true;
    }
    return false;
}

// ─── Fake execs ──────────────────────────────────────────────────────
static int Fake_execve(const char* path, char* const argv[], char* const envp[]) {
    if (IsSuPath(path)) {
        LOGE("execve DENIED: %s", path);
        errno = ENOENT;
        return -1;
    }
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
static int Fake_execl(const char* path, const char* arg0, ...) {
    if (IsSuPath(path)) { errno = ENOENT; return -1; }
    // Fallback: check the first vararg after path — this is arg0 usually
    if (IsSuPath(arg0)) { errno = ENOENT; return -1; }
    // Not forwarding: varargs can't be forwarded safely. Return ENOENT.
    // Any real use of execl() for anything other than su is rare in Viber.
    errno = ENOENT;
    return -1;
}
static int Fake_execlp(const char* file, const char* arg0, ...) {
    if (IsSuPath(file) || IsSuPath(arg0)) { errno = ENOENT; return -1; }
    errno = ENOENT;
    return -1;
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
    // Deny PTRACE_TRACEME / PTRACE_ATTACH so anti-debug checks think ptrace is blocked
    // but a legitimate ptrace (debugger attached) can't attach either — this is
    // exactly what a locked-down device looks like.
    if (request == 0 /*PTRACE_TRACEME*/ || request == 16 /*PTRACE_ATTACH*/) {
        errno = EPERM;
        return -1;
    }
    if (!_ptrace) { errno = ENOSYS; return -1; }
    return _ptrace(request, pid, addr, data);
}

static int Fake_kill(int pid, int sig) {
    // Pass all signals through. Blocking kill() breaks legitimate code
    // (JIT watchdog, ART GC threads, timeout handlers) and provides no
    // security benefit — Viber's anti-tamper does not test kill behavior.
    // The hook exists so ProcessHooks reflects the full kxqp surface; it
    // can be tightened later if a specific app requires it.
    if (!_kill) { errno = ENOSYS; return -1; }
    return _kill(pid, sig);
}

static int Fake_getpid() {
    if (!_getpid) { errno = ENOSYS; return -1; }
    // In some contexts (system call depth), we need the real pid.
    // Otherwise return fake. For now, always fake.
    return g_virtualPid;
}

static int Fake_getppid() {
    return 1;
}

// ─── Installer ───────────────────────────────────────────────────────
static bool InstallGotHook(const char* libName, const char* symName,
                           void* replacement, void** outOriginal) {
    void** got = GetGotAddress(libName, symName);
    if (!got) return false;
    HookHandle* h = nullptr;
    HookResult r = HookGotEntryEx(got, replacement, outOriginal,
                                  libName, symName, &h);
    return (r == HOOK_OK || r == HOOK_ALREADY_HOOKED);
}
static bool InstallInlineHookFor(const char* symName, void* replacement, void** outOriginal) {
    void* target = dlsym(RTLD_DEFAULT, symName);
    if (!target) {
        LOGE("InstallInlineHookFor: dlsym(%s) failed", symName);
        return false;
    }
    HookResult r = InstallInlineHookEx(target, replacement, outOriginal, symName);
    return r == HOOK_OK || r == HOOK_ALREADY_HOOKED;
}

extern "C" int InstallProcessHooks(JNIEnv* env) {
    int n = 0;

    // Native hooks: try GOT first (cheap, per-library), then inline the libc
    // symbol itself. Inline is what actually catches callers in OTHER libraries
    // (our own module's PLT, libart's, etc.) — GOT-only is insufficient.
    struct { const char* sym; void* fn; void** orig; } native[] = {
            {"getpid",  (void*)Fake_getpid,  (void**)&_getpid},
            {"getppid", (void*)Fake_getppid, (void**)&_getppid},
            {"execve",  (void*)Fake_execve,  (void**)&_execve},
            {"execv",   (void*)Fake_execv,   (void**)&_execv},
            {"execvp",  (void*)Fake_execvp,  (void**)&_execvp},
            {"system",  (void*)Fake_system,  (void**)&_system},
            {"ptrace",  (void*)Fake_ptrace,  (void**)&_ptrace},
            {"kill",    (void*)Fake_kill,    (void**)&_kill},
            {nullptr, nullptr, nullptr}
    };

    for (int i = 0; native[i].sym; i++) {
        if (*native[i].orig) { n++; continue; }   // already installed

        // Inline FIRST. It patches the function body, which is what dlsym
        // returns and what any caller outside libc's own PLT hits. GOT-only
        // coverage is invisible to dlsym callers and our own module — that is
        // why the previous version reported installed=8 yet getpid returned
        // the real pid (GOT succeeded, inline was skipped, body unpatched).
        if (InstallInlineHookFor(native[i].sym, native[i].fn, native[i].orig)) {
            n++;
            LOGD("InstallProcessHooks: INLINE %s installed", native[i].sym);
            continue;
        }

        // Inline failed (unrelocatable first insn, or symbol absent from dlsym).
        // Fall back to GOT — partial coverage, in-library callers only.
        if (InstallGotHook("libc.so", native[i].sym, native[i].fn, native[i].orig)) {
            n++;
            LOGD("InstallProcessHooks: GOT %s installed (inline failed — dlsym callers bypass)",
                 native[i].sym);
            continue;
        }

        // ptrace on some ROMs: the named stub is unrelocatable, the real one
        // is __ptrace.
        if (!strcmp(native[i].sym, "ptrace") &&
            InstallInlineHookFor("__ptrace", native[i].fn, native[i].orig)) {
            n++;
            LOGD("InstallProcessHooks: INLINE __ptrace installed (alias)");
            continue;
        }

        LOGE("InstallProcessHooks: %s could not be hooked (neither inline nor GOT)",
             native[i].sym);
    }

    // Java hooks — only hookable methods. Process.myUid/myPid/myTid are
    // fast-JNI with a small int token at ArtMethod.data_ (offset 24), which
    // we cannot hook without ART-internal surgery. Skip them here; they need
    // static-field modification, tracked in Batch 2.
    if (env) {
        jclass processCls = env->FindClass("android/os/Process");
        if (processCls) {
            const char* skip[] = {"myUid", "myPid", "myTid", "myTgid", nullptr};
            struct { const char* name; const char* sig; void* fn; } jh[] = {
                    {"myUid",  "()I", (void*)FakeMyUid},
                    {"myPid",  "()I", (void*)FakeMyPid},
                    {"myTid",  "()I", (void*)FakeMyTid},
                    {"myTgid", "()I", (void*)FakeMyTgid},
                    {"myPpid", "()I", (void*)FakeMyPpid},
            };
            for (auto& e : jh) {
                bool skip_this = false;
                for (int k = 0; skip[k]; k++) if (!strcmp(e.name, skip[k])) skip_this = true;
                if (skip_this) {
                    LOGD("InstallProcessHooks: %s is a cached static (fast-JNI), deferring to Batch 2",
                         e.name);
                    continue;
                }
                jmethodID m = env->GetStaticMethodID(processCls, e.name, e.sig);
                if (!m) { if (env->ExceptionCheck()) env->ExceptionClear(); continue; }
                if ((uintptr_t)m < 0x1000) {
                    LOGD("InstallProcessHooks: %s is fast-JNI, skipping", e.name);
                    continue;
                }
                jobject methodObj = env->ToReflectedMethod(processCls, m, JNI_TRUE);
                if (!methodObj) { if (env->ExceptionCheck()) env->ExceptionClear(); continue; }
                JavaHook* h = JavaHookInstall(env, methodObj, e.fn, nullptr, e.name);
                env->DeleteLocalRef(methodObj);
                if (h) { n++; LOGD("InstallProcessHooks: Java %s installed", e.name); }
            }
            env->DeleteLocalRef(processCls);
        }
    }

    LOGD("InstallProcessHooks: %d hooks installed", n);
    return n;
}

extern "C" int UninstallProcessHooks() {
    int n = JavaHookUninstallAll();

    auto unhook = [&](HookHandle*& h) {
        if (h) { h = nullptr; n++; }
    };
    unhook(g_h_execve);
    unhook(g_h_execv);
    unhook(g_h_execvp);
    unhook(g_h_execl);
    unhook(g_h_execlp);
    unhook(g_h_system);
    unhook(g_h_ptrace);
    unhook(g_h_kill);
    unhook(g_h_getpid);
    unhook(g_h_getppid);

    LOGD("UninstallProcessHooks: %d hooks removed", n);
    return n;
}

extern "C" int ProcessHookCount() {
    return JavaHookCount();
}