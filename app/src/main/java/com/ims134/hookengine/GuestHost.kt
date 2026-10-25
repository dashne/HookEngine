package com.ims134.hookengine

import android.app.Application
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.util.Log
import dalvik.system.PathClassLoader
import java.io.File
import java.io.PrintWriter
import java.io.StringWriter
import java.util.zip.ZipFile

/**
 * Session 18 — minimal ApplicationProxy-style guest loader.
 *
 * Loads a guest APK's Application into this process, calls its
 * attachBaseContext + onCreate with a Context that reports the guest's
 * identity, and captures the first failure with full stack trace.
 *
 * The purpose is NOT to make the guest boot. It is to surface the
 * first real integration wall — the point at which the 14/90 already
 * shipped is not enough, or the guest's assumptions about Context /
 * ClassLoader / native libs diverge from ours. The failure is the data.
 *
 * Rule 12: this harness reports its own outcome honestly. Success =
 * "harness ran, produced an outcome". Failure of the guest to boot is
 * a Result.outcome value, not an exception thrown at the caller.
 */
object GuestHost {
    private const val TAG = "DualCore.Guest"
    @Volatile private var installed = false

    @Synchronized
    fun load(hostContext: Context, apkPath: String): Result {
        val result = Result(apkPath)
        if (installed) {
            result.outcome = "already-loaded"
            return result.finish()
        }
        val t0 = System.currentTimeMillis()
        try {
            val apk = File(apkPath)
            if (!apk.exists()) {
                result.outcome = "apk-missing"
                result.error = "not found: $apkPath"
                return result.finish()
            }
            result.apkSize = apk.length()
            Log.d(TAG, "loading guest apk=$apkPath size=${result.apkSize}")

            // 1. Extract guest arm64-v8a native libs to a readable private dir.
            //    PathClassLoader's librarySearchPath must point at the dir
            //    containing the .so files directly (not a per-abi subdir).
            val libDir = File(hostContext.filesDir, "guest-libs").apply { mkdirs() }
            var extracted = 0
            try {
                ZipFile(apkPath).use { zip ->
                    for (entry in zip.entries()) {
                        if (!entry.name.startsWith("lib/arm64-v8a/")) continue
                        if (!entry.name.endsWith(".so")) continue
                        val out = File(libDir, entry.name.substringAfterLast('/'))
                        if (!out.exists() || out.length() != entry.size) {
                            zip.getInputStream(entry).use { input ->
                                out.outputStream().use { output -> input.copyTo(output) }
                            }
                            extracted++
                        }
                    }
                }
            } catch (t: Throwable) {
                Log.w(TAG, "native lib extraction partial: $t")
            }
            result.libsExtracted = extracted

            // 2. PathClassLoader over the guest APK, parented to our loader,
            //    with the extracted lib dir as native search path.
            val hostLoader = GuestHost::class.java.classLoader!!
            val guestLoader = PathClassLoader(apkPath, libDir.absolutePath, hostLoader)
            result.guestLoaderClass = guestLoader.javaClass.name

            // Chain host dex elements into guest so guest can see our classes
            // (Fake_connect, ParcelRewriter, PmSpoof, etc.).
            try {
                result.classLoaderChained = ClassLoaderBridge.installFor(guestLoader, hostLoader)
            } catch (t: Throwable) {
                result.classLoaderChained = false
                Log.w(TAG, "ClassLoaderBridge.installFor failed: $t")
            }

            // 3. Read guest Application class from its manifest.
            val pm = hostContext.packageManager
            val flags = PackageManager.GET_META_DATA or
                    PackageManager.GET_ACTIVITIES or
                    PackageManager.GET_SERVICES or
                    PackageManager.GET_RECEIVERS or
                    PackageManager.GET_PROVIDERS
            val guestInfo = pm.getPackageArchiveInfo(apkPath, flags) ?: run {
                result.outcome = "no-archive-info"
                result.error = "getPackageArchiveInfo returned null for $apkPath"
                return result.finish()
            }
            val guestPkg = guestInfo.packageName ?: ""
            result.guestPackage = guestPkg

            val guestAppInfo = guestInfo.applicationInfo ?: run {
                result.outcome = "no-app-info"
                result.error = "PackageInfo has no applicationInfo"
                return result.finish()
            }
            guestAppInfo.sourceDir = apkPath
            guestAppInfo.publicSourceDir = apkPath

            val guestAppClassName = guestAppInfo.className
            result.guestAppClass = guestAppClassName
            if (guestAppClassName.isNullOrEmpty()) {
                result.outcome = "no-app-class-name"
                result.error = "<application> has no android:name in guest manifest"
                return result.finish()
            }

            // 4. Instantiate guest Application via the guest loader.
            val guestAppCls = Class.forName(guestAppClassName, true, guestLoader)
            val guestApp = guestAppCls.getDeclaredConstructor()
                .apply { isAccessible = true }
                .newInstance() as Application
            result.instantiated = true

            // 5. Guest context that reports the guest's package identity.
            val guestCtx = GuestContext(hostContext, guestLoader, guestPkg, guestAppInfo)

            // 6. attachBaseContext (protected on ContextWrapper — reflection).
            val attachM = Application::class.java
                .getDeclaredMethod("attachBaseContext", Context::class.java)
            attachM.isAccessible = true
            try {
                attachM.invoke(guestApp, guestCtx)
                result.attached = true
            } catch (t: Throwable) {
                val inner = (t as? java.lang.reflect.InvocationTargetException)
                    ?.targetException ?: t
                result.outcome = "attach-failed"
                result.error = stackOf(inner)
                return result.finish()
            }

            // 7. onCreate (public on Application).
            try {
                guestApp.onCreate()
                result.onCreated = true
            } catch (t: Throwable) {
                result.outcome = "oncreate-failed"
                result.error = stackOf(t)
                return result.finish()
            }

            installed = true
            result.guestApp = guestApp
            result.outcome = "booted"
            return result.finish()
        } catch (t: Throwable) {
            result.outcome = "loader-threw"
            result.error = stackOf(t)
            return result.finish()
        }
    }

    private fun stackOf(t: Throwable): String {
        val sw = StringWriter()
        t.printStackTrace(PrintWriter(sw))
        return sw.toString()
    }

    class Result(val apkPath: String) {
        var outcome: String = "?"
        var error: String? = null
        var apkSize: Long = 0
        var libsExtracted: Int = 0
        var guestPackage: String? = null
        var guestAppClass: String? = null
        var guestLoaderClass: String? = null
        var classLoaderChained: Boolean = false
        var instantiated: Boolean = false
        var attached: Boolean = false
        var onCreated: Boolean = false
        var elapsedMs: Long = 0
        var guestApp: Application? = null
        private var t0: Long = System.currentTimeMillis()

        fun finish(): Result {
            elapsedMs = System.currentTimeMillis() - t0
            Log.d(TAG, "== guest-load ==")
            Log.d(TAG, "  outcome=$outcome elapsed=${elapsedMs}ms")
            Log.d(TAG, "  apk=$apkPath size=$apkSize libsExtracted=$libsExtracted")
            Log.d(TAG, "  pkg=$guestPackage appClass=$guestAppClass")
            Log.d(TAG, "  loader=$guestLoaderClass chained=$classLoaderChained")
            Log.d(TAG, "  instantiated=$instantiated attached=$attached onCreated=$onCreated")
            error?.let { Log.e(TAG, "  error:\n$it") }
            return this
        }
    }

    class GuestContext(
        base: Context,
        private val guestLoader: ClassLoader,
        private val guestPackage: String,
        private val guestAppInfo: ApplicationInfo
    ) : ContextWrapper(base) {
        override fun getPackageName(): String = guestPackage
        override fun getApplicationInfo(): ApplicationInfo = guestAppInfo
        override fun getClassLoader(): ClassLoader = guestLoader
    }
}