// BinderTransactHook.cpp
//
// Hooks android::IPCThreadState::transact. Caller thread does only pure C++.
// Fresh thread does JNI.
//
// Session 59 — added CP diag for IContentProvider.call() transacts, to
// capture the real request/reply byte layout for Settings.Global.getInt
// (Feature 5.10, settings-path). Same diagnostic pattern as SM diag[n].

#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <future>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <vector>
#include <mutex>
#include <cstdio>
#include "HookRegistry.h"

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);
extern "C" bool ParcelRewriter_Init(JNIEnv* env);
extern "C" bool ParcelRewriter_MatchesRequest(int code, jlong dataPtr);
extern "C" bool ParcelRewriter_TryRewriteCallerIdentity(jlong dataPtr, int code);
extern "C" void ParcelRewriter_TryRewriteServiceManagerLookup(jlong dataPtr, int code);
extern "C" void ParcelRewriter_AfterServiceManagerLookup(jlong dataPtr, jlong replyPtr, int code);

// Feature 3.7 — synthetic service registry intercepts
extern "C" void SyntheticServiceRegistry_PreIntercept(jlong dataPtr, int code);
extern "C" void SyntheticServiceRegistry_PostIntercept(jlong replyPtr);

extern "C" const uint8_t* ParcelRewriter_GetData(jlong parcelPtr);
extern "C" size_t         ParcelRewriter_GetSize(jlong parcelPtr);
extern "C" size_t         ParcelRewriter_GetPos(jlong parcelPtr);

// ─── Dynsym scan ─────────────────────────────────────────────────────
struct ScanCtx { const char* lib; const char* needle; void* result; };

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
        const char* name = strtab + symtab[i].st_name;
        if (strcmp(name, c->needle) == 0 && symtab[i].st_value) {
            c->result = (void*)(info->dlpi_addr + symtab[i].st_value);
            return 1;
        }
    }
    return 1;
}

static void* ScanDynsym(const char* libSubstr, const char* needle) {
    ScanCtx ctx = { libSubstr, needle, nullptr };
    dl_iterate_phdr(ScanCb, &ctx);
    return ctx.result;
}

// ─── State ───────────────────────────────────────────────────────────
typedef int32_t (*ipc_transact_t)(void*, int32_t, uint32_t,
                                  const void*, void*, uint32_t);
static ipc_transact_t _orig_transact = nullptr;
static void*          g_target_fn    = nullptr;
static HookHandle*    g_hook_handle  = nullptr;

static std::atomic<int> g_transact_count{0};
static std::atomic<int> g_rewrite_count{0};
static std::atomic<int> g_last_code{0};
static std::atomic<int> g_inside{0};

static std::atomic<int> g_diag_sm_handle0{0};
static std::atomic<int> g_diag_code12{0};
static std::atomic<int> g_diag_cp{0};       // Session 59 — CP diag

static std::mutex             g_codes_mutex;
static std::vector<int>       g_codes;
static std::atomic<int>       g_rewrite_effective_count{0};

static std::mutex             g_reply_mutex;
static std::vector<uint8_t>   g_last_code36_reply;

static void PushCode(int code) {
    std::lock_guard<std::mutex> lk(g_codes_mutex);
    if (g_codes.size() < 10000) g_codes.push_back(code);
}

static JavaVM*   g_vm                    = nullptr;
static jclass    g_rewriter_class        = nullptr;
static jmethodID g_rewriter_rewriteReply = nullptr;

static constexpr int kCodeGetPackageInfo       = 3;
static constexpr int kCodeGetInstalledPackages = 36;

static void HexDump(const char* label, const uint8_t* buf, size_t size, size_t cap) {
    if (!buf || size == 0) { LOGD("%s: empty", label); return; }
    size_t n = size < cap ? size : cap;
    char line[1024];
    int pos = snprintf(line, sizeof(line), "%s: size=%zu first%zu=", label, size, n);
    for (size_t i = 0; i < n && pos < (int)sizeof(line) - 3; i++) {
        pos += snprintf(line + pos, sizeof(line) - pos, "%02x", buf[i]);
        if ((i & 3) == 3) line[pos++] = ' ';
    }
    LOGD("%s", line);
}

// UTF-16 byte-search for an iface token string.
static bool ParcelContainsIfaceToken(const uint8_t* data, size_t size,
                                     const char16_t* token, size_t tokenLen) {
    if (!data || size < tokenLen * 2) return false;
    size_t byteLen = tokenLen * 2;
    size_t limit = size - byteLen;
    for (size_t off = 0; off <= limit; off += 2) {
        if (memcmp(data + off, token, byteLen) == 0) return true;
    }
    return false;
}

static bool ParcelLooksLikeContentProviderCall(const uint8_t* data, size_t size) {
    static const char16_t kIface[] = u"android.content.IContentProvider";
    constexpr size_t kIfaceLen = sizeof(kIface)/sizeof(char16_t) - 1;
    return ParcelContainsIfaceToken(data, size, kIface, kIfaceLen);
}

static void RewriteOnFreshThread(JNIEnv* env, int code, jlong dataPtr, jlong replyPtr) {
    if (!env || !dataPtr || !replyPtr || !g_rewriter_class) return;

    const uint8_t* callerReplyBuf = ParcelRewriter_GetData(replyPtr);
    size_t callerReplySize = ParcelRewriter_GetSize(replyPtr);
    size_t callerReplyPos  = ParcelRewriter_GetPos(replyPtr);
    if (!callerReplyBuf || callerReplySize == 0) {
        LOGE("RewriteOnFreshThread: caller reply empty");
        return;
    }

    HexDump(code == kCodeGetInstalledPackages ? "RewriteOnFreshThread: code 36 in" : "RewriteOnFreshThread: code 3 in",
            callerReplyBuf, callerReplySize, 32);
    if (code == kCodeGetInstalledPackages && callerReplySize <= 256 * 1024) {
        std::lock_guard<std::mutex> lk(g_reply_mutex);
        g_last_code36_reply.assign(callerReplyBuf, callerReplyBuf + callerReplySize);
    }
    LOGD("RewriteOnFreshThread: code=%d size=%zu pos=%zu",
         code, callerReplySize, callerReplyPos);

    if (code == kCodeGetInstalledPackages) {
        LOGD("RewriteOnFreshThread: code 36 skipped (parsing not yet safe)");
        return;
    }

    jclass cls = env->FindClass("android/os/Parcel");
    if (!cls) { env->ExceptionClear(); return; }
    jmethodID obtainM = env->GetStaticMethodID(cls, "obtain", "()Landroid/os/Parcel;");
    jobject wReplyFresh = obtainM ? env->CallStaticObjectMethod(cls, obtainM) : nullptr;
    jfieldID fNativePtr = wReplyFresh ? env->GetFieldID(cls, "mNativePtr", "J") : nullptr;
    jlong freshPtr = fNativePtr ? env->GetLongField(wReplyFresh, fNativePtr) : 0;
    env->DeleteLocalRef(cls);
    if (!wReplyFresh || !freshPtr) {
        env->ExceptionClear();
        if (wReplyFresh) env->DeleteLocalRef(wReplyFresh);
        return;
    }

    uint8_t* newBuf = (uint8_t*)malloc(callerReplySize);
    if (!newBuf) { env->DeleteLocalRef(wReplyFresh); return; }
    memcpy(newBuf, callerReplyBuf, callerReplySize);

    void** freshDataField = (void**)((uint8_t*)freshPtr + 8);
    if (*freshDataField) free(*freshDataField);
    *freshDataField = newBuf;
    *(size_t*)((uint8_t*)freshPtr + 16) = callerReplySize;
    *(size_t*)((uint8_t*)freshPtr + 24) = callerReplySize;
    *(size_t*)((uint8_t*)freshPtr + 32) = 0;

    *(int32_t*)((uint8_t*)freshPtr + 0)  = *(int32_t*)((uint8_t*)replyPtr + 0);
    *(uint8_t*)((uint8_t*)freshPtr + 81) = *(uint8_t*)((uint8_t*)replyPtr + 81);
    *(uint8_t*)((uint8_t*)freshPtr + 82) = *(uint8_t*)((uint8_t*)replyPtr + 82);
    *(int32_t*)((uint8_t*)freshPtr + 88) = *(int32_t*)((uint8_t*)replyPtr + 88);

    jlong origData = 0;
    jclass dcls = env->FindClass("android/os/Parcel");
    jmethodID dObtainM = dcls ? env->GetStaticMethodID(dcls, "obtain", "()Landroid/os/Parcel;") : nullptr;
    jobject wData = dObtainM ? env->CallStaticObjectMethod(dcls, dObtainM) : nullptr;
    jfieldID dNativePtr = wData ? env->GetFieldID(dcls, "mNativePtr", "J") : nullptr;
    if (wData && dNativePtr) {
        origData = env->GetLongField(wData, dNativePtr);
        env->SetLongField(wData, dNativePtr, dataPtr);
    }
    env->DeleteLocalRef(dcls);
    if (!wData) { env->ExceptionClear(); env->DeleteLocalRef(wReplyFresh); return; }

    bool rewriterOk = false;
    if (g_rewriter_rewriteReply) {
        env->CallStaticVoidMethod(g_rewriter_class, g_rewriter_rewriteReply,
                                  code, wData, wReplyFresh);
        if (env->ExceptionCheck()) {
            LOGE("RewriteOnFreshThread: rewriter threw code=%d — skipping swap", code);
            env->ExceptionClear();
            rewriterOk = false;
        } else {
            rewriterOk = true;
        }
    }

    const uint8_t* newData = ParcelRewriter_GetData(freshPtr);
    size_t newSize = ParcelRewriter_GetSize(freshPtr);

    bool swapped = false;
    if (rewriterOk && newData && newSize > 0) {
        *(void**)((uint8_t*)replyPtr + 8) = (void*)newData;
        *(size_t*)((uint8_t*)replyPtr + 16) = newSize;
        *(size_t*)((uint8_t*)replyPtr + 24) = newSize;
        *(size_t*)((uint8_t*)replyPtr + 32) = callerReplyPos;

        *freshDataField = nullptr;
        *(size_t*)((uint8_t*)freshPtr + 16) = 0;
        *(size_t*)((uint8_t*)freshPtr + 24) = 0;
        *(size_t*)((uint8_t*)freshPtr + 32) = 0;
        swapped = true;
    }

    jclass rcls = env->FindClass("android/os/Parcel");
    if (rcls) {
        jmethodID recycleM = env->GetMethodID(rcls, "recycle", "()V");
        if (recycleM) env->CallVoidMethod(wReplyFresh, recycleM);
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(rcls);
    }
    env->DeleteLocalRef(wReplyFresh);

    if (wData) {
        env->SetLongField(wData, dNativePtr, origData);
        jclass rcls2 = env->FindClass("android/os/Parcel");
        if (rcls2) {
            jmethodID recycleM = env->GetMethodID(rcls2, "recycle", "()V");
            if (recycleM) env->CallVoidMethod(wData, recycleM);
            if (env->ExceptionCheck()) env->ExceptionClear();
            env->DeleteLocalRef(rcls2);
        }
        env->DeleteLocalRef(wData);
    }

    if (swapped) {
        g_rewrite_effective_count.fetch_add(1, std::memory_order_relaxed);
        LOGD("RewriteOnFreshThread: swapped reply (%zu -> %zu bytes) pos=%zu",
             callerReplySize, newSize, callerReplyPos);
    } else {
        LOGD("RewriteOnFreshThread: no swap (rewriterOk=%d newData=%p newSize=%zu) "
             "— caller sees original reply",
             rewriterOk ? 1 : 0, (const void*)newData, newSize);
    }
}

extern "C" int32_t Hook_Transact(void* self, int32_t handle, uint32_t code,
                                 const void* data, void* reply, uint32_t flags) {
    g_transact_count.fetch_add(1, std::memory_order_relaxed);
    g_last_code.store((int)code, std::memory_order_relaxed);

    PushCode((int)code);

    // Diagnostic dump — first 20 SM (handle==0) or code 1/2 transacts
    bool want_handle0 = (handle == 0);
    bool want_code12  = (code == 1 || code == 2);
    if (want_handle0 || want_code12) {
        int idx0 = want_handle0 ? g_diag_sm_handle0.fetch_add(1) : -1;
        int idx1 = want_code12  ? g_diag_code12.fetch_add(1)    : -1;
        int idx  = (idx0 >= 0) ? idx0 : idx1;
        if (idx < 20) {
            size_t dataSize = data ? ParcelRewriter_GetSize((jlong)data) : 0;
            const uint8_t* p = data ? ParcelRewriter_GetData((jlong)data) : nullptr;
            char hex[96] = {0};
            int hp = 0;
            int nbytes = 0;
            if (p && dataSize > 0) {
                nbytes = (int)(dataSize < 32 ? dataSize : 32);
                for (int i = 0; i < nbytes && hp < 90; i++) {
                    hp += snprintf(hex + hp, sizeof(hex) - hp, "%02x", p[i]);
                }
            }
            LOGD("SM diag[%d]: handle=%d code=%u flags=0x%x dataSize=%zu hex=%s",
                 idx, (int)handle, code, flags, dataSize, hex);
        }
    }

    // Session 59 — Feature 5.10 CP diag.
    // Detect IContentProvider.call() transacts and dump request + reply
    // hex so we can identify the Bundle reply layout for the settings
    // Bundle rewriter (iteration B).
    bool cpDiag = false;
    if (data && !(flags & 1)) {
        int idx = g_diag_cp.load();
        if (idx < 10) {
            const uint8_t* p = ParcelRewriter_GetData((jlong)data);
            size_t sz = ParcelRewriter_GetSize((jlong)data);
            if (p && sz > 0 && ParcelLooksLikeContentProviderCall(p, sz)) {
                g_diag_cp.fetch_add(1);
                LOGD("CP diag[%d]: handle=%d code=%u flags=0x%x reqSize=%zu",
                     idx, (int)handle, code, flags, sz);
                HexDump("CP diag req", p, sz, 256);
                cpDiag = true;
            }
        }
    }

    if (!_orig_transact) return -1;

    // Session 23 — Wall B: rewrite IStorageManager callingPackage BEFORE
    // forwarding. Session 27 — Wall C2: rewrite IContentProvider
    // AttributionSource for the same reason.
    if (!(flags & 1) && data) {
        ParcelRewriter_TryRewriteCallerIdentity((jlong)data, (int)code);
    }

    // Feature 3.4 — ServiceManager lookup interception.
    if (!(flags & 1) && data && handle == 0) {
        // Feature 3.7 — pre-intercept BEFORE alias rewrite, so we can detect
        // the synthetic name before it gets rewritten to "activity".
        SyntheticServiceRegistry_PreIntercept((jlong)data, (int)code);
        // Feature 3.4 — alias rewrite runs second (rewrites synthetic -> activity)
        ParcelRewriter_TryRewriteServiceManagerLookup((jlong)data, (int)code);
    }

    bool matched = false;
    if (!(flags & 1) && data && reply &&
        (code == (uint32_t)kCodeGetPackageInfo ||
         code == (uint32_t)kCodeGetInstalledPackages)) {
        matched = ParcelRewriter_MatchesRequest((int)code, (jlong)data);
    }

    int32_t result = _orig_transact(self, handle, code, data, reply, flags);

    // Session 59 — CP diag reply dump (after _orig, before any early return).
    if (cpDiag && reply) {
        const uint8_t* rp = ParcelRewriter_GetData((jlong)reply);
        size_t rsz = ParcelRewriter_GetSize((jlong)reply);
        LOGD("CP diag reply: replySize=%zu result=%d", rsz, result);
        HexDump("CP diag reply", rp, rsz, 256);
    }

    // Feature 3.7 — post-intercept swaps reply flat_binder_object for synthetic
    if (result == 0 && !(flags & 1) && handle == 0 && reply) {
        SyntheticServiceRegistry_PostIntercept((jlong)reply);
    }

    if (result != 0 || !matched || !reply) return result;

    g_rewrite_count.fetch_add(1, std::memory_order_relaxed);

    if (g_inside.load(std::memory_order_acquire) > 0) return result;
    g_inside.fetch_add(1, std::memory_order_acq_rel);

    try {
        auto fut = std::async(std::launch::async,
                              [code, data, reply]() {
                                  JNIEnv* env = nullptr;
                                  bool attached = false;
                                  if (!g_vm) return;
                                  if (g_vm->GetEnv((void**)&env, JNI_VERSION_1_6) != JNI_OK) {
                                      if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
                                      attached = true;
                                  }
                                  RewriteOnFreshThread(env, (int)code, (jlong)data, (jlong)reply);
                                  if (attached) g_vm->DetachCurrentThread();
                              });
        fut.wait();
    } catch (...) {
        LOGE("BinderTransactHook: async dispatch threw");
    }
    g_inside.fetch_sub(1, std::memory_order_acq_rel);
    return result;
}

static constexpr const char* kTransactSym =
        "_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j";

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_installNative(JNIEnv* env, jobject) {
    if (g_hook_handle) return 0;
    if (!env) return -1;
    if (!g_vm) env->GetJavaVM(&g_vm);

    if (!g_rewriter_class) {
        jclass local = env->FindClass("com/ims134/hookengine/ParcelRewriter");
        if (env->ExceptionCheck() || !local) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("BinderTransactHook: FindClass(ParcelRewriter) failed");
            return -3;
        }
        g_rewriter_class = (jclass)env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
        if (!g_rewriter_class) return -3;
        g_rewriter_rewriteReply = env->GetStaticMethodID(
                g_rewriter_class, "rewriteReply",
                "(ILandroid/os/Parcel;Landroid/os/Parcel;)V");
        if (env->ExceptionCheck() || !g_rewriter_rewriteReply) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("BinderTransactHook: GetStaticMethodID(rewriteReply) failed");
            env->DeleteGlobalRef(g_rewriter_class);
            g_rewriter_class = nullptr;
            return -4;
        }
    }

    if (!ParcelRewriter_Init(env)) {
        LOGE("BinderTransactHook: ParcelRewriter_Init failed");
        return -5;
    }

    g_target_fn = ScanDynsym("libbinder.so", kTransactSym);
    if (!g_target_fn) {
        LOGE("BinderTransactHook: %s not found in libbinder.so dynsym", kTransactSym);
        return -6;
    }

    HookResult r = InstallInlineHookEx(g_target_fn, (void*)Hook_Transact,
                                       (void**)&_orig_transact, "IPCThreadState::transact");
    if (r != HOOK_OK && r != HOOK_ALREADY_HOOKED) {
        LOGE("BinderTransactHook: InstallInlineHookEx r=%d", (int)r);
        return -7;
    }
    g_hook_handle = FindHandle(g_target_fn);
    LOGD("BinderTransactHook: installed target=%p orig=%p", g_target_fn, (void*)_orig_transact);
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_uninstallNative(JNIEnv* env, jobject) {
    if (!g_hook_handle) return 0;
    HookResult r = UninstallInlineHook(g_target_fn);
    g_hook_handle = nullptr;
    g_target_fn = nullptr;
    _orig_transact = nullptr;
    if (g_rewriter_class && env) env->DeleteGlobalRef(g_rewriter_class);
    g_rewriter_class = nullptr;
    g_rewriter_rewriteReply = nullptr;
    LOGD("BinderTransactHook: uninstalled r=%d", (int)r);
    return (r == HOOK_OK) ? 1 : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_fireCountNative(JNIEnv*, jobject) {
    return g_transact_count.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_rewriteCountNative(JNIEnv*, jobject) {
    return g_rewrite_count.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_lastCodeNative(JNIEnv*, jobject) {
    return g_last_code.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_ims134_hookengine_BinderTransactHook_drainCodesNative(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lk(g_codes_mutex);
    int n = (int)g_codes.size();
    jintArray arr = env->NewIntArray(n);
    if (arr && n > 0) {
        env->SetIntArrayRegion(arr, 0, n, g_codes.data());
    }
    g_codes.clear();
    return arr;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_rewriteEffectiveCountNative(
        JNIEnv*, jobject) {
    return g_rewrite_effective_count.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jbyteArray JNICALL
Java_com_ims134_hookengine_NativeTestBridge_dumpLastCode36Reply(
        JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lk(g_reply_mutex);
    jsize n = (jsize)g_last_code36_reply.size();
    jbyteArray arr = env->NewByteArray(n);
    if (arr && n > 0) {
        env->SetByteArrayRegion(arr, 0, n,
                                (const jbyte*)g_last_code36_reply.data());
    }
    return arr;
}