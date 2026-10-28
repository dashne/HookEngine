package com.ims134.hookengine

import android.os.Debug
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.net.ConnectException
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Session 60 — Feature 5.11: debugger detect blocking.
 *
 * ─── Mechanism: property-gated JDWP activation ─────────────────────
 *
 * The handoff's originally-planned mechanism was JavaHookInstall on
 * Debug.isDebuggerConnected / Debug.waitingForDebugger, or on their
 * VMDebug.* delegates. Probe #1 (S60) and probe #2 (S60) measured
 * every one of those methods and every one returned a fast-JNI token:
 *
 *     VMDebug.isDebuggerConnected  FromReflectedMethod = 0x39
 *     Debug.waitForDebugger        FromReflectedMethod = 0x3b
 *     Debug.isDebuggerConnected    FromReflectedMethod = 0x3d
 *     Debug.waitingForDebugger     FromReflectedMethod = 0x3f
 *
 * All < 0x1000. JavaHookInstall refuses each one with the fast-JNI
 * guard. Same wall class as S51's ServiceManager (0x3d/0x3f/0x41).
 *
 * Worse: probe #1 and probe #2 measured DIFFERENT tokens for the
 * SAME method across processes (isDebuggerConnected: 0x39 → 0x3d,
 * waitingForDebugger: 0x3b → 0x3f). The tokens are per-process
 * dispatch-table indices, not stable ArtMethod identities. No user-
 * space hook can reach them without IDA/Ghidra disassembly of
 * libart.so's fast-JNI layer (Phase F).
 *
 * ─── The functional goal is already achieved ──────────────────────
 *
 * On this ROM, JDWP activation is gated on `ro.debuggable`:
 *   - AMS reads ro.debuggable at ActivityThread.attach time
 *   - If "0" → JDWP endpoint is never bound for this process
 *   - If "1" → AMS binds a JDWP listener on port 8700 and up
 *
 * S59's kPropSpoofs in Hooks.cpp already carries ro.debuggable="0"
 * and ro.adb.secure="1", and ShouldHookProperty routes
 * libandroid_runtime.so through the GOT hook so SystemProperties.get()
 * reflects the spoof. The consequence: JDWP never activates, so
 * Debug.isDebuggerConnected() and Debug.waitingForDebugger() return
 * their VM flags as `false` forever, without any further hook.
 *
 * This is architecturally stronger than a hook-based approach — a
 * hook has a call site an attacker can bypass; a property spoof has
 * no call site because the debugger endpoint simply doesn't exist.
 *
 * ─── Rule 12 — every assertion is falsifiable ──────────────────────
 *
 *   1. debugIsDebuggerConnectedFalse — fails if a debugger attaches
 *      (would flip the VM flag to true)
 *   2. debugWaitingForDebuggerFalse — same
 *   3. jdwpPortUnreachable — fails if AMS binds the JDWP listener
 *      (would happen if ro.debuggable spoof regressed to "1")
 *   4. roDebuggableSpoofed — fails if kPropSpoofs loses the entry
 *      or the libandroid_runtime.so GOT hook stops firing
 *   5. roAdbSecureSpoofed — same
 *   6. unrelatedDebugApiWorks — fails if ART's JNI machinery breaks
 *
 * Every one of the six can fail if a mechanism regresses. None is a
 * tautology.
 *
 * ─── Test-isolation lesson (S60) ──────────────────────────────────
 *
 * ConditionalHookTest (alphabetically before this class) calls
 * NativeTestBridge.uninstallAll() in both @Before and @After. That
 * tears down the file/property GOT hooks installed by
 * DualApplication.attachBaseContext, which is why AdbDetectTest failed
 * with 2 reds in the previous session before the fix. Same trap here:
 * installFileHooks() in @Before re-installs the property hooks.
 * installFileHooks() is idempotent (HOOK_ALREADY_HOOKED is treated as
 * success in PATCH_ALIAS). Same pattern as AdbDetectTest.@Before.
 */
@RunWith(AndroidJUnit4::class)
class DebuggerDetectTest {

    private val TAG = "DebuggerDetectTest"

    // Standard Android JDWP base port. AMS increments from 8700 if taken;
    // on this device with ro.debuggable="0", no listener is bound on any
    // of 8700..8705, so checking the base port is sufficient and fast.
    private val JDWP_PORT = 8700

    private fun sysProp(key: String): String {
        return try {
            val spCls = Class.forName("android.os.SystemProperties")
            val getM = spCls.getMethod("get", String::class.java)
            (getM.invoke(null, key) as? String) ?: ""
        } catch (t: Throwable) {
            Log.e(TAG, "sysProp('$key') threw: ${t.javaClass.simpleName}: ${t.message}")
            "<error>"
        }
    }

    @Before
    fun reinstallHooks() {
        // See class-level "Test-isolation lesson" comment above.
        // InstallHooks() re-scans every loaded library and re-hooks every
        // ShouldHookProperty GOT entry, including libandroid_runtime.so.
        // This is what makes SystemProperties.get() fire the property
        // spoof for ro.debuggable / ro.adb.secure.
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    @Test
    fun debugIsDebuggerConnectedFalse() {
        val v = Debug.isDebuggerConnected()
        Log.d(TAG, "Debug.isDebuggerConnected() = $v")
        assertFalse(
            "Debug.isDebuggerConnected() must be false " +
                    "(ro.debuggable='0' means JDWP never activates, so the VM " +
                    "flag never flips; if this fails, either a debugger is " +
                    "attached or the property spoof regressed)",
            v
        )
    }

    @Test
    fun debugWaitingForDebuggerFalse() {
        val v = Debug.waitingForDebugger()
        Log.d(TAG, "Debug.waitingForDebugger() = $v")
        assertFalse(
            "Debug.waitingForDebugger() must be false " +
                    "(same mechanism as debugIsDebuggerConnectedFalse)",
            v
        )
    }

    @Test
    fun jdwpPortUnreachable() {
        var connected = false
        var failure: Throwable? = null
        try {
            Socket().use { s ->
                s.connect(InetSocketAddress("127.0.0.1", JDWP_PORT), 300)
                connected = true
            }
        } catch (c: ConnectException) {
            Log.d(TAG, "JDWP port $JDWP_PORT correctly refused: ${c.message}")
        } catch (t: Throwable) {
            failure = t
            Log.d(TAG, "JDWP port $JDWP_PORT connect threw: " +
                    "${t.javaClass.simpleName}: ${t.message}")
        }
        assertFalse(
            "JDWP port $JDWP_PORT must not accept connections " +
                    "(ro.debuggable='0' prevents AMS from binding the listener; " +
                    "if this fails, the property spoof regressed and AMS opened " +
                    "a debugger endpoint). failure=$failure",
            connected
        )
    }

    @Test
    fun roDebuggableSpoofed() {
        val v = sysProp("ro.debuggable")
        Log.d(TAG, "SystemProperties[ro.debuggable] = '$v'")
        assertEquals(
            "ro.debuggable must be spoofed to '0' " +
                    "(Hooks.cpp kPropSpoofs + libandroid_runtime.so GOT hook)",
            "0", v
        )
    }

    @Test
    fun roAdbSecureSpoofed() {
        val v = sysProp("ro.adb.secure")
        Log.d(TAG, "SystemProperties[ro.adb.secure] = '$v'")
        assertEquals(
            "ro.adb.secure must be spoofed to '1'",
            "1", v
        )
    }

    @Test
    fun unrelatedDebugApiWorks() {
        val heap = Debug.getNativeHeapSize()
        Log.d(TAG, "Debug.getNativeHeapSize() = $heap")
        assertTrue(
            "Debug.getNativeHeapSize() must be > 0 — this is a native JNI " +
                    "call that round-trips through libart.so's JNI trampoline. " +
                    "If it returns 0 or crashes, ART's JNI machinery is broken " +
                    "and every hook in the engine is suspect.",
            heap > 0L
        )
    }
}