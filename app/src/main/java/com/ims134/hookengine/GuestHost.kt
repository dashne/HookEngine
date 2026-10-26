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

object GuestHost {
    private const val TAG = "DualCore.Guest"
    @Volatile private var installed = false

    @Synchronized
    fun load(hostContext: Context, apkPath: String): Result {
        val result = Result(apkPath)

        val alreadyBooted = try {
            ApplicationProxy.getGuestApplication()
        } catch (_: Throwable) { null }

        if (alreadyBooted != null) {
            result.guestApp = alreadyBooted
            result.guestPackage = alreadyBooted.packageName
            result.guestAppClass = alreadyBooted.javaClass.name
            result.guestLoaderClass = alreadyBooted.classLoader?.javaClass?.name
            result.loaderInstalled = true
            result.instantiated = true
            result.attached = true
            result.onCreated = true
            result.outcome = "booted-at-startup"
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

            val proxy = ApplicationProxy.getInstance()
            val guestRes = try {
                proxy.nativeBuildGuestResources(apkPath, hostContext)
            } catch (t: Throwable) {
                Log.w(TAG, "nativeBuildGuestResources failed: $t")
                null
            }
            Log.d(TAG, "guestRes=${guestRes?.javaClass?.simpleName}")

            val guestCtx = GuestContext(hostContext, guestPkg, guestAppInfo, guestLoader, guestRes)
            proxy.setBaseContext(guestCtx)
            proxy.onCreate()

            val guestApp = ApplicationProxy.getGuestApplication()
            if (guestApp == null) {
                result.outcome = "native-oncreate-failed"
                result.error = "ApplicationProxy.onCreate did not boot guest; see DualCore.Native log"
                return result.finish()
            }
            result.instantiated = true
            result.attached = true
            result.onCreated = true
            result.guestApp = guestApp

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

            try {
                PmSpoof.guestArchives = PmSpoof.guestArchives + (guestPkg to apkPath)
                PmSpoof.signatureNullOutPackages =
                    PmSpoof.signatureNullOutPackages + guestPkg
                Log.d(TAG, "registered guest archive + sig null-out: $guestPkg")
            } catch (t: Throwable) {
                Log.w(TAG, "guest archive registration: $t")
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

    /**
     * Session 21g — run block on main thread, 15s timeout, catch everything.
     *
     * Previous version re-threw exceptions from the block. On the main
     * Looper thread, an uncaught exception propagates through
     * Looper.loop() → uncaught handler → process killed. That killed
     * the instrumentation when ViberContactsProvider.onCreate threw
     * bb2.r5 NPE.
     *
     * Contract now:
     *   - Block is called on the main Looper thread.
     *   - Any exception thrown by the block is caught and logged here.
     *   - Return true if the block returned normally (with or without
     *     an internally-caught exception — caller checks lastEx).
     *   - Return false only on 15s timeout.
     *
     * Caller distinguishes outcomes via:
     *   completed && lastEx == null  → provider succeeded
     *   completed && lastEx != null  → provider threw (exception stored)
     *  !completed                    → provider hung (timeout, break loop)
     */
    private fun runOnMainThreadWithTimeout(timeoutMs: Long, block: () -> Unit): Boolean {
        val mainLooper = android.os.Looper.getMainLooper()
        if (android.os.Looper.myLooper() == mainLooper) {
            try {
                block()
                return true
            } catch (t: Throwable) {
                Log.e(TAG, "main-thread block threw (in-place): " +
                        "${t.javaClass.simpleName}: ${t.message}")
                return true
            }
        }
        val latch = java.util.concurrent.CountDownLatch(1)
        android.os.Handler(mainLooper).post {
            try {
                block()
            } catch (t: Throwable) {
                // Swallow — must never kill main thread.
                Log.e(TAG, "main-thread block threw (posted): " +
                        "${t.javaClass.simpleName}: ${t.message}")
            } finally {
                latch.countDown()
            }
        }
        val done = try {
            latch.await(timeoutMs, java.util.concurrent.TimeUnit.MILLISECONDS)
        } catch (e: InterruptedException) {
            Log.w(TAG, "main-thread block interrupted")
            false
        }
        if (!done) Log.e(TAG, "main-thread block timed out after ${timeoutMs}ms")
        return done
    }

    fun installGuestProviders(guestApp: Application, apkPath: String): ProviderResult {
        val result = ProviderResult()
        try {
            val gPkg = guestApp.packageName
            if (!gPkg.isNullOrEmpty()) {
                PmSpoof.guestArchives = PmSpoof.guestArchives + (gPkg to apkPath)
            }
        } catch (_: Throwable) {}
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
            val lastEx = arrayOfNulls<Throwable>(1)
            val startMs = System.currentTimeMillis()
            val completed = runOnMainThreadWithTimeout(15_000L) {
                try {
                    pin.applicationInfo = guestInfo
                    if (pin.packageName.isNullOrEmpty()) pin.packageName = guestApp.packageName
                    if (pin.processName.isNullOrEmpty()) pin.processName = guestApp.packageName
                    val cls = Class.forName(fqName, true, guestApp.classLoader)
                    val ctor = cls.getDeclaredConstructor().apply { isAccessible = true }
                    val provider = ctor.newInstance() as ContentProvider
                    provider.attachInfo(guestApp, pin)
                    provider.onCreate()
                } catch (t: Throwable) {
                    // Store and swallow — do NOT rethrow. Rethrowing would
                    // escape to Looper.loop() and kill the process.
                    lastEx[0] = t
                }
            }
            val elapsed = System.currentTimeMillis() - startMs

            if (!completed) {
                // Timeout — main thread stuck, break to avoid N×15s hangs.
                result.failed++
                result.errors += "$fqName: TIMEOUT (main thread stuck)"
                Log.e(TAG, "provider[$fqName] TIMEOUT — breaking loop")
                break
            }
            val t = lastEx[0]
            if (t != null) {
                val inner = (t as? java.lang.reflect.InvocationTargetException)
                    ?.targetException ?: t
                result.failed++
                result.errors += "$fqName: ${inner.javaClass.simpleName}: ${inner.message}"
                if (result.firstErrorStack == null) result.firstErrorStack = stackOf(inner)
                Log.e(TAG, "provider FAIL[$fqName] ${elapsed}ms: " +
                        "${inner.javaClass.simpleName}: ${inner.message}")
            } else {
                result.succeeded++
                result.installed += fqName
                Log.d(TAG, "provider OK[$fqName] ${elapsed}ms")
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
        hostBase: Context,
        private val guestPackage: String,
        private val guestAppInfo: ApplicationInfo,
        private val guestLoader: ClassLoader,
        private val guestResources: android.content.res.Resources? = null
    ) : ContextWrapper(hostBase) {
        override fun getPackageName(): String = guestPackage
        override fun getApplicationInfo(): ApplicationInfo = guestAppInfo
        override fun getClassLoader(): ClassLoader = guestLoader
        override fun getResources(): android.content.res.Resources =
            guestResources ?: super.getResources()

        override fun getApplicationContext(): Context {
            return try {
                ApplicationProxy.getGuestApplication() ?: super.getApplicationContext()
            } catch (_: Throwable) {
                super.getApplicationContext()
            }
        }
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