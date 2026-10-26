package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Session 38 — nativeInit structural port verification.
 *
 * Asserts:
 *   1. nativeInit returns 0 on success
 *   2. At least one .so extracted into libPath
 *   3. At least one .dex extracted into libPath
 *
 * Uses a scratch directory (guest-libs-nativeinit-test) to avoid disturbing
 * the real guest-libs dir. Idempotency check on second call.
 */
@RunWith(AndroidJUnit4::class)
class NativeInitTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val tag = "NativeInitTest"

    @Test
    fun nativeInitExtractsSoAndDex() {
        val apk = File(ctx.filesDir, "viber.apk")
        assumeTrue("viber.apk must be staged at ${apk.absolutePath}", apk.exists())

        val scratch = File(ctx.filesDir, "guest-libs-nativeinit-test").apply {
            deleteRecursively(); mkdirs()
        }
        val proxy = ApplicationProxy.getInstance()

        val rc = proxy.nativeInit(apk.absolutePath, scratch.absolutePath, 0)
        Log.d(tag, "nativeInit returned rc=$rc")
        assertEquals("nativeInit must return 0", 0, rc)

        val files = scratch.listFiles() ?: emptyArray()
        val soFiles  = files.filter { it.name.endsWith(".so") }
        val dexFiles = files.filter { it.name.endsWith(".dex") }
        Log.d(tag, "extracted: so=${soFiles.size} dex=${dexFiles.size} total=${files.size}")
        soFiles.take(10).forEach { Log.d(tag, "  so  ${it.name} (${it.length()} bytes)") }
        dexFiles.take(10).forEach { Log.d(tag, "  dex ${it.name} (${it.length()} bytes)") }

        assertTrue("at least 1 .so extracted, got ${soFiles.size}", soFiles.isNotEmpty())
        assertTrue("at least 1 .dex extracted, got ${dexFiles.size}", dexFiles.isNotEmpty())

        // Idempotency: second call must also return 0 and not re-extract
        val rc2 = proxy.nativeInit(apk.absolutePath, scratch.absolutePath, 0)
        assertEquals("nativeInit idempotent return", 0, rc2)
        val files2 = scratch.listFiles() ?: emptyArray()
        assertEquals("no file count change on second call", files.size, files2.size)
    }
}