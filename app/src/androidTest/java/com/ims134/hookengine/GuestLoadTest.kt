package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class GuestLoadTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

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

        // Session 23 — Wall B probe: give Viber's JNI Engine IO_POOL thread
        // time to spawn and attempt Environment.getExternalStorageDirectory().
        // Then drain everything BinderTransactHook saw. The new code that
        // appears here (not 3, not 36 — both already handled) is the
        // IStorageManager.getVolumeList AIDL code we need to intercept in B.2.
        // Session 24 — milestone 3 probe: is Viber's JNI Engine done initializing
// after 60s, or is it stuck? If nothing crashes in 60s, engine likely
// finished. If a new FATAL appears past the 2s mark, that names Wall C.
        Thread.sleep(60_000)
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

            // Session 23 — Wall B code capture
            appendLine("--- ipc codes (during boot + 2s post) ---")
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