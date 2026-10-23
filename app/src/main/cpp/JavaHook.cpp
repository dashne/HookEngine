#include "JavaHook.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/mman.h>
#include <unistd.h>
#include <dlfcn.h>
#include <android/log.h>
#include <mutex>
#include <vector>

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct JavaHook {
    void*   art_method;
    void**  entry_slot;
    void*   original;
    void*   replacement;
    int     orig_prot;
    size_t  page_size;
    char    name[64];
};

static std::mutex            g_jh_mutex;
static std::vector<JavaHook*> g_jh_hooks;

extern "C" jint CheckHookCapability();

static void** FindEntrySlot(void* art_method) {
    if ((uintptr_t)art_method < 0x1000) return nullptr;

    void* val = *(void**)((uint8_t*)art_method + 24);
    if (!val) return nullptr;
    if ((uintptr_t)val < 0x1000) return nullptr;

    Dl_info info;
    if (!dladdr(val, &info) || !info.dli_fname) {
        LOGE("FindEntrySlot: dladdr failed for val=%p", val);
        return nullptr;
    }
    if (!strstr(info.dli_fname, "libart.so") &&
        !strstr(info.dli_fname, "libandroid_runtime.so") &&
        !strstr(info.dli_fname, "libc.so")) {
        LOGE("FindEntrySlot: refusing to hook %p (in %s) — not a native entry",
             val, info.dli_fname);
        return nullptr;
    }
    return (void**)((uint8_t*)art_method + 24);
}

static bool ReadProt(uintptr_t page, int* out_prot) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3) {
            if (s <= page && page < e) {
                int p = 0;
                if (perms[0] == 'r') p |= PROT_READ;
                if (perms[1] == 'w') p |= PROT_WRITE;
                if (perms[2] == 'x') p |= PROT_EXEC;
                *out_prot = p;
                fclose(f);
                return true;
            }
        }
    }
    fclose(f);
    return false;
}

extern "C" JavaHook* JavaHookInstall(JNIEnv* env, jobject method_obj,
                                     void* replacement, void** out_original,
                                     const char* name) {
    if (!env || !method_obj || !replacement) return nullptr;

    // Same gate as HookJavaMethod. Refuse on any ART build outside the
    // verified range instead of patching offsets that may not mean the same
    // thing. One bad offset = native crash or silent corruption.
    jint cap = CheckHookCapability();
    if (cap != HOOK_OK) {
        LOGE("JavaHookInstall[%s]: capability refused (%d) — not patching",
             name ? name : "?", (int)cap);
        return nullptr;
    }

    jmethodID mid = env->FromReflectedMethod(method_obj);
    if ((uintptr_t)mid < 0x1000) {
        LOGE("JavaHookInstall[%s]: fast-JNI intrinsic (mid=0x%lx), cannot ArtMethod-hook",
             name ? name : "?", (unsigned long)(uintptr_t)mid);
        return nullptr;
    }
    if (!mid) {
        LOGE("JavaHookInstall[%s]: FromReflectedMethod null", name ? name : "?");
        return nullptr;
    }
    void* art_method = (void*)mid;
    void** entry_slot = FindEntrySlot(art_method);
    if (!entry_slot) {
        LOGE("JavaHookInstall[%s]: no entry slot", name ? name : "?");
        return nullptr;
    }
    void* original = *entry_slot;
    if (!original) {
        LOGE("JavaHookInstall[%s]: null entry point", name ? name : "?");
        return nullptr;
    }
    if (original == replacement) {
        LOGE("JavaHookInstall[%s]: already hooked to this replacement", name ? name : "?");
        return nullptr;
    }

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)entry_slot & ~(uintptr_t)(ps - 1);
    int prot = 0;
    if (!ReadProt(page, &prot)) prot = PROT_READ | PROT_WRITE;

    if (mprotect((void*)page, (size_t)ps, prot | PROT_WRITE) != 0) {
        LOGE("JavaHookInstall[%s]: mprotect RW failed errno=%d", name ? name : "?", errno);
        return nullptr;
    }
    *entry_slot = replacement;
    __builtin___clear_cache((char*)entry_slot, (char*)entry_slot + 8);
    mprotect((void*)page, (size_t)ps, prot);

    JavaHook* h = (JavaHook*)calloc(1, sizeof(JavaHook));
    h->art_method  = art_method;
    h->entry_slot  = entry_slot;
    h->original    = original;
    h->replacement = replacement;
    h->orig_prot   = prot;
    h->page_size   = (size_t)ps;
    if (name) snprintf(h->name, sizeof(h->name), "%s", name);

    {
        std::lock_guard<std::mutex> lock(g_jh_mutex);
        g_jh_hooks.push_back(h);
    }
    if (out_original) *out_original = original;

    LOGD("JavaHookInstall[%s]: ok art_method=%p slot=%p orig=%p repl=%p",
         h->name, art_method, entry_slot, original, replacement);
    return h;
}

extern "C" HookResult JavaHookUninstall(JavaHook* hook) {
    if (!hook) return HOOK_NOT_HOOKED;

    uintptr_t page = (uintptr_t)hook->entry_slot & ~(uintptr_t)(hook->page_size - 1);
    if (mprotect((void*)page, hook->page_size, hook->orig_prot | PROT_WRITE) != 0) {
        return HOOK_MPROTECT_FAILED;
    }
    *hook->entry_slot = hook->original;
    __builtin___clear_cache((char*)hook->entry_slot, (char*)hook->entry_slot + 8);
    mprotect((void*)page, hook->page_size, hook->orig_prot);

    {
        std::lock_guard<std::mutex> lock(g_jh_mutex);
        for (auto it = g_jh_hooks.begin(); it != g_jh_hooks.end(); ++it) {
            if (*it == hook) { g_jh_hooks.erase(it); break; }
        }
    }
    LOGD("JavaHookUninstall[%s]: restored orig=%p", hook->name, hook->original);
    free(hook);
    return HOOK_OK;
}

extern "C" int JavaHookUninstallAll() {
    std::vector<JavaHook*> copy;
    {
        std::lock_guard<std::mutex> lock(g_jh_mutex);
        copy = g_jh_hooks;
    }
    int n = 0;
    for (auto* h : copy) {
        if (JavaHookUninstall(h) == HOOK_OK) n++;
    }
    return n;
}

extern "C" int JavaHookCount() {
    std::lock_guard<std::mutex> lock(g_jh_mutex);
    return (int)g_jh_hooks.size();
}