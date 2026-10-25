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
            "stage Viber APK at ${apk.absolutePath} before running " +
                    "(see Session 18 setup commands)",
            apk.exists()
        )

        val result = GuestHost.load(ctx, apk.absolutePath)

        // Rule 12 — the honest assertion is that the harness ran and
        // produced a named outcome. Whether the guest booted is reported
        // in the outcome value and in guest-boot.txt, not asserted here.
        assertTrue("result.outcome must be set", result.outcome != "?")

        Log.d("DualCore.Guest",
            "GuestLoadTest: outcome=${result.outcome} elapsed=${result.elapsedMs}ms")

        // Session 18b probe — signature spoof against the guest package.
        // Expected pre-fix: realSigNulled=false (PmSpoof nulls only our own
        // package). Expected post-fix: realSigNulled=true.
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
            appendLine("classLoaderChained=${result.loaderInstalled}")
            appendLine("instantiated=${result.instantiated}")
            appendLine("attached=${result.attached}")
            appendLine("onCreated=${result.onCreated}")
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
            // (stale spoof.error line removed — SpoofProbe since 18c exposes
            // installedError/info0Error/sigError/archiveError individually,
            // all already written above)

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
            result.error?.let {
                appendLine("--- error ---")
                appendLine(it)
            }
        })
    }
}