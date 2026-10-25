package com.ims134.hookengine

import android.app.Application
import android.content.Context
import android.content.res.Configuration

/**
 * Session 20 — ApplicationProxy shape (Kotlin-compatible).
 *
 * Reference: com.excelliance.kxqp.platform.ApplicationProxy, whose
 * onCreate/onTerminate/onConfigurationChanged are `public native`.
 * Kotlin forbids `external override`, so we override normally and
 * forward into non-override `external fun`s. From the framework's
 * view the class is a normal Application; from ours, all logic lives
 * in ApplicationProxy.cpp.
 *
 * Not declared in the manifest. Container (DualApplication or our test)
 * creates it via getInstance(), calls setBaseContext(guestCtx), then
 * onCreate(). Native onCreate boots the guest Application using the
 * guest Context it finds via getBaseContext().
 *
 * Rule 15: engine behavior is native. Kotlin only declares the surface.
 */
class ApplicationProxy : Application() {

    companion object {
        @Volatile private var instance: ApplicationProxy? = null

        @JvmStatic
        fun getInstance(): ApplicationProxy {
            val existing = instance
            if (existing != null) return existing
            synchronized(ApplicationProxy::class.java) {
                val again = instance
                if (again != null) return again
                val fresh = ApplicationProxy()
                instance = fresh
                return fresh
            }
        }

        /** Returns the booted guest Application, or null before onCreate. */
        @JvmStatic
        external fun getGuestApplication(): Application?
    }

    // ─── Native lifecycle (ApplicationProxy shape) ─────────────────────
    // Kotlin won't allow `external override`, so the override is a
    // thin Kotlin method that forwards into the external fun. The
    // framework still calls attachBaseContext → onCreate → onTerminate
    // → onConfigurationChanged. Native code handles the work.

    external fun nativeOnCreate()
    external fun nativeOnTerminate()
    external fun nativeOnConfigurationChanged(newConfig: Configuration)
    external fun nativeOnDestroy()

    override fun onCreate() {
        super.onCreate()
        nativeOnCreate()
    }

    override fun onTerminate() {
        super.onTerminate()
        nativeOnTerminate()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        nativeOnConfigurationChanged(newConfig)
    }

    /** Not an Application lifecycle method — called by container on shutdown. */
    fun onDestroy() {
        nativeOnDestroy()
    }

    /** Public wrapper for protected attachBaseContext — reference-shape container call. */
    fun setBaseContext(base: Context) {
        attachBaseContext(base)
    }
}