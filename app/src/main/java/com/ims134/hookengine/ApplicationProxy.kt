package com.ims134.hookengine

import android.app.Application
import android.content.Context
import android.content.res.Configuration
import android.content.res.Resources

/**
 * Engine Application subclass — mirrors reference ApplicationProxy:
 * native onCreate/onTerminate/onConfigurationChanged/onDestroy which
 * dispatch into libhookengine.so. Not in manifest. Container calls
 * getInstance(), setBaseContext(guestCtx), then onCreate() which
 * attaches AND creates the guest Application in one native call.
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

        @JvmStatic
        fun getGuestApplication(): Application? =
            instance?.nativeGetGuestApplication()

        @JvmStatic
        fun buildGuestResources(apkPath: String, host: Context): Resources? =
            instance?.nativeBuildGuestResources(apkPath, host)
    }

    external fun nativeOnCreate()
    external fun nativeOnTerminate()
    external fun nativeOnConfigurationChanged(newConfig: Configuration)
    external fun nativeOnDestroy()
    external fun nativeGetGuestApplication(): Application?
    external fun nativeBuildGuestResources(apkPath: String, host: Context): Resources?

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

    fun onDestroy() {
        nativeOnDestroy()
    }

    fun setBaseContext(base: Context) {
        attachBaseContext(base)
    }
}