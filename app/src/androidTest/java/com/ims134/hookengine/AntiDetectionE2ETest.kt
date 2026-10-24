package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Ignore
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
 * ─── ROM limitation (Rule 12 exemption, documented) ───────────────────────
 *
 * Three tests in this class (connectToFridaPortBlockedEvenWithListener,
 * connectToIdaPortBlockedEvenWithListener, connectToNonBlockedPortSucceeds)
 * require binding a listener to 127.0.0.1 on this API 37 / HyperOS device.
 * Every bind() attempt fails with SocketException: EPERM (Operation not
 * permitted) because Android 15+ restricts unprivileged loopback bind in
 * the instrumentation process (unless the app declares a foreground service
 * type that permits socket binding).
 *
 * Prior to Session 15, those three tests silently returned on bind failure
 * and were reported as PASSED by JUnit — a Rule 12 violation (a test that
 * cannot fail). Session 15 marks them @Ignore with this reason string so the
 * skip is visible in `am instrument -r` output (`run finished: 52 tests,
 * 0 failed, 3 ignored`).
 *
 * The equivalent decision-function coverage lives in:
 *   - AntiDetectionTest.allAntiDetectionTestsPass
 *     (asserts frida_port_27042/27043 logically blocked, ida_port_23946
 *      logically blocked, normal port 8080 allowed, non-loopback 8.8.8.8:27042
 *      allowed — the exact decision Fake_connect makes)
 *   - AntiDetectionTest also asserts connect_hook_installed == 1, which
 *     confirms the inline hook is actually in the process.
 *
 * Net: the logic is covered where it can be covered on this ROM. The E2E
 * connect tests are re-enabled on any device/API where loopback bind works.
 *
 * ─── Non-socket tests ─────────────────────────────────────────────────────
 *
 * javaFileHidingFiresHook does NOT require socket bind — it exercises the
 * file-path hook path (ShouldHidePath / Fake_stat) via a Java File.exists()
 * call. It remains live and can fail.
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

    // ─── Connect blocking E2E (ignored on this ROM — see class comment) ──

    @Ignore("Loopback bind EPERM on API 37 / HyperOS. " +
            "Covered by AntiDetectionTest.allAntiDetectionTestsPass " +
            "(decision-function asserts). Re-enable on devices where bind(127.0.0.1) works.")
    @Test
    fun connectToFridaPortBlockedEvenWithListener() {
        val server = tryListen(27042) ?: return
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

    @Ignore("Loopback bind EPERM on API 37 / HyperOS. See class comment.")
    @Test
    fun connectToIdaPortBlockedEvenWithListener() {
        val server = tryListen(23946) ?: return
        var connected = false
        try {
            Socket("127.0.0.1", 23946).use { connected = true }
        } catch (_: ConnectException) { /* expected */ }
        assertFalse("connect(127.0.0.1:23946) must fail", connected)
        server.close()
    }

    @Ignore("Loopback bind EPERM on API 37 / HyperOS. See class comment.")
    @Test
    fun connectToNonBlockedPortSucceeds() {
        val server = tryListen(55555) ?: return
        // Should not throw
        Socket("127.0.0.1", 55555).use { /* connected OK */ }
        server.close()
    }

    // ─── File hiding E2E (LIVE — no socket needed) ───────────────────────

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
        val readBack = NativeTestBridge.readFileRaw(testPath)
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