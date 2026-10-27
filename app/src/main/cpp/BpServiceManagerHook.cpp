// BpServiceManagerHook.cpp
// Feature 3.4 Path A — Session 51c.
//
// Attempts to intercept framework-cached service lookups at the C++
// BpServiceManager layer, one level above IPCThreadState::transact.
//
// Background (Session 51b probe result):
//   - First lookup of a name:      sCache miss → ServiceManagerProxy
//                                    → JNI → BpServiceManager::getService
//                                    → transact (both hooks fire)
//   - Subsequent lookup of a name: sCache HIT → returns cached IBinder
//                                    immediately, never touches
//                                    ServiceManagerProxy or BpServiceManager
//
// Conclusion (proven, not assumed): cache-hits do NOT reach
// BpServiceManager::getService — the cache is *above* the C++ IPC layer,
// by design. This module proves the C++ hook works on cache-miss lookups
// so we can document the finding concretely, and so future sessions have
// a wired, verified entry point if the cache is ever bypassed or
// reconfigured.
//
// Symbol (Session 51c probe, from real dynsym on MIUI/API 37):
//   _ZN7android2os16BpServiceManager10getServiceERKNSt3__112basic_stringIcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE
// Signature: void getService(const std::string& name, sp<IBinder>* out)
//   x0 = this, x1 = &name (std::string), x2 = out (sp<IBinder>*)
//
// Note the namespace is android::os (not android) and the arg is
// std::__1::basic_string (not String16) — this ROM has migrated
// BpServiceManager to std::string. Return is void with out-param.
//
// v1 behavior: count + forward. No name parsing yet — the test asserts
// only that the hook fires on cache-miss lookups (which reach this layer)
// and does NOT assert anything about cache-hits (which cannot reach this
// layer, definitively). Feature 3.4's cached-service gap remains open by
// proof, not by omission.

#include "BpServiceManagerHook.h"
#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <link.h>
#include "HookRegistry.h"

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static std::atomic<int> g_bpsm_hits{0};

// void getService(const std::string& name, sp<IBinder>* out)
// AAPCS64: this=x0, &name=x1, out=x2. Return void.
typedef void (*BpGetServiceFn)(void* self, const void* name, void* out);
typedef void (*BpCheckServiceFn)(void* self, const void* name, void* out);

static BpGetServiceFn   _orig_getService   = nullptr;
static BpCheckServiceFn _orig_checkService = nullptr;
static void* g_hook_getService   = nullptr;
static void* g_hook_checkService = nullptr;

extern "C" __attribute__((visibility("hidden")))
void bpsm_getService_shim(void* self, const void* name, void* out) {
    g_bpsm_hits.fetch_add(1, std::memory_order_relaxed);
    if (_orig_getService) _orig_getService(self, name, out);
}

extern "C" __attribute__((visibility("hidden")))
void bpsm_checkService_shim(void* self, const void* name, void* out) {
    g_bpsm_hits.fetch_add(1, std::memory_order_relaxed);
    if (_orig_checkService) _orig_checkService(self, name, out);
}

// ─── Dynsym scanner ──────────────────────────────────────────────────
// Same shape as ParcelRewriter's ScanCb, but matches exact symbol names
// (not substrings) so we hook precisely one function.

struct ScanCtx { const char* lib; const char* exact; void* result; };

static int ScanCb(struct dl_phdr_info* info, size_t, void* data) {
    ScanCtx* c = (ScanCtx*)data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, c->lib)) return 0;
    ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return 1;
    const char* strtab = nullptr; ElfW(Sym)* symtab = nullptr;
    size_t strsz = 0; ElfW(Addr) gnuHash = 0, sysvHash = 0;
    for (ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB:   strtab   = (const char*)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_SYMTAB:   symtab   = (ElfW(Sym)*)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_STRSZ:    strsz    = d->d_un.d_val; break;
            case DT_HASH:     sysvHash = info->dlpi_addr + d->d_un.d_ptr; break;
            case DT_GNU_HASH: gnuHash  = info->dlpi_addr + d->d_un.d_ptr; break;
        }
    }
    if (!strtab || !symtab || !strsz) return 1;
    uint32_t count = 0;
    if (gnuHash) {
        uint32_t* h = (uint32_t*)gnuHash;
        uint32_t nb = h[0], so = h[1], bs = h[2];
        if (!nb || !bs) return 1;
        uintptr_t p = (uintptr_t)h + 16 + (uintptr_t)bs * 8;
        uint32_t* bkts = (uint32_t*)p; p += (uintptr_t)nb * 4;
        uint32_t* chain = (uint32_t*)p;
        uint32_t mx = 0;
        for (uint32_t i = 0; i < nb; i++) if (bkts[i] > mx) mx = bkts[i];
        if (mx < so) count = so;
        else { uint32_t i = mx; while (i < so + 1000000 && !(chain[i - so] & 1)) i++; count = i + 1; }
    } else if (sysvHash) count = ((uint32_t*)sysvHash)[1];
    if (count == 0 || count > 500000) return 1;
    for (uint32_t i = 1; i < count; i++) {
        if (symtab[i].st_name == 0 || symtab[i].st_name >= strsz) continue;
        if (!symtab[i].st_value) continue;
        const char* name = strtab + symtab[i].st_name;
        if (strcmp(name, c->exact) == 0) {
            c->result = (void*)(info->dlpi_addr + symtab[i].st_value);
            return 1;
        }
    }
    return 1;
}

static void* findExact(const char* exact) {
    ScanCtx c = { "libbinder.so", exact, nullptr };
    dl_iterate_phdr(ScanCb, &c);
    return c.result;
}

// Probe result from Session 51c run — hard-coded for post-fix clarity.
static constexpr const char* kGetServiceSym =
        "_ZN7android2os16BpServiceManager10getServiceERKNSt3__112basic_string"
        "IcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE";
static constexpr const char* kCheckServiceSym =
        "_ZN7android2os16BpServiceManager12checkServiceERKNSt3__112basic_string"
        "IcNS2_11char_traitsIcEENS2_9allocatorIcEEEEPNS_2spINS_7IBinderEEE";

extern "C" int BpSmProbe() {
    void* gs = findExact(kGetServiceSym);
    void* cs = findExact(kCheckServiceSym);
    LOGD("BpSM probe: getService=%p checkService=%p", gs, cs);
    return (gs ? 1 : 0) + (cs ? 1 : 0);
}

extern "C" int BpSmInstall() {
    int installed = 0;

    if (!g_hook_getService) {
        void* target = findExact(kGetServiceSym);
        if (target) {
            void* unused = nullptr;
            HookResult r = InstallInlineHookEx(
                    target, (void*)bpsm_getService_shim,
                    (void**)&_orig_getService, "BpServiceManager::getService");
            if (r == HOOK_OK) {
                g_hook_getService = target;
                installed++;
                LOGD("BpSM install: getService hooked at %p", target);
            } else {
                LOGE("BpSM install: getService InstallInlineHookEx r=%d", (int)r);
            }
        } else {
            LOGE("BpSM install: getService symbol not found");
        }
    } else {
        installed++;
    }

    if (!g_hook_checkService) {
        void* target = findExact(kCheckServiceSym);
        if (target) {
            void* unused = nullptr;
            HookResult r = InstallInlineHookEx(
                    target, (void*)bpsm_checkService_shim,
                    (void**)&_orig_checkService, "BpServiceManager::checkService");
            if (r == HOOK_OK) {
                g_hook_checkService = target;
                installed++;
                LOGD("BpSM install: checkService hooked at %p", target);
            } else {
                LOGE("BpSM install: checkService InstallInlineHookEx r=%d", (int)r);
            }
        } else {
            LOGE("BpSM install: checkService symbol not found");
        }
    } else {
        installed++;
    }

    LOGD("BpSM install: %d hook(s) active", installed);
    return installed > 0 ? 0 : -1;
}

extern "C" void BpSmUninstall() {
    if (g_hook_getService) {
        UninstallInlineHook(g_hook_getService);
        g_hook_getService = nullptr;
        _orig_getService = nullptr;
    }
    if (g_hook_checkService) {
        UninstallInlineHook(g_hook_checkService);
        g_hook_checkService = nullptr;
        _orig_checkService = nullptr;
    }
    LOGD("BpSM: uninstalled");
}

extern "C" int BpSmCount() {
    return g_bpsm_hits.load(std::memory_order_relaxed);
}

// ─── Feature 3.4 Path B — fast-JNI native implementation hook ─────
extern "C" int  FastJniProbe();
extern "C" int  FastJniInstall();
extern "C" void FastJniUninstall();
extern "C" int  FastJniHits();

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_fastJniProbe(JNIEnv*, jobject) {
    return (jint)FastJniProbe();
}
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_fastJniInstall(JNIEnv*, jobject) {
    return (jint)FastJniInstall();
}
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_fastJniUninstall(JNIEnv*, jobject) {
    FastJniUninstall();
}
extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_fastJniHits(JNIEnv*, jobject) {
    return (jint)FastJniHits();
}