package com.ims134.hookengine

import android.util.Log

/**
 * tnFIN — Binder transact interceptor.
 *
 * Session 10 (Option 2): hooks android::IPCThreadState::transact (C++ target,
 * via inline hook) instead of BinderProxy.transactNative (Java target).
 *
 * Rationale: on this ROM, ANY JNI call touching a jobject on the Java hook's
 * caller thread crashes (CheckJNI ScopedCheck walks the JIT frame). The C++
 * target lets the caller thread do only pure C++ memcpy + async dispatch,
 * with all JNI work on a fresh (safe) thread.
 */
object BinderTransactHook {
    init { System.loadLibrary("hookengine") }

    @Volatile var installed = false; private set
    @Volatile var lastError: String = ""; private set

    private external fun installNative(): Int
    private external fun uninstallNative(): Int
    private external fun fireCountNative(): Int
    private external fun rewriteCountNative(): Int
    private external fun lastCodeNative(): Int
    private external fun drainCodesNative(): IntArray

    private external fun rewriteEffectiveCountNative(): Int
    fun rewriteEffectiveCount(): Int = rewriteEffectiveCountNative()

    @Synchronized
    fun install(): Boolean {
        if (installed) return true
        lastError = ""
        val rc = installNative()
        if (rc != 0) {
            lastError = "installNative rc=$rc"
            Log.e("DualCore", "BinderTransactHook: $lastError")
            return false
        }
        installed = true
        Log.d("DualCore", "BinderTransactHook: installed")
        return true
    }

    @Synchronized
    fun uninstall(): Boolean {
        if (!installed) return true
        uninstallNative()
        installed = false
        return true
    }

    fun isInstalled(): Boolean = installed
    fun fireCount(): Int = fireCountNative()
    fun rewriteCount(): Int = rewriteCountNative()
    fun lastCode(): Int = lastCodeNative()
    fun drainCodes(): IntArray = drainCodesNative()
}