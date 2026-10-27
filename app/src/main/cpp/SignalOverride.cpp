#include "SignalOverride.h"
#include <pthread.h>
#include <unistd.h>
#include <ucontext.h>
#include <cstdint>
#include <cstring>
#include <android/log.h>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// SIGRTMAX is __libc_current_sigrtmax() on Android — a function call, not
// a compile-time constant. _NSIG is 65 on bionic and IS a compile-time
// constant. Valid signal numbers on Linux/Android are 1..64 inclusive.
static constexpr int kNSig = _NSIG;   // == 65

struct SO_Slot {
    int                   installed;    // 0/1, accessed via __atomic
    int                   signo;
    SignalOverrideHandler handler;
    void*                 user_ctx;
    struct sigaction      old_action;
    int                   fire_count;   // accessed via __atomic
};

static SO_Slot          g_slots[kNSig] = {};
static pthread_mutex_t  g_mutex = PTHREAD_MUTEX_INITIALIZER;

// Async-signal-safe dispatch. No locks, no allocs, no JNI.
static void so_dispatch(int signo, siginfo_t* info, void* ucv) {
    if (signo <= 0 || signo >= kNSig) return;
    SO_Slot* slot = &g_slots[signo];

    if (!__atomic_load_n(&slot->installed, __ATOMIC_ACQUIRE)) {
        // Raced with uninstall — chain to whatever was there to avoid
        // dropping the signal entirely.
        struct sigaction old = slot->old_action;
        if ((old.sa_flags & SA_SIGINFO) && old.sa_sigaction
            && old.sa_sigaction != (void*)SIG_DFL
            && old.sa_sigaction != (void*)SIG_IGN) {
            old.sa_sigaction(signo, info, ucv);
        }
        return;
    }

    __atomic_fetch_add(&slot->fire_count, 1, __ATOMIC_RELAXED);

    SignalOverrideHandler h = slot->handler;
    void* ctx = slot->user_ctx;
    SignalOverrideAction action = SIGOVR_PASS_THROUGH;
    if (h) action = h(signo, info, ucv, ctx);

    if (action == SIGOVR_SUPPRESS) return;

    // PASS_THROUGH — chain to old action.
    struct sigaction old = slot->old_action;
    if (old.sa_flags & SA_SIGINFO) {
        if (old.sa_sigaction
            && old.sa_sigaction != (void*)SIG_DFL
            && old.sa_sigaction != (void*)SIG_IGN) {
            old.sa_sigaction(signo, info, ucv);
            return;
        }
        if (old.sa_sigaction == (void*)SIG_IGN) return;
    } else {
        if (old.sa_handler && old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN) {
            old.sa_handler(signo);
            return;
        }
        if (old.sa_handler == SIG_IGN) return;
    }

    // Old action was SIG_DFL — restore and re-raise (typically terminates).
    struct sigaction dfl = {};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(signo, &dfl, nullptr);
    raise(signo);
}

extern "C" HookResult SignalOverrideInstall(int signo, SignalOverrideHandler handler, void* user_ctx) {
    if (signo <= 0 || signo >= kNSig) return HOOK_TARGET_INVALID;
    if (signo == SIGKILL || signo == SIGSTOP) return HOOK_TARGET_INVALID;
    if (!handler) return HOOK_INTERNAL_ERROR;

    pthread_mutex_lock(&g_mutex);
    SO_Slot* slot = &g_slots[signo];
    if (__atomic_load_n(&slot->installed, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&g_mutex);
        return HOOK_ALREADY_HOOKED;
    }

    struct sigaction sa = {};
    sa.sa_sigaction = so_dispatch;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, signo);

    struct sigaction old = {};
    if (sigaction(signo, &sa, &old) != 0) {
        pthread_mutex_unlock(&g_mutex);
        LOGE("SignalOverride: sigaction install failed for %d", signo);
        return HOOK_INTERNAL_ERROR;
    }

    slot->signo = signo;
    slot->handler = handler;
    slot->user_ctx = user_ctx;
    slot->old_action = old;
    // Reset fire_count as part of install — critical for independent
    // test cycles. Session 52 found that leaving a stale count from a
    // prior install broke the "baseline fire count == 0" pre-install
    // assertion in sigovrInstallFireSuppress when tests ran in
    // alphabetical order.
    __atomic_store_n(&slot->fire_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&slot->installed, 1, __ATOMIC_RELEASE);

    pthread_mutex_unlock(&g_mutex);
    LOGD("SignalOverride: installed for signal %d", signo);
    return HOOK_OK;
}

extern "C" HookResult SignalOverrideUninstall(int signo) {
    if (signo <= 0 || signo >= kNSig) return HOOK_TARGET_INVALID;
    pthread_mutex_lock(&g_mutex);
    SO_Slot* slot = &g_slots[signo];
    if (!__atomic_load_n(&slot->installed, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&g_mutex);
        return HOOK_NOT_HOOKED;
    }

    // Restore old action FIRST, then clear installed flag.
    sigaction(signo, &slot->old_action, nullptr);
    __atomic_store_n(&slot->installed, 0, __ATOMIC_RELEASE);
    slot->handler = nullptr;
    slot->user_ctx = nullptr;
    // Reset fire_count as part of uninstall — leaves the slot in a
    // fully clean pre-install state so a subsequent baseline assertion
    // sees 0. Session 52 fix: prior version left fire_count at its
    // accumulated value, causing the NEXT install's pre-check to fail.
    __atomic_store_n(&slot->fire_count, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&g_mutex);
    LOGD("SignalOverride: uninstalled for signal %d", signo);
    return HOOK_OK;
}

extern "C" int SignalOverrideCount() {
    int n = 0;
    for (int i = 1; i < kNSig; i++) {
        if (__atomic_load_n(&g_slots[i].installed, __ATOMIC_ACQUIRE)) n++;
    }
    return n;
}

extern "C" int SignalOverrideFireCount(int signo) {
    if (signo <= 0 || signo >= kNSig) return -1;
    return __atomic_load_n(&g_slots[signo].fire_count, __ATOMIC_RELAXED);
}

extern "C" void SignalOverrideResetAll() {
    for (int i = 1; i < kNSig; i++) {
        if (__atomic_load_n(&g_slots[i].installed, __ATOMIC_ACQUIRE)) {
            SignalOverrideUninstall(i);
        }
    }
}