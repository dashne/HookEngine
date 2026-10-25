package com.ims134.hookengine

import android.app.Application
import android.content.ContentProvider
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
 * Session 20 — DualHost guest loader.
 *
 * Rule 15: engine behavior in native. The guest Application boot
 * (instantiate + attachBaseContext + onCreate) is a native call into
 * GuestBoot.cpp, matching the reference's ApplicationProxy.onCreate
 * being a native method that constructs the guest Application.
 *
 * Kotlin's role is scaffolding: stage the APK, read manifest for
 * reporting, call native boot, drive provider installation, register
 * guest package for sig null-out.
 */
object GuestHost {
    private const val TAG = "DualCore.Guest"
    @Volatile private var installed = false

    private external fun nativeBootGuest(
        apkPath: String,
        guestContext: Context,
        guestLoader: ClassLoader
    ): Application?

    @Synchronized
    fun load(hostContext: Context, apkPath: String): Result {
        val result = Result(apkPath)

        // Session 20b — if DualApplication already booted the guest at
        // process start (APK was staged before launch), report that.
        // ApplicationProxy is the single source of truth for guest boot.
        val alreadyBooted = try { ApplicationProxy.getGuestApplication() } catch (_: Throwable) { null }
        if (alreadyBooted != null) {
            result.outcome = "booted-at-startup"
            result.instantiated = true
            result.attached = true
            result.onCreated = true
            result.guestApp = alreadyBooted
            result.guestPackage = alreadyBooted.packageName
            result.guestAppClass = alreadyBooted.javaClass.name
            result.guestLoaderClass = alreadyBooted.classLoader?.javaClass?.name
            result.loaderInstalled = true
            // Attempt providers for reporting
            try {
                val prov = installGuestProviders(alreadyBooted, apkPath)
                result.providersDeclared = prov.declaredCount
                result.providersAttempted = prov.attempted
                result.providersSucceeded = prov.succeeded
                result.providersFailed = prov.failed
                result.firstProviderError = prov.errors.firstOrNull()
                result.firstProviderStack = prov.firstErrorStack
            } catch (t: Throwable) {
                result.firstProviderError = "installGuestProviders threw: $t"
            }
            return result.finish()
        }

        if (installed) {
            result.outcome = "already-loaded"
            return result.finish()
        }
        try {
            // 0. APK existence + chmod read-only
            val apk = File(apkPath)
            if (!apk.exists()) {
                result.outcome = "apk-missing"
                result.error = "not found: $apkPath"
                return result.finish()
            }
            result.apkSize = apk.length()
            if (apk.canWrite() && !apk.setReadOnly()) {
                result.outcome = "apk-readonly-failed"
                result.error = "chmod failed on ${apk.path}"
                return result.finish()
            }

            // 1. Extract arm64-v8a native libs
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

            // 2. Read manifest for reporting
            val pm = hostContext.packageManager
            val flags = PackageManager.GET_META_DATA or
                    PackageManager.GET_ACTIVITIES or
                    PackageManager.GET_SERVICES or
                    PackageManager.GET_RECEIVERS or
                    PackageManager.GET_PROVIDERS
            val guestInfo = pm.getPackageArchiveInfo(apkPath, flags) ?: run {
                result.outcome = "no-archive-info"
                result.error = "getPackageArchiveInfo null"
                return result.finish()
            }
            val guestPkg = guestInfo.packageName ?: ""
            result.guestPackage = guestPkg
            val guestAppInfo = guestInfo.applicationInfo ?: run {
                result.outcome = "no-app-info"
                result.error = "no applicationInfo"
                return result.finish()
            }
            guestAppInfo.sourceDir = apkPath
            guestAppInfo.publicSourceDir = apkPath
            result.guestAppClass = guestAppInfo.className

            // 3. Native boot — Application construction + attach + onCreate
            // 3. Native boot — Application construction + attach + onCreate.
            // We construct one PathClassLoader in Kotlin, pass it to both:
            //  - GuestContext (so guest code's getClassLoader() returns it)
            //  - nativeBootGuest (so guest Application is instantiated via it)
            // This guarantees class identity: the loader Viber's code sees
            // via getClassLoader() is exactly the one its Application was
            // instantiated through. Session 18's fix preserved this; Session
            // 20's refactor lost it (attachBaseContext → StringsKt.i0).
            val guestLoader: ClassLoader = try {
                GuestClassLoader(apkPath, libDir.absolutePath, hostContext.classLoader)
            } catch (t: Throwable) {
                Log.e(TAG, "guest PathClassLoader ctor failed: $t", t)
                result.outcome = "loader-ctor-failed"
                result.error = t.toString()
                return result.finish()
            }
            result.guestLoaderClass = guestLoader.javaClass.name
            result.loaderInstalled = true

            val guestCtx = GuestContext(hostContext, guestPkg, guestAppInfo, guestLoader)
            val guestApp = nativeBootGuest(apkPath, guestCtx, guestLoader)
            if (guestApp == null) {
                result.outcome = "native-boot-failed"
                result.error = "nativeBootGuest returned null; see DualCore.Native log"
                return result.finish()
            }
            result.instantiated = true
            result.attached = true
            result.onCreated = true
            result.guestApp = guestApp
            Log.d(TAG, "native boot OK for $guestPkg")

            // 4. ContentProviders (Session 19 instrumentation)
            try {
                val prov = installGuestProviders(guestApp, apkPath)
                result.providersDeclared = prov.declaredCount
                result.providersAttempted = prov.attempted
                result.providersSucceeded = prov.succeeded
                result.providersFailed = prov.failed
                result.firstProviderError = prov.errors.firstOrNull()
                result.firstProviderStack = prov.firstErrorStack
            } catch (t: Throwable) {
                Log.e(TAG, "provider install threw: $t")
                result.firstProviderError = "installGuestProviders threw: $t"
            }

            // 5. Register guest for sig null-out
            try {
                PmSpoof.signatureNullOutPackages =
                    PmSpoof.signatureNullOutPackages + guestPkg
                Log.d(TAG, "registered guest sig null-out: $guestPkg")
            } catch (t: Throwable) {
                Log.w(TAG, "sig null-out registration: $t")
            }

            installed = true
            result.outcome = "booted"
            return result.finish()
        } catch (t: Throwable) {
            result.outcome = "loader-threw"
            result.error = stackOf(t)
            return result.finish()
        }
    }

    fun installGuestProviders(guestApp: Application, apkPath: String): ProviderResult {
        val result = ProviderResult()
        val pm = guestApp.packageManager

        val info = try {
            pm.getPackageArchiveInfo(apkPath, PackageManager.GET_PROVIDERS)
        } catch (t: Throwable) {
            result.error = "getPackageArchiveInfo threw: $t"
            return result
        }
        val providers = info?.providers
        if (providers.isNullOrEmpty()) {
            result.error = if (providers == null) "providers null" else "zero providers"
            return result
        }
        result.declaredCount = providers.size

        val guestInfo = guestApp.applicationInfo ?: info.applicationInfo ?: ApplicationInfo().apply {
            packageName = guestApp.packageName
            sourceDir = apkPath
            publicSourceDir = apkPath
        }
        guestInfo.sourceDir = apkPath
        guestInfo.publicSourceDir = apkPath
        if (guestInfo.packageName.isNullOrEmpty()) guestInfo.packageName = guestApp.packageName

        for (pin in providers) {
            if (pin == null) continue
            result.attempted++
            val rawName = pin.name ?: ""
            if (rawName.isEmpty()) {
                result.failed++
                result.errors += "<unnamed>: ProviderInfo.name null"
                continue
            }
            val fqName = when {
                rawName.startsWith(".") -> guestApp.packageName + rawName
                !rawName.contains(".") -> "${guestApp.packageName}.$rawName"
                else -> rawName
            }
            try {
                pin.applicationInfo = guestInfo
                if (pin.packageName.isNullOrEmpty()) pin.packageName = guestApp.packageName
                if (pin.processName.isNullOrEmpty()) pin.processName = guestApp.packageName

                val cls = Class.forName(fqName, true, guestApp.classLoader)
                val ctor = cls.getDeclaredConstructor().apply { isAccessible = true }
                val provider = ctor.newInstance() as ContentProvider
                provider.attachInfo(guestApp, pin)
                provider.onCreate()
                result.succeeded++
                result.installed += fqName
            } catch (t: Throwable) {
                val inner = (t as? java.lang.reflect.InvocationTargetException)
                    ?.targetException ?: t
                result.failed++
                result.errors += "$fqName: ${inner.javaClass.simpleName}: ${inner.message}"
                if (result.firstErrorStack == null) result.firstErrorStack = stackOf(inner)
            }
        }
        return result
    }

    fun probeGuestSignatureSpoof(hostContext: Context, guestPackage: String): SpoofProbe {
        val result = SpoofProbe()
        result.guestPackage = guestPackage
        val pm = hostContext.packageManager

        try {
            val installed = pm.getInstalledPackages(0)
            result.installedTotal = installed.size
            result.installedHasGuest = installed.any { it.packageName == guestPackage }
        } catch (t: Throwable) { result.installedError = t.toString() }

        try {
            val pi0 = pm.getPackageInfo(guestPackage, 0)
            result.info0Ok = true
            result.info0Version = pi0.versionName ?: "?"
        } catch (t: Throwable) { result.info0Error = t.toString() }

        try {
            @Suppress("DEPRECATION")
            val piSig = pm.getPackageInfo(guestPackage, PackageManager.GET_SIGNATURES)
            @Suppress("DEPRECATION")
            val sigs = piSig.signatures
            result.realSigCount = sigs?.size ?: 0
            result.realSigNulled = (sigs == null || sigs.isEmpty())
            if (sigs != null && sigs.isNotEmpty()) result.firstSigHash = sigs[0].hashCode()
        } catch (t: Throwable) { result.sigError = t.toString() }

        try {
            val apk = File(hostContext.filesDir, "viber.apk").absolutePath
            @Suppress("DEPRECATION")
            val api = pm.getPackageArchiveInfo(apk, PackageManager.GET_SIGNATURES)
            if (api != null) {
                @Suppress("DEPRECATION")
                val sigs = api.signatures
                result.archiveSigCount = sigs?.size ?: 0
            } else result.archiveError = "null"
        } catch (t: Throwable) { result.archiveError = t.toString() }
        return result
    }

    private fun stackOf(t: Throwable): String {
        val sw = StringWriter()
        t.printStackTrace(PrintWriter(sw))
        return sw.toString()
    }

    class GuestContext(
        private val hostBase: Context,
        private val guestPackage: String,
        private val guestAppInfo: ApplicationInfo,
        private val guestLoader: ClassLoader
    ) : ContextWrapper(hostBase) {
        override fun getPackageName(): String = guestPackage
        override fun getApplicationInfo(): ApplicationInfo = guestAppInfo
        override fun getApplicationContext(): Context = this
        override fun getClassLoader(): ClassLoader = guestLoader
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
        var providersDeclared: Int = 0
        var providersAttempted: Int = 0
        var providersSucceeded: Int = 0
        var providersFailed: Int = 0
        var firstProviderError: String? = null
        var firstProviderStack: String? = null
        private var t0: Long = System.currentTimeMillis()

        fun finish(): Result {
            elapsedMs = System.currentTimeMillis() - t0
            Log.d(TAG, "== guest-load ==")
            Log.d(TAG, "  outcome=$outcome elapsed=${elapsedMs}ms")
            Log.d(TAG, "  pkg=$guestPackage appClass=$guestAppClass")
            Log.d(TAG, "  loader=$guestLoaderClass installed=$loaderInstalled")
            Log.d(TAG, "  instantiated=$instantiated attached=$attached onCreated=$onCreated")
            Log.d(TAG, "  providers: declared=$providersDeclared attempted=$providersAttempted ok=$providersSucceeded fail=$providersFailed")
            firstProviderError?.let { Log.e(TAG, "  first provider error: $it") }
            error?.let { Log.e(TAG, "  error:\n$it") }
            return this
        }
    }

    class ProviderResult {
        var declaredCount: Int = 0
        var attempted: Int = 0
        var succeeded: Int = 0
        var failed: Int = 0
        val installed = mutableListOf<String>()
        val errors = mutableListOf<String>()
        var firstErrorStack: String? = null
        var error: String? = null
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
}