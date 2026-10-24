#include <jni.h>
#include <android/log.h>
#include <atomic>
#include <future>
#include <dlfcn.h>
#include "JavaHook.h"

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

typedef jboolean (*BinderTransact_t)(JNIEnv*, jobject, jint, jobject, jobject, jint);
static BinderTransact_t _orig_binder_transact = nullptr;
static JavaHook* g_binder_hook = nullptr;

static std::atomic<int> g_transact_count{0};
static std::atomic<int> g_transact_last_code{0};
static std::atomic<int> g_inside_rewriter{0};

static constexpr jint kCodeGetPackageInfo       = 3;
static constexpr jint kCodeGetInstalledPackages = 36;

static constexpr int kCodeBufSize = 256;
static int g_code_buf[kCodeBufSize];
static std::atomic<int> g_code_count{0};

static JavaVM*   g_vm                    = nullptr;
static jclass    g_rewriter_class        = nullptr;
static jmethodID g_rewriter_rewriteReply = nullptr;

// ─── Raw (non-CheckJNI) JNI function pointers ────────────────────────
//
// Under `am instrument`, ART enables -Xcheck:jni (CheckJNI). Every
// env->NewGlobalRef(...) on a foreign-thread local ref goes:
//   CheckJNI::NewRef → ScopedCheck::Check → IsJniTransitionReference →
//   StackVisitor::WalkStack → FindOatMethodFor → SIGSEGV
// when the current stack has a JIT-compiled caller above us
// (android.os.BinderProxy.transact+1344 in dalvik-jit-code-cache, entered
// because we inline-hooked transactNative).
//
// art::JNI<false>::NewGlobalRef does the same ref-table lookup (so it
// correctly resolves caller-thread local refs to their mirror objects)
// but SKIPS the validation walk. That's the whole difference. Calling
// the raw function directly from dlsym bypasses the crash entirely.
//
// Mangled name `_ZN3art3JNIILb0EE13NewGlobalRefEP7_JNIEnvP8_jobject`
// for art::JNI<false>::NewGlobalRef(JNIEnv*, jobject).
typedef jobject (*RawNewGlobalRef_t)(JNIEnv*, jobject);
static RawNewGlobalRef_t g_rawNewGlobalRef = nullptr;

static void ResolveRawJni() {
    if (g_rawNewGlobalRef) return;
    // Multiple candidate manglings — different Clang versions produce
    // slightly different type-name encodings (JNIEnv vs _JNIEnv).
    const char* ngr[] = {
            "_ZN3art3JNIILb0EE13NewGlobalRefEP7_JNIEnvP8_jobject",
            "_ZN3art3JNIILb0EE13NewGlobalRefEP7_JNIEnvP7_jobject",
            "_ZN3art3JNIILb0EE13NewGlobalRefEP6JNIEnvP8_jobject",
            nullptr
    };
    for (int i = 0; ngr[i]; i++) {
        g_rawNewGlobalRef = (RawNewGlobalRef_t)dlsym(RTLD_DEFAULT, ngr[i]);
        LOGD("BinderTransactHook: dlsym(%s) = %p", ngr[i], (void*)g_rawNewGlobalRef);
        if (g_rawNewGlobalRef) return;
    }
    LOGE("BinderTransactHook: raw NewGlobalRef unavailable — "
         "rewriter will skip under CheckJNI. Fallback: C++ Parcel rewriter.");
}

static void RewriteIfTarget(JNIEnv* realEnv, jint code, jobject data, jobject reply) {
    if (code != kCodeGetPackageInfo && code != kCodeGetInstalledPackages) return;
    if (!g_vm || !g_rewriter_class || !g_rewriter_rewriteReply) return;
    if (!g_rawNewGlobalRef) return;   // resolver failed

    // ─── Fix A: raw NewGlobalRef on caller thread ────────────────────
    // Caller thread has the JIT `BinderProxy.transact` frame above us,
    // so any CheckJNI-validated call crashes. Raw NewGlobalRef does the
    // same ref-table walk but skips validation. No crash; returns a
    // real global ref we can pass to the fresh thread.
    jobject gData  = g_rawNewGlobalRef(realEnv, data);
    jobject gReply = g_rawNewGlobalRef(realEnv, reply);
    if (!gData || !gReply) {
        LOGE("BinderTransactHook: rawNewGlobalRef failed gData=%p gReply=%p",
             (void*)gData, (void*)gReply);
        if (gData)  realEnv->DeleteGlobalRef(gData);
        if (gReply) realEnv->DeleteGlobalRef(gReply);
        return;
    }

    // ─── Fix B: run rewriter on fresh thread ─────────────────────────
    // The rewriter's own JNI calls (Parcel.readInt, etc.) trigger the
    // same CheckJNI walk. On a fresh attached thread, the stack above
    // is `pthread_start → lambda → CallStaticVoidMethod`, with no JIT
    // frames to trip over. The walk terminates cleanly.
    jint c = code;
    auto fut = std::async(std::launch::async, [c, gData, gReply]() {
        JNIEnv* e = nullptr;
        if (!g_vm || g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) {
            LOGE("BinderTransactHook: fresh thread attach failed");
            return;
        }
        if (e) {
            e->CallStaticVoidMethod(g_rewriter_class, g_rewriter_rewriteReply,
                                    c, gData, gReply);
            if (e->ExceptionCheck()) {
                LOGE("BinderTransactHook: rewriter threw (cleared, code=%d)", (int)c);
                e->ExceptionClear();
            }
            e->DeleteGlobalRef(gData);
            e->DeleteGlobalRef(gReply);
        }
        g_vm->DetachCurrentThread();
    });
    fut.wait();   // synchronous — caller reads modified reply after this
}

static jboolean FakeBinderTransact(JNIEnv* env_as_received, jobject thiz, jint code,
                                   jobject data, jobject reply, jint flags) {
    g_transact_count.fetch_add(1, std::memory_order_relaxed);
    g_transact_last_code.store(code, std::memory_order_relaxed);

    int idx = g_code_count.fetch_add(1, std::memory_order_relaxed);
    if (idx >= 0 && idx < kCodeBufSize) g_code_buf[idx] = code;

    if (!_orig_binder_transact) {
        LOGE("BinderTransactHook: no original (code=%d)", (int)code);
        return JNI_FALSE;
    }

    if (g_inside_rewriter.load(std::memory_order_acquire) > 0) {
        return _orig_binder_transact(env_as_received, thiz, code, data, reply, flags);
    }

    // env_as_received IS the ArtMethod* the JIT stub expects (compiled
    // ABI). Do NOT change to realEnv — that would break the JIT stub.
    jboolean ok = _orig_binder_transact(env_as_received, thiz, code, data, reply, flags);
    if (!ok) return ok;

    if (code == kCodeGetPackageInfo || code == kCodeGetInstalledPackages) {
        JNIEnv* realEnv = nullptr;
        if (!g_vm || g_vm->GetEnv((void**)&realEnv, JNI_VERSION_1_6) != JNI_OK || !realEnv) {
            LOGE("BinderTransactHook: GetEnv failed, skipping rewriter");
            return ok;
        }
        if (idx < 5) {
            LOGD("BinderTransactHook: transact code=%d realEnv=%p", (int)code, (void*)realEnv);
        }
        g_inside_rewriter.fetch_add(1, std::memory_order_acq_rel);
        RewriteIfTarget(realEnv, code, data, reply);
        g_inside_rewriter.fetch_sub(1, std::memory_order_acq_rel);
    }
    return ok;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_installNative(
        JNIEnv* env, jobject, jobject methodObj) {
    if (g_binder_hook) return 0;
    if (!env || !methodObj) return -1;

    if (!g_vm) env->GetJavaVM(&g_vm);

    ResolveRawJni();

    if (!g_rewriter_class) {
        jclass local = env->FindClass("com/ims134/hookengine/ParcelRewriter");
        if (env->ExceptionCheck() || !local) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("BinderTransactHook: FindClass(ParcelRewriter) failed at install");
            return -3;
        }
        g_rewriter_class = (jclass)env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
        if (!g_rewriter_class) {
            LOGE("BinderTransactHook: NewGlobalRef failed at install");
            return -3;
        }
        g_rewriter_rewriteReply = env->GetStaticMethodID(
                g_rewriter_class, "rewriteReply",
                "(ILandroid/os/Parcel;Landroid/os/Parcel;)V");
        if (env->ExceptionCheck() || !g_rewriter_rewriteReply) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            LOGE("BinderTransactHook: GetStaticMethodID(rewriteReply) failed at install");
            env->DeleteGlobalRef(g_rewriter_class);
            g_rewriter_class = nullptr;
            return -4;
        }
        LOGD("BinderTransactHook: cached rewriter class=0x%lx method=0x%lx",
             (unsigned long)(uintptr_t)g_rewriter_class,
             (unsigned long)(uintptr_t)g_rewriter_rewriteReply);
    }

    JavaHook* h = JavaHookInstall(env, methodObj, (void*)FakeBinderTransact,
                                  (void**)&_orig_binder_transact,
                                  "BinderProxy.transactNative");
    if (!h) { LOGE("BinderTransactHook: JavaHookInstall returned null"); return -2; }
    g_binder_hook = h;
    LOGD("BinderTransactHook: installed hook=0x%lx orig=0x%lx g_vm=0x%lx rawNGR=0x%lx",
         (unsigned long)(uintptr_t)h,
         (unsigned long)(uintptr_t)_orig_binder_transact,
         (unsigned long)(uintptr_t)g_vm,
         (unsigned long)(uintptr_t)g_rawNewGlobalRef);
    return 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_BinderTransactHook_uninstallNative(JNIEnv* env, jobject) {
    if (!g_binder_hook) return 0;
    HookResult r = JavaHookUninstall(g_binder_hook);
    g_binder_hook = nullptr;
    _orig_binder_transact = nullptr;
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
Java_com_ims134_hookengine_BinderTransactHook_lastCodeNative(JNIEnv*, jobject) {
    return g_transact_last_code.load(std::memory_order_relaxed);
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_ims134_hookengine_BinderTransactHook_drainCodesNative(JNIEnv* env, jobject) {
    int count = g_code_count.exchange(0, std::memory_order_acq_rel);
    if (count > kCodeBufSize) count = kCodeBufSize;
    if (count < 0) count = 0;
    jintArray arr = env->NewIntArray(count);
    if (!arr) return nullptr;
    if (count > 0) env->SetIntArrayRegion(arr, 0, count, g_code_buf);
    return arr;
}