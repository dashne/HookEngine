package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class BinderTransactHookTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    @After
    fun cleanup() {
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
    }

    /**
     * Isolated test — install only, no transact fire.
     *
     * Verifies the FindEntrySlot fix accepts the JIT-compiled JNI stub
     * (data_=0x48167000). If this fails, the hook isn't installable and
     * we know it before we risk a crash on the fire path.
     */
    @Test
    fun installSucceeds() {
        val ok = BinderTransactHook.install()
        assertTrue(
            "install failed: ${BinderTransactHook.lastError}",
            ok
        )
        assertTrue("isInstalled should report true", BinderTransactHook.isInstalled())
        BinderTransactHook.uninstall()
        assertTrue("isInstalled should report false after uninstall",
            !BinderTransactHook.isInstalled())
    }

    /**
     * Full fire test — installs, fires on PackageManager, verifies count.
     *
     * If the JIT stub is not actually callable as a JNI function, this
     * test process will SIGSEGV. That's the definitive signal that the
     * JIT path is not viable and we need to scan offsets 32/40/48 next.
     * The install test above still reports pass in that scenario.
     */
    @Test
    fun hookFiresAndPassesThrough() {
        val ok = BinderTransactHook.install()
        assertTrue(
            "install failed: ${BinderTransactHook.lastError}",
            ok
        )

        val before = BinderTransactHook.fireCount()

        @Suppress("DEPRECATION")
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        assertNotNull("getPackageInfo must succeed via passthrough", pi)

        val pkgs = ctx.packageManager.getInstalledPackages(0)
        assertTrue("getInstalledPackages must return non-empty", pkgs.isNotEmpty())

        val after = BinderTransactHook.fireCount()
        assertTrue(
            "hook should fire on PackageManager calls (before=$before after=$after)",
            after > before
        )

        android.util.Log.d(
            "DualCore",
            "BinderTransactHookTest: fired=${after - before} times, " +
                    "lastCode=${BinderTransactHook.lastCode()}"
        )

        BinderTransactHook.uninstall()
    }
}