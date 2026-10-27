#include "FastJniHook.h"
#include <jni.h>
#include <android/log.h>
#include <unistd.h>
#include <sys/mman.h>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <atomic>
#include "HookRegistry.h"

extern "C" JavaVM* dualcore_get_vm();
extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);
extern "C" bool ParcelRewriter_IsNullOverridden(const char*);
extern "C" bool ParcelRewriter_FindAlias(const char*, char*, size_t);

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static std::atomic<int> g_fj_hits{0};
static bool             g_installed          = false;
static void*            g_art_method         = nullptr;
static void*            g_hooked_jit_code    = nullptr;
static void*            g_saved_quick_before = nullptr;

typedef jobject (*OrigQuickFn)(void* art_method, jstring name);
static OrigQuickFn g_orig_quick = nullptr;

static bool ReadPageProtCpp(uintptr_t page, int* out) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e; char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3) {
            if (s <= page && page < e) {
                int p = 0;
                if (perms[0] == 'r') p |= PROT_READ;
                if (perms[1] == 'w') p |= PROT_WRITE;
                if (perms[2] == 'x') p |= PROT_EXEC;
                *out = p;
                fclose(f);
                return true;
            }
        }
    }
    fclose(f);
    return false;
}

// Quick-ABI shim: dispatches with x0=ArtMethod*, x1=jstring. Called from
// either the inline hook on the JIT code, or the +32 slot (fresh callers).
// Checks override tables first; forwards to trampoline (original JIT body).
extern "C" __attribute__((visibility("hidden")))
jobject getService_quick(void* art_method, jstring name) {
    int hits = g_fj_hits.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hits <= 3) LOGD("quick shim: entered (hit #%d)", hits);

    JavaVM* vm = dualcore_get_vm();
    if (!vm) {
        LOGD("quick shim: no vm");
        if (g_orig_quick) return g_orig_quick(art_method, name);
        return nullptr;
    }
    JNIEnv* env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
        LOGD("quick shim: no env");
        if (g_orig_quick) return g_orig_quick(art_method, name);
        return nullptr;
    }
    if (!name) {
        LOGD("quick shim: name null");
        if (g_orig_quick) return g_orig_quick(art_method, name);
        return nullptr;
    }
    const char* cname = env->GetStringUTFChars(name, nullptr);
    if (!cname) {
        if (g_orig_quick) return g_orig_quick(art_method, name);
        return nullptr;
    }
    bool nullOv = ParcelRewriter_IsNullOverridden(cname);
    char alias[128];
    bool hasAlias = ParcelRewriter_FindAlias(cname, alias, sizeof(alias));

    if (nullOv) {
        LOGD("quick shim: '%s' -> null override", cname);
        env->ReleaseStringUTFChars(name, cname);
        return nullptr;
    }
    if (hasAlias && g_orig_quick) {
        LOGD("quick shim: alias '%s' -> '%s'", cname, alias);
        env->ReleaseStringUTFChars(name, cname);
        jstring jtarget = env->NewStringUTF(alias);
        if (!jtarget) return g_orig_quick(art_method, name);
        jobject r = g_orig_quick(art_method, jtarget);
        env->DeleteLocalRef(jtarget);
        return r;
    }
    env->ReleaseStringUTFChars(name, cname);
    if (g_orig_quick) return g_orig_quick(art_method, name);
    return nullptr;
}

// Two-pronged install:
//   1. Inline-hook the JIT code at `+32` — catches baked-in callers
//      (framework code that resolved the address at its own JIT time).
//   2. Patch `+32` to point at the shim directly — catches fresh callers
//      that read `+32` at their own JIT time (they get the shim address).
extern "C" int FastJniDirectInstall(JNIEnv* env, void* art_method, void* /*unused*/) {
    if (!env || !art_method) return -1;
    if ((uintptr_t)art_method < 0x1000) return -1;
    if (g_installed) return 0;

    void* jit_code = *(void**)((uint8_t*)art_method + 32);
    if (!jit_code || (uintptr_t)jit_code < 0x1000) {
        LOGE("FastJni: +32 not a valid code ptr: %p", jit_code);
        return -2;
    }
    LOGD("FastJni: art_method=%p jit_code=%p", art_method, jit_code);

    void* tramp = nullptr;
    HookResult r = InstallInlineHookEx(jit_code, (void*)getService_quick, &tramp,
                                       "SM.getService");
    if (r != HOOK_OK) {
        LOGE("FastJni: InstallInlineHookEx on JIT code failed r=%d", (int)r);
        return -3;
    }
    LOGD("FastJni: JIT code hooked, tramp=%p", tramp);

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)art_method & ~((uintptr_t)ps - 1);
    int prot = 0;
    if (ReadPageProtCpp(page, &prot)) {
        if (mprotect((void*)page, (size_t)ps, prot | PROT_WRITE) == 0) {
            g_saved_quick_before = *(void**)((uint8_t*)art_method + 32);
            *(void**)((uint8_t*)art_method + 32) = (void*)getService_quick;
            __builtin___clear_cache((char*)art_method + 32, (char*)art_method + 40);
            mprotect((void*)page, (size_t)ps, prot);
            LOGD("FastJni: +32 patched to shim");
        } else {
            LOGE("FastJni: mprotect on +32 failed errno=%d", errno);
        }
    }

    g_orig_quick      = (OrigQuickFn)tramp;
    g_art_method      = art_method;
    g_hooked_jit_code = jit_code;
    g_installed       = true;
    LOGD("FastJni: INSTALLED (JIT hook + +32 patch)");
    return 0;
}

extern "C" int FastJniRestore() {
    if (!g_installed) {
        LOGD("FastJni: restore — nothing installed");
        return -1;
    }
    if (g_hooked_jit_code) {
        UninstallInlineHook(g_hooked_jit_code);
        LOGD("FastJni: JIT code %p unhooked", g_hooked_jit_code);
    }
    if (g_art_method && g_saved_quick_before) {
        long ps = sysconf(_SC_PAGESIZE);
        uintptr_t page = (uintptr_t)g_art_method & ~((uintptr_t)ps - 1);
        int prot = 0;
        if (ReadPageProtCpp(page, &prot)) {
            if (mprotect((void*)page, (size_t)ps, prot | PROT_WRITE) == 0) {
                *(void**)((uint8_t*)g_art_method + 32) = g_saved_quick_before;
                __builtin___clear_cache((char*)g_art_method + 32, (char*)g_art_method + 40);
                mprotect((void*)page, (size_t)ps, prot);
            }
        }
    }
    g_orig_quick          = nullptr;
    g_saved_quick_before  = nullptr;
    g_art_method          = nullptr;
    g_hooked_jit_code     = nullptr;
    g_installed           = false;
    LOGD("FastJni: RESTORED");
    return 0;
}

extern "C" int  FastJniProbe()     { return g_installed ? 1 : 0; }
extern "C" int  FastJniInstall()   { return g_installed ? 0 : -1; }
extern "C" void FastJniUninstall() { FastJniRestore(); }
extern "C" int  FastJniHits()      { return g_fj_hits.load(std::memory_order_relaxed); }

extern "C" jobject FastJniJavaGetService(JNIEnv* env, jstring name) {
    if (!env || !name) return nullptr;
    jclass smCls = env->FindClass("android/os/ServiceManager");
    if (!smCls) { env->ExceptionClear(); return nullptr; }
    jmethodID mid = env->GetStaticMethodID(smCls, "getService",
                                           "(Ljava/lang/String;)Landroid/os/IBinder;");
    if (env->ExceptionCheck() || !mid) {
        env->ExceptionClear();
        env->DeleteLocalRef(smCls);
        return nullptr;
    }
    jobject result = env->CallStaticObjectMethod(smCls, mid, name);
    if (env->ExceptionCheck()) {
        LOGE("JavaGetService threw, clearing");
        env->ExceptionClear();
        result = nullptr;
    }
    env->DeleteLocalRef(smCls);
    return result;
}

extern "C" int   FastJniFlipAccNative(void*)      { return 0; }
extern "C" void* FastJniFindArtTrampoline()       { return nullptr; }