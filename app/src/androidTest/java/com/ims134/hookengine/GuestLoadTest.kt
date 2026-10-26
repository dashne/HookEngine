package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class GuestLoadTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    /**
     * Session 41 — targeted uncaught-exception suppressor.
     *
     * Guest boot spawns background threads that crash on AMS verification
     * because the caller packageName is spoofed to `com.viber.voip` but the
     * uid still belongs to `com.ims134.hookengine`. Two known shapes:
     *
     *   (a) miui.contentcatcher.InterceptorProxy calls
     *       ProcessManager.addMiuiApplicationThread — SecurityException
     *       "Permission Denial: ProcessManager.addMiuiApplicationThread()".
     *
     *   (b) com.google.android.gms.common.internal.BaseGmsClient.getService
     *       — "Unknown calling package name 'com.viber.voip'". And
     *       ViberApplication.getLanguageUpdateController() →
     *       registerReceiverWithFeature — "Given caller package
     *       com.viber.voip is not running in process".
     *
     * Both are canary-state facts per PROJECT_STATE.md, not engine bugs.
     * Android kills the whole process on any uncaught background-thread
     * exception, which trips am instrument's retry loop (~20s each, 3
     * retries = ~60s pure noise per suite invocation).
     *
     * Suppressor is deliberately narrow:
     *
     *   - Must be SecurityException.
     *   - Message must mention com.viber.voip (the spoofed guest identity).
     *   - Message must match one of the four known AMS/MIUI rejections.
     *
     * Any throwable outside that narrow window propagates to the default
     * handler → process kill → am instrument -2 → Rule 12 preserved:
     * real bugs still surface.
     *
     * If Viber upgrades and the crash shapes change, remove this @Before
     * and re-verify — the suppressor should not need to survive upgrades.
     */
    @Before
    fun installCanaryCrashSuppressor() {
        val previous = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            if (isKnownGuestBootCrash(throwable)) {
                Log.w("DualCore.Guest",
                    "Suppressed canary-noise crash on ${thread.name}: " +
                            "${throwable.javaClass.simpleName}: ${throwable.message}")
            } else {
                Log.e("DualCore.Guest",
                    "Uncaught on ${thread.name}: " +
                            "${throwable.javaClass.simpleName}: ${throwable.message}")
                previous?.uncaughtException(thread, throwable)
            }
        }
    }

    private fun isKnownGuestBootCrash(t: Throwable): Boolean {
        if (t !is SecurityException) return false
        val msg = t.message ?: return false
        // Every known canary-noise shape mentions the spoofed guest package.
        // Engine-internal exceptions with other packages won't match.
        if (!msg.contains("com.viber.voip")) return false
        return msg.contains("addMiuiApplicationThread") ||
                msg.contains("registerReceiverWithFeature") ||
                msg.contains("Unknown calling package name") ||
                msg.contains("is not running in process")
    }

    @Test
    fun loadViberReportsOutcome() {
        val apk = File(ctx.filesDir, "viber.apk")
        assertTrue(
            "stage Viber APK at ${apk.absolutePath} before running",
            apk.exists()
        )

        // Session 23 — clear any codes from prior tests in the suite so the
        // post-boot drain only captures IPC produced by this run.
        BinderTransactHook.drainCodes()

        val result = GuestHost.load(ctx, apk.absolutePath)

        // Rule 12 — assert only that the harness produced a named outcome.
        // Whether the guest booted is data, not a gate.
        assertTrue("result.outcome must be set", result.outcome != "?")

        Log.d("DualCore.Guest",
            "GuestLoadTest: outcome=${result.outcome} elapsed=${result.elapsedMs}ms")

        // Session 41 — reduced from 60s to 3s.
        //
        // The 60s sleep was Session 23/24 diagnostic: give Viber's JNI Engine
        // IO_POOL thread time to spawn and attempt Environment.getExternalStorageDirectory(),
        // then drain BinderTransactHook's code buffer. Diagnostic only — no
        // assertion depends on it. Canary pass condition (outcome=booted-viber-order,
        // providers>=14, no new FATAL) is fully established by GuestHost.load()
        // returning. 3s still captures the early IPC burst. If fuller Wall-B
        // code capture is needed, restore the sleep locally without committing.
        Thread.sleep(3_000)
        val bootCodes = BinderTransactHook.drainCodes()
        val codeList = bootCodes.joinToString(",")
        Log.d("DualCore.Guest",
            "Guest boot IPC codes (n=${bootCodes.size}): $codeList")

        val spoof = GuestHost.probeGuestSignatureSpoof(
            ctx, result.guestPackage ?: "com.viber.voip")

        File(ctx.filesDir, "guest-boot.txt").writeText(buildString {
            appendLine("outcome=${result.outcome}")
            appendLine("elapsedMs=${result.elapsedMs}")
            appendLine("apk=${result.apkPath}")
            appendLine("apkSize=${result.apkSize}")
            appendLine("libsExtracted=${result.libsExtracted}")
            appendLine("guestPackage=${result.guestPackage}")
            appendLine("guestAppClass=${result.guestAppClass}")
            appendLine("guestLoaderClass=${result.guestLoaderClass}")
            appendLine("loaderInstalled=${result.loaderInstalled}")
            appendLine("instantiated=${result.instantiated}")
            appendLine("attached=${result.attached}")
            appendLine("onCreated=${result.onCreated}")
            result.error?.let {
                appendLine("--- error ---")
                appendLine(it)
            }

            // Session 23 — Wall B code capture (window reduced Session 41)
            appendLine("--- ipc codes (during boot + 3s post) ---")
            appendLine("ipcCodesRaw=$codeList")
            appendLine("ipcCodeCount=${bootCodes.size}")

            appendLine("--- pm-visibility probe ---")
            appendLine("guestPackage=${spoof.guestPackage}")
            appendLine("installedTotal=${spoof.installedTotal}")
            appendLine("installedHasGuest=${spoof.installedHasGuest}")
            spoof.installedError?.let { appendLine("installedError=$it") }
            appendLine("info0Ok=${spoof.info0Ok}")
            appendLine("info0Version=${spoof.info0Version}")
            spoof.info0Error?.let { appendLine("info0Error=$it") }
            appendLine("realSigCount=${spoof.realSigCount}")
            appendLine("realSigNulled=${spoof.realSigNulled}")
            appendLine("firstSigHash=${spoof.firstSigHash}")
            spoof.sigError?.let { appendLine("sigError=$it") }
            appendLine("archiveSigCount=${spoof.archiveSigCount}")
            spoof.archiveError?.let { appendLine("archiveError=$it") }

            appendLine("--- providers ---")
            appendLine("providersDeclared=${result.providersDeclared}")
            appendLine("providersAttempted=${result.providersAttempted}")
            appendLine("providersSucceeded=${result.providersSucceeded}")
            appendLine("providersFailed=${result.providersFailed}")
            result.firstProviderError?.let { appendLine("firstProviderError=$it") }
            result.firstProviderStack?.let {
                appendLine("--- first provider stack ---")
                appendLine(it)
            }
        })
    }
}