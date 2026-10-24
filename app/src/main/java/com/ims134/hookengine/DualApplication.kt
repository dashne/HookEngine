package com.ims134.hookengine

import android.app.Application
import android.content.Context
import android.util.Log

class DualApplication : Application() {

    companion object {
        init { System.loadLibrary("hookengine") }

        private const val TAG = "DualCore"
        private var guestApp: Application? = null
        private var initialized = false
    }

    // Native entry — called once during attachBaseContext
    private external fun nativeOnLoad(dexPath: String, libPath: String)
    private external fun nativeAttachBaseContext(base: Context)
    private external fun nativeOnCreate()

    override fun attachBaseContext(base: Context?) {
        if (base == null) {
            super.attachBaseContext(null)
            return
        }

        Log.d(TAG, "DualApplication.attachBaseContext: entering")

        // 1. Engine init — MUST happen before any hook installation
        try {
            nativeOnLoad(base.applicationInfo.sourceDir, base.applicationInfo.nativeLibraryDir)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeOnLoad failed: $t", t)
        }

        // 2. Native hooks (file/io/uid/exec/process/ptrace/kill)
        try {
            nativeAttachBaseContext(base)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeAttachBaseContext failed: $t", t)
        }

        // 3. Kotlin-side hooks — order matters: classloader BEFORE package
        try {
            ClassLoaderBridge.install()
        } catch (t: Throwable) {
            Log.e(TAG, "ClassLoaderBridge.install failed: $t", t)
        }
        try {
            PmSpoof.install(base)
        } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof.install failed: $t", t)
        }

        // 4. Now that hooks are live, let the guest app initialize normally
        Log.d(TAG, "DualApplication: hooks installed, calling super.attachBaseContext")
        super.attachBaseContext(base)

        initialized = true
    }

    override fun onCreate() {
        Log.d(TAG, "DualApplication.onCreate")
        super.onCreate()

        if (!initialized) {
            Log.e(TAG, "DualApplication.onCreate: not initialized (attachBaseContext failed?)")
            return
        }

        // Give PmSpoof another retry — ActivityThread has finished initializing.
        try { PmSpoof.install(this) } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof.install (onCreate) failed: $t", t)
        }
        // tnFIN payload needs to know which package's signatures to null
        ParcelRewriter.ourPackageName = packageName

        try {
            nativeOnCreate()
        } catch (t: Throwable) {
            Log.e(TAG, "nativeOnCreate failed: $t", t)
        }
    }
}