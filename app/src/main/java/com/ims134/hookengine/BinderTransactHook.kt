package com.ims134.hookengine

import android.util.Log

/**
 * `tnFIN` — Binder transact interceptor (reference equivalent).
 *
 * Hooks `BinderProxy.transactNative(int, Parcel, Parcel, int)`, the native
 * entry point behind every Java-side AIDL transaction. When installed, all
 * `IPackageManager$Stub$Proxy.transact(...)` calls route through
 * `FakeBinderTransact` before reaching the remote service.
 *
 * Session 8: pass-through + fire counter (mechanism proven).
 * Session 9: parcel rewriting for signature spoofing via AIDL.
 */
object BinderTransactHook {
    init { System.loadLibrary("hookengine") }

    @Volatile var installed = false
        private set

    @Volatile var lastError: String = ""
        private set

    private external fun installNative(method: java.lang.reflect.Method): Int
    private external fun uninstallNative(): Int
    private external fun fireCountNative(): Int
    private external fun lastCodeNative(): Int

    @Synchronized
    fun install(): Boolean {
        if (installed) return true
        lastError = ""

        val cls = try {
            Class.forName("android.os.BinderProxy")
        } catch (t: Throwable) {
            lastError = "Class.forName(BinderProxy): $t"
            return false
        }

        val params = arrayOf<Class<*>>(
            Integer.TYPE,
            android.os.Parcel::class.java,
            android.os.Parcel::class.java,
            Integer.TYPE,
        )

        val method = try {
            cls.getDeclaredMethod("transactNative", *params)
        } catch (_: Throwable) {
            // Reflection may be filtered even on public methods after our bypass
            HiddenApiBridge.reflectGetDeclaredMethod(cls, "transactNative", params)
        } ?: run {
            lastError = "transactNative not found"
            return false
        }

        val rc = installNative(method)
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
    fun lastCode(): Int = lastCodeNative()
}