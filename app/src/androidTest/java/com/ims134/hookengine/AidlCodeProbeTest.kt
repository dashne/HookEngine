package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Diagnostic only. No assertions on the PM codes.
 *
 * Narrowed in Session 12 to codes 3 and 36 only. The broad sweep of
 * PackageManager methods (getApplicationInfo, checkPermission,
 * queryIntentActivities, getInstalledApplications, etc.) risked
 * bringing the whole instrumentation suite down if any future reply
 * parcel is malformed. Codes 3 (getPackageInfo) and 36
 * (getInstalledPackages) are the only two codes our rewriter touches,
 * so they are the only two worth mapping.
 *
 * Logcat tag "AidlProbe" will show the mapping.
 */
@RunWith(AndroidJUnit4::class)
class AidlCodeProbeTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Before
    fun setup() {
        val ok = BinderTransactHook.install()
        assertTrue("hook install failed: ${BinderTransactHook.lastError}", ok)
        Thread.sleep(200)
        BinderTransactHook.drainCodes()
    }

    @After
    fun cleanup() {
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
    }

    @Test
    fun probePackageManagerCodes() {
        android.util.Log.d("AidlProbe", "=== AidlCodeProbeTest start (narrowed) ===")

        fun probe(name: String, call: () -> Unit) {
            BinderTransactHook.drainCodes()
            Thread.sleep(50)
            BinderTransactHook.drainCodes()
            try {
                call()
            } catch (t: Throwable) {
                android.util.Log.d("AidlProbe",
                    "$name THREW: ${t.javaClass.simpleName}: ${t.message}")
                return
            }
            val codes = BinderTransactHook.drainCodes()
            android.util.Log.d("AidlProbe", "$name -> [${codes.joinToString(",")}]")
        }

        // Code 3 — getPackageInfo. GET_SIGNATURES reliably fires IPC on this ROM.
        probe("getPackageInfo(GET_SIGNATURES)") {
            @Suppress("DEPRECATION")
            ctx.packageManager.getPackageInfo(
                ctx.packageName, android.content.pm.PackageManager.GET_SIGNATURES)
        }
        probe("getPackageInfo(GET_SIGNING_CERTIFICATES)") {
            ctx.packageManager.getPackageInfo(
                ctx.packageName,
                android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES,
            )
        }

        // Code 36 — getInstalledPackages. Never cached.
        probe("getInstalledPackages(0)") {
            ctx.packageManager.getInstalledPackages(0)
        }
        probe("getInstalledPackages(GET_SIGNATURES)") {
            @Suppress("DEPRECATION")
            ctx.packageManager.getInstalledPackages(
                android.content.pm.PackageManager.GET_SIGNATURES)
        }

        android.util.Log.d("AidlProbe", "=== AidlCodeProbeTest end ===")
    }
}