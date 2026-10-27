package com.ims134.hookengine

import android.os.IBinder
import android.util.Log

/**
 * Session 51 — Feature 3.4 ServiceManager lookup interception.
 *
 * The active mechanism is transact-level rewrite
 * (ParcelRewriter_TryRewriteServiceManagerLookup in CppParcelRewriter.cpp).
 * Alias and null overrides both work on names delivered via IPC transact.
 * 8 green tests in ServiceManagerTest.kt.
 *
 * Cache-hit interception is architecturally blocked on MIUI/API 37.
 * All three ServiceManager lookup methods (rawGetService, getService,
 * checkService) return fast-JNI tokens from FromReflectedMethod
 * (0x3d, 0x3f, 0x41) — no ArtMethod to hook. The dead-mechanism methods
 * below return -1 with a loud log for reference; do not call.
 */
object ServiceManagerShim {
    private const val TAG = "FastJni"
    init { System.loadLibrary("hookengine") }

    // Legacy FastJni bridges — no longer used by active code
    private external fun nativeDirectInstall(artMethod: Long, trampoline: Long): Int
    private external fun nativeReadU64(addr: Long): Long
    private external fun nativeRestore(): Int
    private external fun nativeGetHits(): Int
    private external fun nativeJavaGetService(name: String): IBinder?

    // Dead mechanisms — C++ returns -1 with a diagnostic log
    private external fun nativeInstallArrayMapHook(sCacheInstance: Any): Int
    private external fun nativeUninstallArrayMapHook(): Int
    private external fun nativeGetArrayMapHits(): Int
    private external fun nativeInstallRawGetServiceHook(): Int
    private external fun nativeUninstallRawGetServiceHook(): Int
    private external fun nativeGetRawGetServiceHits(): Int

    @Volatile private var rawGetServiceMethod: java.lang.reflect.Method? = null
    @Volatile private var initialized = false
    @Volatile private var callCount: Int = 0

    @JvmStatic fun getCallCount(): Int = callCount
    @JvmStatic fun resetCallCount() { callCount = 0 }
    @JvmStatic fun nativeHits(): Int = nativeGetHits()
    @JvmStatic fun arrayMapHits(): Int = nativeGetArrayMapHits()
    @JvmStatic fun rawGetServiceHits(): Int = nativeGetRawGetServiceHits()

    private fun ensureInit() {
        if (initialized) return
        synchronized(this) {
            if (initialized) return
            try {
                val cls = Class.forName("android.os.ServiceManager")
                val m = HiddenApiBridge.reflectGetDeclaredMethod(
                    cls, "rawGetService", arrayOf(String::class.java))
                if (m != null) {
                    HiddenApiBridge.reflectSetAccessible(m)
                    rawGetServiceMethod = m
                    Log.d(TAG, "rawGetService Method obtained")
                }
            } catch (t: Throwable) {
                Log.e(TAG, "ensureInit failed: ${t.javaClass.simpleName}")
            }
            initialized = true
        }
    }

    @JvmStatic
    fun getArtMethodPointer(method: java.lang.reflect.Method): Long {
        var cls: Class<*>? = method.javaClass
        while (cls != null) {
            val f = HiddenApiBridge.reflectGetDeclaredField(cls, "artMethod")
            if (f != null && HiddenApiBridge.reflectSetAccessible(f)) {
                return try { f.getLong(method) } catch (_: Throwable) { 0L }
            }
            cls = cls.superclass
        }
        return 0L
    }

    @JvmStatic
    fun directGetService(name: String): IBinder? = nativeJavaGetService(name)

    @JvmStatic
    fun install(): Int {
        ensureInit()
        if (rawGetServiceMethod == null) return -1
        val cls = Class.forName("android.os.ServiceManager")
        val m = cls.getDeclaredMethod("getService", String::class.java)
        val am = getArtMethodPointer(m)
        if (am < 0x1000) return -2
        try { directGetService("activity") } catch (_: Throwable) {}
        return nativeDirectInstall(am, 0L)
    }

    @JvmStatic
    fun uninstall(): Int = nativeRestore()

    // ─── Dead APIs — retained for symbol compatibility ─────────────────
    @JvmStatic
    fun installArrayMapHook(): Int {
        return try { nativeInstallArrayMapHook(Any()) } catch (_: Throwable) { -1 }
    }

    @JvmStatic
    fun uninstallArrayMapHook(): Int {
        return try { nativeUninstallArrayMapHook() } catch (_: Throwable) { -1 }
    }

    @JvmStatic
    fun installRawGetServiceHook(): Int {
        return try { nativeInstallRawGetServiceHook() } catch (_: Throwable) { -1 }
    }

    @JvmStatic
    fun uninstallRawGetServiceHook(): Int {
        return try { nativeUninstallRawGetServiceHook() } catch (_: Throwable) { -1 }
    }

    @JvmStatic
    fun call(name: String): IBinder? {
        callCount++
        ensureInit()
        val m = rawGetServiceMethod ?: return null
        return try { m.invoke(null, name) as? IBinder } catch (_: Throwable) { null }
    }
}