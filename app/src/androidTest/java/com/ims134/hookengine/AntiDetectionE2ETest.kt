package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.net.ConnectException
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket

/**
 * End-to-end tests that prove the installed GOT hooks actually fire when
 * Java-side APIs call into them.
 *
 * Why this test cannot live in C++: `HookLibraryCallback` in Hooks.cpp
 * explicitly excludes libhookengine.so from GOT patching, so a C++ test
 * inside libhookengine calling open()/connect() bypasses the hook. Java
 * calls route through libjavacore.so's GOT, which IS patched. Only a
 * Kotlin-side test can prove the hook is reached.
 *
 * Failure of any of these tests means:
 *   - InstallHooks()/InstallAntiDetection() didn't install
 *   - The library-name matching in ShouldHookFileIO drifted
 *   - The decision function is correct but the hook pathway is broken
 *
 * All three are silent regressions that AntiDetectionTest (which calls
 * the decision functions directly) cannot detect.
 */
@RunWith(AndroidJUnit4::class)
class AntiDetectionE2ETest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val openServers = mutableListOf<ServerSocket>()
    private val createdFiles = mutableListOf<String>()

    @Before
    fun setup() {
        // Hooks are installed at DualApplication.attachBaseContext startup.
        // Re-invoking is idempotent and ensures state if a prior test removed them.
        val f = NativeTestBridge.installFileHooks()
        val a = NativeTestBridge.installAntiDetection()
        android.util.Log.d("DualCore", "AntiDetectionE2ETest setup: fileHooks=$f antiDetect=$a")
    }

    @After
    fun cleanup() {
        openServers.forEach { try { it.close() } catch (_: Throwable) {} }
        openServers.clear()
        createdFiles.forEach { NativeTestBridge.deleteFileRaw(it) }
        createdFiles.clear()
    }

    private fun tryListen(port: Int): ServerSocket? {
        return try {
            val s = ServerSocket()
            s.bind(InetSocketAddress("127.0.0.1", port))
            openServers.add(s)
            s
        } catch (t: Throwable) {
            android.util.Log.w("DualCore", "tryListen($port) failed: $t")
            null
        }
    }

    // ─── Connect blocking E2E ──────────────────────────────────────────

    /**
     * Start a real listener on Frida's port 27042, then attempt to connect.
     * If the connect hook is active: our Fake_connect refuses before the
     * kernel ever sees the listener → ConnectException.
     * If the connect hook is broken: connect succeeds (listener is up) → FAIL.
     *
     * This is the test that proves the hook is not silently misrouted.
     */
    @Test
    fun connectToFridaPortBlockedEvenWithListener() {
        val server = tryListen(27042) ?: run {
            android.util.Log.w("DualCore", "port 27042 busy — skipping test")
            return
        }
        var connected = false
        var err: Throwable? = null
        try {
            Socket("127.0.0.1", 27042).use { connected = true }
        } catch (t: Throwable) { err = t }

        assertFalse("connect(127.0.0.1:27042) must fail — hook should refuse before kernel",
            connected)
        assertTrue("expected ConnectException, got ${err?.javaClass?.name}: ${err?.message}",
            err is ConnectException)
        server.close()
    }

    @Test
    fun connectToIdaPortBlockedEvenWithListener() {
        val server = tryListen(23946) ?: run {
            android.util.Log.w("DualCore", "port 23946 busy — skipping test")
            return
        }
        var connected = false
        try {
            Socket("127.0.0.1", 23946).use { connected = true }
        } catch (_: ConnectException) { /* expected */ }
        assertFalse("connect(127.0.0.1:23946) must fail", connected)
        server.close()
    }

    /**
     * Sanity: non-blocked loopback port should connect normally.
     * Catches overreach: if Fake_connect refuses everything, this fails.
     */
    @Test
    fun connectToNonBlockedPortSucceeds() {
        val server = tryListen(55555) ?: run {
            android.util.Log.w("DualCore", "port 55555 busy — skipping test")
            return
        }
        // Should not throw
        Socket("127.0.0.1", 55555).use { /* connected OK */ }
        server.close()
    }

    // ─── File hiding E2E ───────────────────────────────────────────────

    /**
     * The definite test for file hiding: create a file on disk via raw
     * syscall (bypasses hooks), confirm it exists on disk, then check
     * Java's File.exists() which routes through libjavacore's patched GOT.
     *
     * Uses the test-only `.hookengine-test-hidden` marker added to
     * ShouldHidePath. Production apps never create files with that string,
     * so adding it to ShouldHidePath does not affect production behavior.
     */
    @Test
    fun javaFileHidingFiresHook() {
        val marker = ".hookengine-test-hidden"
        val testPath = File(ctx.filesDir, "probe$marker").absolutePath
        createdFiles.add(testPath)

        // Create file bypassing hooks (raw syscall)
        val wrote = NativeTestBridge.writeFileRaw(testPath, "content")
        assertTrue("failed to create test file $testPath via raw syscall", wrote)

        // Sanity: file exists on disk. Verified by raw stat, not File.exists()
        // (which would go through the hook).
        val readBack = NativeTestBridge.readFileRaw(testPath)  // see note below
        assertTrue("raw read of $testPath should return content; got '$readBack'",
            readBack == "content")

        // THE test: Java File.exists() should now return false because
        // ShouldHidePath hides any path with ".hookengine-test-hidden".
        // If the GOT hook is broken, exists() returns true → test fails.
        val exists = File(testPath).exists()
        assertFalse("File.exists() must return false — hook should hide path; " +
                "got true, hook is NOT firing from Java layer",
            exists)

        // Double-check the hook isn't returning -1 for ALL stat calls
        // (overreach). A normal path must still be visible.
        val normalPath = File(ctx.filesDir, "probe_normal_no_marker.txt").absolutePath
        createdFiles.add(normalPath)
        NativeTestBridge.writeFileRaw(normalPath, "content")
        assertTrue("normal file must remain visible — hook must not overreach",
            File(normalPath).exists())
    }
}