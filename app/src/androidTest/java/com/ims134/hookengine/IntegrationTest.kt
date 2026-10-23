package com.ims134.hookengine

import android.content.Intent
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

@RunWith(AndroidJUnit4::class)
class IntegrationTest {

    @Test
    fun selftestReportIsComplete() {
        val ctx = InstrumentationRegistry.getInstrumentation().targetContext
        val f = File(ctx.filesDir, "selftest.txt")
        if (f.exists()) f.delete()

        ctx.startService(Intent(ctx, RemoteService::class.java))

        val deadline = System.currentTimeMillis() + 60_000
        while (System.currentTimeMillis() < deadline) {
            if (f.exists()) {
                val lines = f.readLines()
                if (lines.any { it.startsWith("END ") }) break
            }
            Thread.sleep(500)
        }

        assertTrue("selftest.txt not written within 60s", f.exists())

        val lines = f.readLines()
        val end = lines.indexOfFirst { it.startsWith("END ") }
        assertTrue("selftest.txt has no END marker (truncated run)", end >= 0)

        val body = lines.take(end)
        val passes = body.count { it.startsWith("PASS|") }
        val fails  = body.filter { it.startsWith("FAIL|") }

        assertTrue("selftest failures:\n" + fails.joinToString("\n"), fails.isEmpty())
        assertEquals("expected 23 PASS entries", 23, passes)
    }
}