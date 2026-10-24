package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class Session11ProbeTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Before
    fun setup() {
        val ok = BinderTransactHook.install()
        assertTrue("hook install failed: ${BinderTransactHook.lastError}", ok)
        Thread.sleep(200)
        BinderTransactHook.drainCodes()
        Thread.sleep(50)
        BinderTransactHook.drainCodes()
    }

    @After
    fun cleanup() {
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
    }

    @Test
    fun confirmGetInstalledPackagesCode() {
        BinderTransactHook.drainCodes()
        ctx.packageManager.getInstalledPackages(0)
        val codes = BinderTransactHook.drainCodes()
        android.util.Log.d("Session11",
            "getInstalledPackages(0) fired codes: ${codes.toList()}")
        android.util.Log.d("Session11",
            "lastCode=${BinderTransactHook.lastCode()} " +
                    "fireCount=${BinderTransactHook.fireCount()} " +
                    "rewriteCount=${BinderTransactHook.rewriteCount()} " +
                    "rewriteEffective=${BinderTransactHook.rewriteEffectiveCount()}")
        assertTrue(
            "at least one code must fire on getInstalledPackages — got empty",
            codes.isNotEmpty()
        )
        assertTrue(
            "at least one rewrite should dispatch (effective will be 0 — " +
                    "code 36 is skipped today, that's expected on this build)",
            BinderTransactHook.rewriteCount() > 0
        )
    }

    @Test
    fun confirmGetPackageInfoCode() {
        BinderTransactHook.drainCodes()
        try {
            @Suppress("DEPRECATION")
            ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        } catch (_: Throwable) {}
        val codes = BinderTransactHook.drainCodes()
        android.util.Log.d("Session11",
            "getPackageInfo(0) fired codes: ${codes.toList()}")
    }
}