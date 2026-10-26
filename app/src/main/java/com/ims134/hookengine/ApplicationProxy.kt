package com.ims134.hookengine

import android.app.Application
import android.content.Context
import android.content.res.Configuration
import android.content.res.Resources

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

    /** Sandbox dataDir for the guest app — created by DualApplication before boot. */
    @Volatile var guestDataDir: String? = null
        private set

    fun setGuestDataDir(path: String?) { this.guestDataDir = path }

    // Session 38 — nativeInit (structural port).
    // 3-param signature matches reference decomp (Session 20):
    //   nativeInit(String, String, int) -> int
    // Behavior: extract .so files and classes*.dex from the staged APK
    // into libPath; return 0 on success, -1 on failure. Body inferred from
    // reference architecture (JNI_OnLoad + ApplicationProxy flow); sub_2A998
    // body not directly decomp'd. Rule 5 partial — structural port only.
    external fun nativeInit(dexPath: String, libPath: String, flags: Int): Int

    external fun nativeOnCreate()
    external fun nativeAttachGuest(): Boolean
    external fun nativeCreateGuest(): Boolean
    external fun nativeIsGuestCreated(): Boolean
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
    fun onDestroy() { nativeOnDestroy() }
    fun setBaseContext(base: Context) { attachBaseContext(base) }
}