package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import android.os.Process

@RunWith(AndroidJUnit4::class)
class ProcessHooksTest {

    @Before
    fun reset() {
        NativeTestBridge.uninstallProcessHooks()
        NativeTestBridge.uninstallAll()
    }

    @After
    fun cleanup() {
        NativeTestBridge.uninstallProcessHooks()
        NativeTestBridge.uninstallAll()
    }

    @Test
    fun allProcessHooksPass() {
        val lines = NativeTestBridge.runSuite("process_hooks")
        assertTrue("process_hooks produced no result lines", lines.isNotEmpty())
        assertTrue(
            "process_hooks produced ${lines.size} cases, expected >= 9",
            lines.size >= 9
        )
        val failures = lines.filter { it.startsWith("FAIL|") }
        assertTrue(
            "process_hooks failures:\n" + failures.joinToString("\n"),
            failures.isEmpty()
        )
    }

    @Test
    fun spoofProcessStaticsApi37State() {
        // API 37: android.os.Process does NOT expose MY_UID / MY_PID as
        // static fields. spoofProcessStatics returns -2 ("fields absent"),
        // which is the correct, expected state — myUid()/myPid() are pure
        // fast-JNI natives, not field-backed. This test locks in the graceful
        // behaviour (no crash, predictable return code).
        try {
            val rc = NativeTestBridge.spoofProcessStatics(10000, 10000)
            assertTrue(
                "spoofProcessStatics returned $rc; expected 0 (fields spoofable) " +
                        "or -2 (fields absent on API 37)",
                rc == 0 || rc == -2
            )
        } finally {
            NativeTestBridge.restoreProcessStatics()
        }
    }

    @Test
    fun processMyUidIsNative() {
        // Process.myUid() must ignore any MY_UID field state. On API 37 the
        // field doesn't exist and this trivially holds; the assertion is the
        // canary that catches any future ART change that starts routing
        // myUid() through a field read.
        val orig = Process.myUid()
        try {
            NativeTestBridge.spoofProcessStatics(10000, 10000)
            assertEquals("myUid() must ignore MY_UID field", orig, Process.myUid())
        } finally {
            NativeTestBridge.restoreProcessStatics()
        }
    }

    @Test
    fun doesFrameworkSeeNativeHook() {
        // Whether Process.myPid() sees the inline getpid hook depends on
        // whether it routes through libc getpid() or a direct svc. Both
        // outcomes are acceptable — this test only verifies that install
        // does not corrupt the framework and uninstall restores reality.
        val realPid = Process.myPid()
        NativeTestBridge.installProcessHooks()
        val hookedPid = try {
            Process.myPid()
        } finally {
            NativeTestBridge.uninstallProcessHooks()
        }
        val restoredPid = Process.myPid()

        assertTrue(
            "myPid() must return real pid ($realPid) or virtual pid (10000), got $hookedPid",
            hookedPid == realPid || hookedPid == 10000
        )
        assertEquals(
            "myPid() must return to real pid after uninstall",
            realPid, restoredPid
        )
    }
}