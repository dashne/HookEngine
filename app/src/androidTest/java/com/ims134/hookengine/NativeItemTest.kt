package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.Parameterized
import java.io.File

@RunWith(Parameterized::class)
class NativeItemTest(
    private val suite: String,
    private val expectedMin: Int
) {
    companion object {
        @JvmStatic
        @Parameterized.Parameters(name = "{0}")
        fun data(): List<Array<Any>> = listOf(
            arrayOf("item1", 6),
            arrayOf("item2", 5),
            arrayOf("item3", 3),
            arrayOf("item4", 4),
            arrayOf("item5", 5),
        )
    }

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val realDir = File(ctx.cacheDir, "item4_real").apply { mkdirs() }
        val fakeDir = File(ctx.cacheDir, "item4_fake").apply { mkdirs() }
        NativeTestBridge.setItem4Paths(realDir.absolutePath, fakeDir.absolutePath)
    }

    @After
    fun cleanup() {
        NativeTestBridge.uninstallAll()
    }

    @Test
    fun allCasesReportedPass() {
        val lines = NativeTestBridge.runSuite(suite)
        assertTrue("$suite produced no result lines", lines.isNotEmpty())
        assertTrue(
            "$suite produced ${lines.size} cases, expected >= $expectedMin",
            lines.size >= expectedMin
        )
        val failures = lines.filter { it.startsWith("FAIL|") }
        assertTrue(
            "$suite failures:\n" + failures.joinToString("\n"),
            failures.isEmpty()
        )
    }
}