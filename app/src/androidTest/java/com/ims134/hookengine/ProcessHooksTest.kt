package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class ProcessHooksTest {

    @Before fun reset() = NativeTestBridge.uninstallAll()
    @After  fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun allProcessHooksPass() {
        val lines = NativeTestBridge.runSuite("process_hooks")
        assertTrue("process_hooks produced no result lines", lines.isNotEmpty())
        assertTrue(
            "process_hooks produced ${lines.size} cases, expected >= 5",
            lines.size >= 5
        )
        val failures = lines.filter { it.startsWith("FAIL|") }
        assertTrue(
            "process_hooks failures:\n" + failures.joinToString("\n"),
            failures.isEmpty()
        )
    }
}