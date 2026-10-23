package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class RegressionTest {

    @Before fun reset() = NativeTestBridge.uninstallAll()
    @After  fun cleanup() = NativeTestBridge.uninstallAll()

    @Test fun bug1_page_prot_restored() {
        assertSuiteClean("regression_bug1", expectedMin = 1)
    }

    @Test fun bug2_dlopen_hooks_disabled() {
        assertSuiteClean("regression_bug2", expectedMin = 1)
    }

    @Test fun bug3_both_pm_fields_patched() {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        PmSpoof.install(ctx)
        val pm = ctx.packageManager
        val instField = pm.javaClass.getDeclaredField("mPM").apply { isAccessible = true }
        val staticField = Class.forName("android.app.ActivityThread")
            .getDeclaredField("sPackageManager").apply { isAccessible = true }
        val instProxy = instField.get(pm)
        val statProxy = staticField.get(null)
        assertNotNull("instance mPM not patched", instProxy)
        assertNotNull("static sPackageManager not patched", statProxy)
        assertEquals("both fields must hold the same proxy class",
            instProxy.javaClass, statProxy.javaClass)
    }

    @Test fun bug4_no_relay_leak() {
        assertSuiteClean("regression_bug4", expectedMin = 3)
    }

    @Test fun bug5_deferred_free() {
        assertSuiteClean("regression_bug5", expectedMin = 3)
    }

    private fun assertSuiteClean(suite: String, expectedMin: Int) {
        val lines = NativeTestBridge.runSuite(suite)
        assertTrue("$suite produced no result lines", lines.isNotEmpty())
        assertTrue("$suite produced ${lines.size} cases, expected >= $expectedMin",
            lines.size >= expectedMin)
        val failures = lines.filter { it.startsWith("FAIL|") }
        assertTrue("$suite failures:\n" + failures.joinToString("\n"), failures.isEmpty())
    }
}