package com.ims134.hookengine

import android.app.Application
import android.content.Context
import android.content.res.Configuration
import android.util.Log
import java.io.File
import java.util.zip.ZipFile

class DualApplication : Application() {

    companion object {
        init { System.loadLibrary("hookengine") }
        private const val TAG = "DualCore"
        private const val GUEST_APK = "viber.apk"
        private const val GUEST_LIB_DIR = "guest-libs"
        private var initialized = false
    }

    private external fun nativeOnLoad(dexPath: String, libPath: String)
    private external fun nativeAttachBaseContext(base: Context)
    private external fun nativeOnCreate()

    override fun attachBaseContext(base: Context?) {
        if (base == null) {
            super.attachBaseContext(null)
            return
        }

        Log.d(TAG, "DualApplication.attachBaseContext: entering")

        try {
            nativeOnLoad(base.applicationInfo.sourceDir, base.applicationInfo.nativeLibraryDir)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeOnLoad failed: $t", t)
        }

        try {
            nativeAttachBaseContext(base)
        } catch (t: Throwable) {
            Log.e(TAG, "nativeAttachBaseContext failed: $t", t)
        }

        try { ClassLoaderBridge.install() } catch (t: Throwable) {
            Log.e(TAG, "ClassLoaderBridge.install failed: $t", t)
        }
        try { PmSpoof.install(base) } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof.install failed: $t", t)
        }

        Log.d(TAG, "DualApplication: hooks installed, calling super.attachBaseContext")
        super.attachBaseContext(base)

        tryWireApplicationProxy(base)

        initialized = true
    }

    override fun onCreate() {
        Log.d(TAG, "DualApplication.onCreate")
        super.onCreate()

        if (!initialized) {
            Log.e(TAG, "DualApplication.onCreate: not initialized")
            return
        }

        try { PmSpoof.install(this) } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof.install (onCreate) failed: $t", t)
        }
        ParcelRewriter.ourPackageName = packageName

        try { nativeOnCreate() } catch (t: Throwable) {
            Log.e(TAG, "nativeOnCreate failed: $t", t)
        }

        driveApplicationProxyOnCreate()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        try {
            ApplicationProxy.getInstance().onConfigurationChanged(newConfig)
        } catch (t: Throwable) {
            Log.w(TAG, "ApplicationProxy.onConfigurationChanged: $t")
        }
    }

    override fun onTerminate() {
        try { ApplicationProxy.getInstance().onTerminate() } catch (t: Throwable) {
            Log.w(TAG, "ApplicationProxy.onTerminate: $t")
        }
        try { ApplicationProxy.getInstance().onDestroy() } catch (t: Throwable) {
            Log.w(TAG, "ApplicationProxy.onDestroy: $t")
        }
        super.onTerminate()
    }

    private fun tryWireApplicationProxy(base: Context) {
        val apk = File(base.filesDir, GUEST_APK)
        if (!apk.exists()) {
            Log.d(TAG, "tryWireApplicationProxy: guest APK not staged — skip")
            return
        }
        try {
            if (apk.canWrite() && !apk.setReadOnly()) {
                Log.e(TAG, "tryWireApplicationProxy: chmod failed")
                return
            }

            val pm = base.packageManager
            val archive = pm.getPackageArchiveInfo(
                apk.absolutePath,
                android.content.pm.PackageManager.GET_META_DATA
            ) ?: run {
                Log.e(TAG, "tryWireApplicationProxy: getPackageArchiveInfo null")
                return
            }
            val guestPkg = archive.packageName ?: run {
                Log.e(TAG, "tryWireApplicationProxy: no packageName")
                return
            }
            val guestAi = archive.applicationInfo ?: run {
                Log.e(TAG, "tryWireApplicationProxy: no applicationInfo")
                return
            }
            guestAi.sourceDir = apk.absolutePath
            guestAi.publicSourceDir = apk.absolutePath

            val libDir = File(base.filesDir, GUEST_LIB_DIR).apply { mkdirs() }
            try {
                ZipFile(apk.absolutePath).use { zip ->
                    for (e in zip.entries()) {
                        if (!e.name.startsWith("lib/arm64-v8a/")) continue
                        if (!e.name.endsWith(".so")) continue
                        val out = File(libDir, e.name.substringAfterLast('/'))
                        if (!out.exists() || out.length() != e.size) {
                            zip.getInputStream(e).use { i ->
                                out.outputStream().use { o -> i.copyTo(o) }
                            }
                        }
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "tryWireApplicationProxy: lib extract: $t")
            }

            val loader = GuestClassLoader(apk.absolutePath, libDir.absolutePath, base.classLoader)

            val proxy = ApplicationProxy.getInstance()
            val guestRes = try {
                proxy.nativeBuildGuestResources(apk.absolutePath, base)
            } catch (t: Throwable) {
                Log.w(TAG, "nativeBuildGuestResources failed: $t", t)
                null
            }
            Log.d(TAG, "tryWireApplicationProxy: guestRes=${guestRes?.javaClass?.simpleName}")

            val guestCtx = GuestHost.GuestContext(base, guestPkg, guestAi, loader, guestRes)
            proxy.setBaseContext(guestCtx)
            Log.d(TAG, "tryWireApplicationProxy: base context set for $guestPkg")

            try {
                PmSpoof.guestArchives = PmSpoof.guestArchives + (guestPkg to apk.absolutePath)
                PmSpoof.signatureNullOutPackages =
                    PmSpoof.signatureNullOutPackages + guestPkg
            } catch (_: Throwable) {}
        } catch (t: Throwable) {
            Log.e(TAG, "tryWireApplicationProxy failed: $t", t)
        }
    }

    private fun driveApplicationProxyOnCreate() {
        try {
            val proxy = ApplicationProxy.getInstance()
            proxy.onCreate()   // native — attach + create
            val guest = ApplicationProxy.getGuestApplication()
            if (guest != null) {
                Log.d(TAG, "DualApplication.onCreate: guest booted — ${guest.javaClass.name}")
                try {
                    val guestPkg = guest.packageName
                    val apkPath = File(filesDir, GUEST_APK).absolutePath
                    if (!guestPkg.isNullOrEmpty() && File(apkPath).exists()) {
                        PmSpoof.guestArchives = PmSpoof.guestArchives + (guestPkg to apkPath)
                        Log.d(TAG, "DualApplication.onCreate: archive registered for $guestPkg")
                    }
                } catch (t: Throwable) {
                    Log.w(TAG, "DualApplication.onCreate: archive register: $t")
                }
            } else {
                Log.w(TAG, "DualApplication.onCreate: ApplicationProxy.onCreate did not boot guest (APK not staged?)")
            }
        } catch (t: Throwable) {
            Log.e(TAG, "driveApplicationProxyOnCreate failed: $t", t)
        }
    }
}