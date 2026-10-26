#include "ArtMethodTrace.h"
#include "JavaHook.h"
#include <mutex>
#include <android/log.h>
#include <cstring>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define MAX_TRACES 16

struct TraceSlot {
    bool         active = false;
    jmethodID    mid = nullptr;
    void*        orig_entry = nullptr;
    JavaHook*    hook = nullptr;
    TraceEntryFn onEntry = nullptr;
    TraceExitFn  onExit  = nullptr;
    void*        ctx = nullptr;
    int          arity = -1;
};

static TraceSlot    g_slots[MAX_TRACES];
static std::mutex   g_mutex;

static jint dispatch_int0 (JNIEnv* env, jobject thiz, int slot);
static jint dispatch_int1 (JNIEnv* env, jobject thiz, jint a0, int slot);
static jint dispatch_int0s(JNIEnv* env, jobject thiz, jstring s, int slot);
static jint dispatch_int1s(JNIEnv* env, jobject thiz, jstring s, jint a0, int slot);

// ─── Per-slot wrappers (template instantiations) ─────────────────────
template<int N>
static jint wrap_int0(JNIEnv* env, jobject thiz) {
    return dispatch_int0(env, thiz, N);
}

template<int N>
static jint wrap_int1(JNIEnv* env, jobject thiz, jint a0) {
    return dispatch_int1(env, thiz, a0, N);
}

template<int N>
static jint wrap_int0s(JNIEnv* env, jobject thiz, jstring s) {
    return dispatch_int0s(env, thiz, s, N);
}

template<int N>
static jint wrap_int1s(JNIEnv* env, jobject thiz, jstring s, jint a0) {
    return dispatch_int1s(env, thiz, s, a0, N);
}

static void* wrapperFor(int slot, int arity) {
    if (arity == 0) {
        static void* fns[] = {
                (void*)&wrap_int0<0>,  (void*)&wrap_int0<1>,  (void*)&wrap_int0<2>,  (void*)&wrap_int0<3>,
                (void*)&wrap_int0<4>,  (void*)&wrap_int0<5>,  (void*)&wrap_int0<6>,  (void*)&wrap_int0<7>,
                (void*)&wrap_int0<8>,  (void*)&wrap_int0<9>,  (void*)&wrap_int0<10>, (void*)&wrap_int0<11>,
                (void*)&wrap_int0<12>, (void*)&wrap_int0<13>, (void*)&wrap_int0<14>, (void*)&wrap_int0<15>,
        };
        if (slot < 0 || slot >= (int)(sizeof(fns)/sizeof(fns[0]))) return nullptr;
        return fns[slot];
    }
    if (arity == 1) {
        static void* fns[] = {
                (void*)&wrap_int1<0>,  (void*)&wrap_int1<1>,  (void*)&wrap_int1<2>,  (void*)&wrap_int1<3>,
                (void*)&wrap_int1<4>,  (void*)&wrap_int1<5>,  (void*)&wrap_int1<6>,  (void*)&wrap_int1<7>,
                (void*)&wrap_int1<8>,  (void*)&wrap_int1<9>,  (void*)&wrap_int1<10>, (void*)&wrap_int1<11>,
                (void*)&wrap_int1<12>, (void*)&wrap_int1<13>, (void*)&wrap_int1<14>, (void*)&wrap_int1<15>,
        };
        if (slot < 0 || slot >= (int)(sizeof(fns)/sizeof(fns[0]))) return nullptr;
        return fns[slot];
    }
    if (arity == 2) {
        static void* fns[] = {
                (void*)&wrap_int0s<0>,  (void*)&wrap_int0s<1>,  (void*)&wrap_int0s<2>,  (void*)&wrap_int0s<3>,
                (void*)&wrap_int0s<4>,  (void*)&wrap_int0s<5>,  (void*)&wrap_int0s<6>,  (void*)&wrap_int0s<7>,
                (void*)&wrap_int0s<8>,  (void*)&wrap_int0s<9>,  (void*)&wrap_int0s<10>, (void*)&wrap_int0s<11>,
                (void*)&wrap_int0s<12>, (void*)&wrap_int0s<13>, (void*)&wrap_int0s<14>, (void*)&wrap_int0s<15>,
        };
        if (slot < 0 || slot >= (int)(sizeof(fns)/sizeof(fns[0]))) return nullptr;
        return fns[slot];
    }
    if (arity == 3) {
        static void* fns[] = {
                (void*)&wrap_int1s<0>,  (void*)&wrap_int1s<1>,  (void*)&wrap_int1s<2>,  (void*)&wrap_int1s<3>,
                (void*)&wrap_int1s<4>,  (void*)&wrap_int1s<5>,  (void*)&wrap_int1s<6>,  (void*)&wrap_int1s<7>,
                (void*)&wrap_int1s<8>,  (void*)&wrap_int1s<9>,  (void*)&wrap_int1s<10>, (void*)&wrap_int1s<11>,
                (void*)&wrap_int1s<12>, (void*)&wrap_int1s<13>, (void*)&wrap_int1s<14>, (void*)&wrap_int1s<15>,
        };
        if (slot < 0 || slot >= (int)(sizeof(fns)/sizeof(fns[0]))) return nullptr;
        return fns[slot];
    }
    return nullptr;
}

static jint dispatch_int0(JNIEnv* env, jobject thiz, int slot) {
    TraceSlot snap;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_slots[slot].active) return 0;
        snap = g_slots[slot];
    }
    if (snap.onEntry) snap.onEntry(snap.ctx);
    typedef jint (*OrigFn)(JNIEnv*, jobject);
    jint ret = ((OrigFn)snap.orig_entry)(env, thiz);
    if (snap.onExit) snap.onExit(snap.ctx, (int64_t)ret);
    return ret;
}

static jint dispatch_int1(JNIEnv* env, jobject thiz, jint a0, int slot) {
    TraceSlot snap;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_slots[slot].active) return 0;
        snap = g_slots[slot];
    }
    if (snap.onEntry) snap.onEntry(snap.ctx);
    typedef jint (*OrigFn)(JNIEnv*, jobject, jint);
    jint ret = ((OrigFn)snap.orig_entry)(env, thiz, a0);
    if (snap.onExit) snap.onExit(snap.ctx, (int64_t)ret);
    return ret;
}

static jint dispatch_int0s(JNIEnv* env, jobject thiz, jstring s, int slot) {
    TraceSlot snap;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_slots[slot].active) return 0;
        snap = g_slots[slot];
    }
    if (snap.onEntry) snap.onEntry(snap.ctx);
    typedef jint (*OrigFn)(JNIEnv*, jobject, jstring);
    jint ret = ((OrigFn)snap.orig_entry)(env, thiz, s);
    if (snap.onExit) snap.onExit(snap.ctx, (int64_t)ret);
    return ret;
}

static jint dispatch_int1s(JNIEnv* env, jobject thiz, jstring s, jint a0, int slot) {
    TraceSlot snap;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (!g_slots[slot].active) return 0;
        snap = g_slots[slot];
    }
    if (snap.onEntry) snap.onEntry(snap.ctx);
    typedef jint (*OrigFn)(JNIEnv*, jobject, jstring, jint);
    jint ret = ((OrigFn)snap.orig_entry)(env, thiz, s, a0);
    if (snap.onExit) snap.onExit(snap.ctx, (int64_t)ret);
    return ret;
}

extern "C" HookResult TraceAttach(JNIEnv* env, jobject methodObj,
                                  TraceEntryFn onEntry, TraceExitFn onExit,
                                  void* ctx, int arity) {
    if (!env || !methodObj) return HOOK_INTERNAL_ERROR;
    if (arity < 0 || arity > 3) return HOOK_INTERNAL_ERROR;

    jmethodID mid = env->FromReflectedMethod(methodObj);
    if (!mid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return HOOK_INTERNAL_ERROR;
    }

    std::lock_guard<std::mutex> lk(g_mutex);

    for (auto& s : g_slots) {
        if (s.active && s.mid == mid) return HOOK_ALREADY_HOOKED;
    }

    int slot = -1;
    for (int i = 0; i < MAX_TRACES; i++) {
        if (!g_slots[i].active) { slot = i; break; }
    }
    if (slot < 0) return HOOK_ALLOC_FAILED;

    void* wrapper = wrapperFor(slot, arity);
    if (!wrapper) return HOOK_INTERNAL_ERROR;

    void* orig = nullptr;
    JavaHook* h = JavaHookInstall(env, methodObj, wrapper, &orig, "trace");
    if (!h) {
        LOGE("TraceAttach: JavaHookInstall failed (slot=%d arity=%d)", slot, arity);
        return HOOK_INTERNAL_ERROR;
    }

    g_slots[slot].active = true;
    g_slots[slot].mid = mid;
    g_slots[slot].orig_entry = orig;
    g_slots[slot].hook = h;
    g_slots[slot].onEntry = onEntry;
    g_slots[slot].onExit = onExit;
    g_slots[slot].ctx = ctx;
    g_slots[slot].arity = arity;

    LOGD("TraceAttach: slot=%d arity=%d orig=%p", slot, arity, orig);
    return HOOK_OK;
}

extern "C" HookResult TraceDetach(JNIEnv* env, jobject methodObj) {
    if (!env || !methodObj) return HOOK_INTERNAL_ERROR;
    jmethodID mid = env->FromReflectedMethod(methodObj);
    if (!mid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return HOOK_INTERNAL_ERROR;
    }

    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& s : g_slots) {
        if (!s.active || s.mid != mid) continue;

        if (s.hook) JavaHookUninstall(s.hook);
        s.active = false;
        s.hook = nullptr;
        s.mid = nullptr;
        s.orig_entry = nullptr;
        s.onEntry = nullptr;
        s.onExit = nullptr;
        s.ctx = nullptr;
        s.arity = -1;
        LOGD("TraceDetach: ok");
        return HOOK_OK;
    }
    return HOOK_NOT_HOOKED;
}

extern "C" int TraceCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    int n = 0;
    for (auto& s : g_slots) if (s.active) n++;
    return n;
}

extern "C" void TraceResetAll() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& s : g_slots) {
        if (s.active && s.hook) JavaHookUninstall(s.hook);
        s.active = false;
        s.hook = nullptr;
        s.mid = nullptr;
        s.orig_entry = nullptr;
        s.onEntry = nullptr;
        s.onExit = nullptr;
        s.ctx = nullptr;
        s.arity = -1;
    }
}