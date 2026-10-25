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

        val result = GuestHost.load(ctx, apk.absolutePath)

        // Rule 12 — assert only that the harness produced a named outcome.
        // Whether the guest booted is data, not a gate.
        assertTrue("result.outcome must be set", result.outcome != "?")

        Log.d("DualCore.Guest",
            "GuestLoadTest: outcome=${result.outcome} elapsed=${result.elapsedMs}ms")

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
            // (native diag now emitted directly to DualCore.Native logcat,
            // not captured in Result — see GuestBoot.cpp LOGD/LOGE)
            result.error?.let {
                appendLine("--- error ---")
                appendLine(it)
            }

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