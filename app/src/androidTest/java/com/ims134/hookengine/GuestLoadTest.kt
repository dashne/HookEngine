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

        File(ctx.filesDir, "guest-boot.txt").writeText(buildString {
            appendLine("outcome=${result.outcome}")
            appendLine("elapsedMs=${result.elapsedMs}")
            appendLine("apk=${result.apkPath}")
            appendLine("apkSize=${result.apkSize}")
            appendLine("libsExtracted=${result.libsExtracted}")
            appendLine("guestPackage=${result.guestPackage}")
            appendLine("guestAppClass=${result.guestAppClass}")
            appendLine("guestLoaderClass=${result.guestLoaderClass}")
            appendLine("classLoaderChained=${result.classLoaderChained}")
            appendLine("instantiated=${result.instantiated}")
            appendLine("attached=${result.attached}")
            appendLine("onCreated=${result.onCreated}")
            result.error?.let {
                appendLine("--- error ---")
                appendLine(it)
            }
        })
    }
}