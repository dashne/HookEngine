package com.ims134.hookengine

import android.app.Application
import android.content.Context
import android.content.ContextWrapper
import android.content.pm.ApplicationInfo
import android.content.pm.PackageManager
import android.util.Log
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

            // Session 18 fix #1 — ART on API 34+ refuses DexFile loading
            // of any dex file that is writable by its owner. The check is
            // (st_mode & S_IWUSR) != 0 in DexFile.openDexFileNative. The
            // staged APK is user-writable because `run-as cp` preserves
            // mode; clear the write bits before PathClassLoader runs.
            if (apk.canWrite() && !apk.setReadOnly()) {
                result.outcome = "apk-readonly-failed"
                result.error = "cannot chmod ${apk.path} to read-only " +
                        "(required for ART 34+ dex loading)"
                return result.finish()
            }
            Log.d(TAG, "loading guest apk=$apkPath size=${result.apkSize} " +
                    "writable=${apk.canWrite()}")

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

            // 2. GuestClassLoader over the guest APK, parented to our loader.
            //    Guest-first lookup for bundled-dependency prefixes (kotlin.*,
            //    androidx.*, okhttp3.*, etc.) so Viber's own versions win over
            //    host's; parent-first for everything else so host engine
            //    classes stay reachable from guest code.
            //
            //    NO ClassLoaderBridge.installFor call. Prepending host dex
            //    elements to guest dexElements shadows the guest's own bundled
            //    dependencies and produces NoSuchMethodError on guest calls
            //    into its own stdlib (observed: StringsKt.i0 from
            //    ViberApplication.attachBaseContext). Parent delegation is
            //    sufficient and non-shadowing.
            val hostLoader = GuestHost::class.java.classLoader!!
            val guestLoader = GuestClassLoader(apkPath, libDir.absolutePath, hostLoader)
            result.guestLoaderClass = guestLoader.javaClass.name
            result.loaderInstalled = true
            Log.d(TAG, "guest loader: GuestClassLoader (parent-delegation, no dex merge)")

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
            // Application does NOT declare attachBaseContext; it is inherited
            // from ContextWrapper. getDeclaredMethod only searches the exact
            // class, so walk the hierarchy to find the actual declaration.
            var attachOwner: Class<*>? = Application::class.java
            var attachM: java.lang.reflect.Method? = null
            while (attachOwner != null) {
                try {
                    attachM = attachOwner.getDeclaredMethod(
                        "attachBaseContext", Context::class.java)
                    break
                } catch (_: NoSuchMethodException) {
                    attachOwner = attachOwner.superclass
                }
            }
            if (attachM == null) {
                result.outcome = "attach-method-missing"
                result.error =
                    "attachBaseContext(Context) not found in Application hierarchy"
                return result.finish()
            }
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

            // Session 18d — register guest package for signature null-out.
            // The host proxy already nulls its own package's signatures.
            // Registering the guest makes the proxy also null the guest's,
            // so guest code that reads its own PackageInfo.signatures sees
            // the same empty result a real multi-tenant install would
            // produce. Requirement surfaced by Session 18c probe; no
            // reference decomp needed (Rule 5 behavior match).
            try {
                PmSpoof.signatureNullOutPackages =
                    PmSpoof.signatureNullOutPackages + guestPkg
                Log.d(TAG, "registered guest package for sig null-out: $guestPkg")
            } catch (t: Throwable) {
                Log.w(TAG, "sig null-out registration failed: $t")
            }

            result.outcome = "booted"
            return result.finish()
        } catch (t: Throwable) {
            result.outcome = "loader-threw"
            result.error = stackOf(t)
            return result.finish()
        }
    }

    /**
     * Session 18c diagnostic matrix. One run yields answers to all the
     * "why can't we see Viber from AMS" questions:
     *   1. Is Viber in getInstalledPackages? (visibility, not uninstall)
     *   2. Does getPackageInfo(name, 0) work? (visibility vs flag-specific)
     *   3. Does getPackageInfo(name, GET_SIGNATURES) work? (the probe case)
     *   4. Does getPackageArchiveInfo(apk, GET_SIGNATURES) work? (fallback path)
     * Each row is reported honestly whether it passed or threw.
     */
    fun probeGuestSignatureSpoof(hostContext: Context, guestPackage: String): SpoofProbe {
        val result = SpoofProbe()
        result.guestPackage = guestPackage
        val pm = hostContext.packageManager

        // 1. getInstalledPackages(0) — is Viber in the visible set?
        try {
            val installed = pm.getInstalledPackages(0)
            result.installedTotal = installed.size
            result.installedHasGuest = installed.any { it.packageName == guestPackage }
            Log.d(TAG, "probe: getInstalledPackages total=${installed.size} " +
                    "hasGuest=${result.installedHasGuest}")
        } catch (t: Throwable) {
            result.installedError = t.toString()
            Log.e(TAG, "probe: getInstalledPackages threw $t")
        }

        // 2. getPackageInfo(name, 0)
        try {
            val pi0 = pm.getPackageInfo(guestPackage, 0)
            result.info0Ok = true
            result.info0Version = pi0.versionName ?: "?"
            Log.d(TAG, "probe: getPackageInfo(name,0) OK v=${result.info0Version}")
        } catch (t: Throwable) {
            result.info0Error = t.toString()
            Log.e(TAG, "probe: getPackageInfo(name,0) threw $t")
        }

        // 3. getPackageInfo(name, GET_SIGNATURES)
        try {
            @Suppress("DEPRECATION")
            val piSig = pm.getPackageInfo(guestPackage, PackageManager.GET_SIGNATURES)
            @Suppress("DEPRECATION")
            val sigs = piSig.signatures
            result.realSigCount = sigs?.size ?: 0
            result.realSigNulled = (sigs == null || sigs.isEmpty())
            if (sigs != null && sigs.isNotEmpty()) result.firstSigHash = sigs[0].hashCode()
            Log.d(TAG, "probe: getPackageInfo(name,SIG) OK sigCount=${sigs?.size ?: 0}")
        } catch (t: Throwable) {
            result.sigError = t.toString()
            Log.e(TAG, "probe: getPackageInfo(name,SIG) threw $t")
        }

        // 4. getPackageArchiveInfo(apk, GET_SIGNATURES)
        try {
            val apk = File(hostContext.filesDir, "viber.apk").absolutePath
            @Suppress("DEPRECATION")
            val api = pm.getPackageArchiveInfo(apk, PackageManager.GET_SIGNATURES)
            if (api != null) {
                @Suppress("DEPRECATION")
                val sigs = api.signatures
                result.archiveSigCount = sigs?.size ?: 0
                Log.d(TAG, "probe: getPackageArchiveInfo OK sigCount=${sigs?.size ?: 0}")
            } else {
                result.archiveError = "null"
            }
        } catch (t: Throwable) {
            result.archiveError = t.toString()
            Log.e(TAG, "probe: getPackageArchiveInfo threw $t")
        }
        return result
    }

    class SpoofProbe {
        var guestPackage: String = ""
        var installedTotal: Int = 0
        var installedHasGuest: Boolean = false
        var installedError: String? = null
        var info0Ok: Boolean = false
        var info0Version: String = ""
        var info0Error: String? = null
        var realSigCount: Int = 0
        var realSigNulled: Boolean = false
        var firstSigHash: Int = 0
        var sigError: String? = null
        var archiveSigCount: Int = 0
        var archiveError: String? = null
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
        var loaderInstalled: Boolean = false
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
            Log.d(TAG, "  loader=$guestLoaderClass chained=$loaderInstalled")
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