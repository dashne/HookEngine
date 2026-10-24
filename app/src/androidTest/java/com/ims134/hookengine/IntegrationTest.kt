package com.ims134.hookengine

import android.content.Intent
import android.util.Log
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

        // Session 16: :remote startService flake. AMS occasionally marks
        // :remote "process is bad" and rejects startService with
        // SecurityException at the caller. Observed 2 fails / 5 runs across
        // Sessions 13-16. Passing runs show :remote starting cleanly in
        // logcat (DualApplication.attachBaseContext → onCreate → onStartCommand,
        // no code crash captured). The bad mark is AMS state, cleared after
        // a delay; retry with backoff. If all 3 attempts fail, the flake
        // is not transient this run and the test fails honestly (rule 12).
        var lastEx: SecurityException? = null
        var started = false
        for (attempt in 1..3) {
            try {
                ctx.startService(Intent(ctx, RemoteService::class.java))
                started = true
                break
            } catch (e: SecurityException) {
                lastEx = e
                Log.w("DualCore",
                    "IntegrationTest: startService attempt $attempt failed: ${e.message}")
                if (attempt < 3) Thread.sleep(2000L * attempt)
            }
        }
        if (!started) {
            throw AssertionError(
                "startService failed after 3 attempts (last: ${lastEx?.message})", lastEx)
        }

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