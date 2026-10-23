package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class RelocTest {

    @Before fun reset() = NativeTestBridge.uninstallAll()
    @After  fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun allRelocTestsPass() {
        val lines = NativeTestBridge.runSuite("reloc")
        assertTrue("reloc produced no result lines", lines.isNotEmpty())
        assertTrue(
            "reloc produced ${lines.size} cases, expected >= 40",
            lines.size >= 40
        )
        val failures = lines.filter { it.startsWith("FAIL|") }
        assertTrue(
            "reloc failures:\n" + failures.joinToString("\n"),
            failures.isEmpty()
        )
    }
}