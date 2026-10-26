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
        if (base == null) { super.attachBaseContext(null); return }
        Log.d(TAG, "DualApplication.attachBaseContext: entering")
        try { nativeOnLoad(base.applicationInfo.sourceDir, base.applicationInfo.nativeLibraryDir) }
        catch (t: Throwable) { Log.e(TAG, "nativeOnLoad failed: $t", t) }
        try { nativeAttachBaseContext(base) }
        catch (t: Throwable) { Log.e(TAG, "nativeAttachBaseContext failed: $t", t) }
        try { ClassLoaderBridge.install() }
        catch (t: Throwable) { Log.e(TAG, "ClassLoaderBridge.install failed: $t", t) }
        try { PmSpoof.install(base) }
        catch (t: Throwable) { Log.e(TAG, "PmSpoof.install failed: $t", t) }
        Log.d(TAG, "DualApplication: hooks installed, calling super.attachBaseContext")
        super.attachBaseContext(base)
        tryWireApplicationProxy(base)
        initialized = true
    }

    override fun onCreate() {
        Log.d(TAG, "DualApplication.onCreate")
        super.onCreate()
        if (!initialized) { Log.e(TAG, "DualApplication.onCreate: not initialized"); return }
        try { PmSpoof.install(this) } catch (t: Throwable) { Log.e(TAG, "PmSpoof.install (onCreate): $t", t) }
        ParcelRewriter.ourPackageName = packageName
        try { nativeOnCreate() } catch (t: Throwable) { Log.e(TAG, "nativeOnCreate: $t", t) }
        driveApplicationProxyOnCreate()
    }

    override fun onConfigurationChanged(newConfig: Configuration) {
        super.onConfigurationChanged(newConfig)
        try { ApplicationProxy.getInstance().onConfigurationChanged(newConfig) }
        catch (t: Throwable) { Log.w(TAG, "ApplicationProxy.onConfigurationChanged: $t") }
    }

    override fun onTerminate() {
        try { ApplicationProxy.getInstance().onTerminate() } catch (_: Throwable) {}
        try { ApplicationProxy.getInstance().onDestroy() } catch (_: Throwable) {}
        super.onTerminate()
    }

    private fun tryWireApplicationProxy(base: Context) {
        val apk = File(base.filesDir, GUEST_APK)
        if (!apk.exists()) { Log.d(TAG, "tryWireApplicationProxy: guest APK not staged"); return }
        try {
            if (apk.canWrite() && !apk.setReadOnly()) {
                Log.e(TAG, "tryWireApplicationProxy: chmod failed"); return
            }
            val pm = base.packageManager
            val archive = pm.getPackageArchiveInfo(apk.absolutePath,
                android.content.pm.PackageManager.GET_META_DATA) ?: run {
                Log.e(TAG, "tryWireApplicationProxy: archive null"); return
            }
            val guestPkg = archive.packageName ?: run {
                Log.e(TAG, "tryWireApplicationProxy: no packageName"); return
            }
            val guestAi = archive.applicationInfo ?: run {
                Log.e(TAG, "tryWireApplicationProxy: no applicationInfo"); return
            }
            guestAi.sourceDir = apk.absolutePath
            guestAi.publicSourceDir = apk.absolutePath
            guestAi.nativeLibraryDir = libDirFor(base).absolutePath

            val libDir = libDirFor(base)
            try {
                ZipFile(apk.absolutePath).use { zip ->
                    for (e in zip.entries()) {
                        if (!e.name.startsWith("lib/arm64-v8a/")) continue
                        if (!e.name.endsWith(".so")) continue
                        val out = File(libDir, e.name.substringAfterLast('/'))
                        if (!out.exists() || out.length() != e.size) {
                            zip.getInputStream(e).use { i -> out.outputStream().use { o -> i.copyTo(o) } }
                        }
                    }
                }
            } catch (t: Throwable) { Log.w(TAG, "lib extract: $t") }

            val guestDataDir = File(base.filesDir, "guest-data/$guestPkg").apply { mkdirs() }

            val loader = GuestClassLoader(apk.absolutePath, libDir.absolutePath, base.classLoader)
            val proxy = ApplicationProxy.getInstance()
            proxy.setGuestDataDir(guestDataDir.absolutePath)

            val guestRes = try { proxy.nativeBuildGuestResources(apk.absolutePath, base) }
            catch (t: Throwable) { Log.w(TAG, "buildGuestResources: $t"); null }
            Log.d(TAG, "tryWireApplicationProxy: guestRes=${guestRes?.javaClass?.simpleName}")
            Log.d(TAG, "tryWireApplicationProxy: dataDir=${guestDataDir.absolutePath}")

            val guestCtx = GuestHost.GuestContext(base, guestPkg, guestAi, loader, guestRes)
            proxy.setBaseContext(guestCtx)

            try {
                PmSpoof.guestArchives = PmSpoof.guestArchives + (guestPkg to apk.absolutePath)
                PmSpoof.signatureNullOutPackages =
                    PmSpoof.signatureNullOutPackages + guestPkg
            } catch (_: Throwable) {}
            Log.d(TAG, "tryWireApplicationProxy: ready for $guestPkg")
        } catch (t: Throwable) { Log.e(TAG, "tryWireApplicationProxy failed: $t", t) }
    }

    private fun libDirFor(base: Context): File =
        File(base.filesDir, GUEST_LIB_DIR).apply { mkdirs() }

    private fun driveApplicationProxyOnCreate() {
        try {
            val proxy = ApplicationProxy.getInstance()
            proxy.onCreate()
            val guest = ApplicationProxy.getGuestApplication()
            if (guest != null) {
                Log.d(TAG, "DualApplication.onCreate: guest booted — ${guest.javaClass.name} " +
                        "base=${guest.baseContext?.javaClass?.name}")
            } else {
                Log.w(TAG, "DualApplication.onCreate: no guest booted")
            }
        } catch (t: Throwable) { Log.e(TAG, "driveApplicationProxyOnCreate: $t", t) }
    }
}