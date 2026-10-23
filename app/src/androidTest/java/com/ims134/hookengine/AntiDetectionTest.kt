package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AntiDetectionTest {

    @Before fun reset()   { NativeTestBridge.uninstallAll() }
    @After  fun cleanup() { NativeTestBridge.uninstallAll() }

    @Test
    fun allAntiDetectionTestsPass() {
        val lines = NativeTestBridge.runSuite("anti_detection")
        assertTrue("anti_detection produced no result lines", lines.isNotEmpty())
        assertTrue(
            "anti_detection produced ${lines.size} cases, expected >= 18",
            lines.size >= 18
        )
        val failures = lines.filter { it.startsWith("FAIL|") }
        assertTrue(
            "anti_detection failures:\n" + failures.joinToString("\n"),
            failures.isEmpty()
        )
    }
}