package com.ims134.hookengine

import android.Manifest
import android.os.Parcel
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
 * Installs tnFIN hook, then for each PackageManager method:
 *   1. Drain the code buffer (clear)
 *   2. Call the PM method (fires a transact)
 *   3. Drain again → codes that method produced
 *
 * Logcat tag "AidlProbe" will show the mapping:
 *   AidlProbe: getPackageInfo -> [3]
 *   AidlProbe: getInstalledPackages -> [27]
 *   ...
 *
 * Some methods may produce MULTIPLE codes (e.g. IPC permission check
 * followed by the actual query). All are listed. The code we rewrite
 * against must be the one whose reply parcel contains PackageInfo /
 * ApplicationInfo — the Session 9 step 2 parser handles that.
 */
@RunWith(AndroidJUnit4::class)
class AidlCodeProbeTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Before
    fun setup() {
        val ok = BinderTransactHook.install()
        assertTrue("hook install failed: ${BinderTransactHook.lastError}", ok)
        // Stabilization: let any startup/background transacts finish, then drain
        Thread.sleep(200)
        BinderTransactHook.drainCodes()
    }

    @After
    fun cleanup() {
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
    }

    @Test
    fun probePackageManagerCodes() {
        android.util.Log.d("AidlProbe", "=== AidlCodeProbeTest start ===")

        fun probe(name: String, call: () -> Unit) {
            // Clear buffer
            BinderTransactHook.drainCodes()
            // Give the framework a moment to settle (background transacts)
            Thread.sleep(50)
            BinderTransactHook.drainCodes()
            // Call the method
            try {
                call()
            } catch (t: Throwable) {
                android.util.Log.d("AidlProbe", "$name THREW: ${t.javaClass.simpleName}: ${t.message}")
                return
            }
            // Read codes fired during the call
            val codes = BinderTransactHook.drainCodes()
            android.util.Log.d("AidlProbe", "$name -> [${codes.joinToString(",")}]")
        }

        // getPackageInfo with varying flags
        probe("getPackageInfo(0)") {
            @Suppress("DEPRECATION")
            ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        }
        probe("getPackageInfo(GET_SIGNATURES)") {
            @Suppress("DEPRECATION")
            ctx.packageManager.getPackageInfo(ctx.packageName, android.content.pm.PackageManager.GET_SIGNATURES)
        }
        probe("getPackageInfo(GET_SIGNING_CERTIFICATES)") {
            ctx.packageManager.getPackageInfo(
                ctx.packageName,
                android.content.pm.PackageManager.GET_SIGNING_CERTIFICATES,
            )
        }
        probe("getPackageInfo(nonexistent)") {
            try {
                @Suppress("DEPRECATION")
                ctx.packageManager.getPackageInfo("com.nonexistent.fake.pkg.xyz", 0)
            } catch (_: Throwable) {}
        }

        // Installed lists
        probe("getInstalledPackages(0)") {
            ctx.packageManager.getInstalledPackages(0)
        }
        probe("getInstalledPackages(GET_SIGNATURES)") {
            @Suppress("DEPRECATION")
            ctx.packageManager.getInstalledPackages(android.content.pm.PackageManager.GET_SIGNATURES)
        }
        probe("getInstalledPackages(GET_META_DATA)") {
            ctx.packageManager.getInstalledPackages(android.content.pm.PackageManager.GET_META_DATA)
        }
        probe("getInstalledApplications(0)") {
            ctx.packageManager.getInstalledApplications(0)
        }

        // Other common methods Viber / Telegram check
        probe("checkPermission(INTERNET)") {
            ctx.packageManager.checkPermission(Manifest.permission.INTERNET, ctx.packageName)
        }
        probe("checkPermission(CAMERA)") {
            ctx.packageManager.checkPermission(Manifest.permission.CAMERA, ctx.packageName)
        }
        probe("getApplicationInfo(0)") {
            ctx.packageManager.getApplicationInfo(ctx.packageName, 0)
        }
        probe("getPackagesForUid(selfUid)") {
            ctx.packageManager.getPackagesForUid(android.os.Process.myUid())
        }
        probe("getPackageUid(self)") {
            ctx.packageManager.getPackageUid(ctx.packageName, 0)
        }
        probe("resolveActivity(MAIN)") {
            val intent = android.content.Intent(android.content.Intent.ACTION_MAIN)
                .addCategory(android.content.Intent.CATEGORY_LAUNCHER)
                .setPackage(ctx.packageName)
            ctx.packageManager.resolveActivity(intent, 0)
        }
        probe("queryIntentActivities(MAIN)") {
            val intent = android.content.Intent(android.content.Intent.ACTION_MAIN)
                .addCategory(android.content.Intent.CATEGORY_LAUNCHER)
            ctx.packageManager.queryIntentActivities(intent, 0)
        }

        android.util.Log.d("AidlProbe", "=== AidlCodeProbeTest end ===")
    }
}