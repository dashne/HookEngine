package com.ims134.hookengine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Feature 8.2 — Per-hook argument logging.
 *
 * Rule 12: every assertion below can fail.
 *
 * Capture semantics:
 *   - ChainHook → GP (x0..x7) + FP (d0..d7) captured per dispatch.
 *   - ReturnMod → GP only (its stub doesn't save q0..q7).
 *   - Ring buffer: 16 captures, wraps mod 16.
 *   - Disabled by default; per-site opt-in via hookArgLogEnable(target).
 *
 * Test plan (7 assertions, one per concrete falsifiable behavior):
 *   1. argLogDisabledByDefault          — opt-in discipline
 *   2. argLogCapturesGpArgs              — GP capture correctness (ChainHook)
 *   3. argLogCapturesFpArgs              — FP capture correctness (ChainHook)
 *   4. argLogRingWraps                   — ring buffer wraparound math
 *   5. argLogDisabledStopsCapturing      — disable actually stops writes
 *   6. argLogResetClearsButPreservesEnabled — reset semantics
 *   7. argLogRetModCapturesGpOnly        — second subsystem + documented
 *                                          FP asymmetry
 */
class HookArgLogTest {

    private fun cleanup() {
        try { NativeTestBridge.uninstallAll() } catch (_: Throwable) {}
        try { NativeTestBridge.chainReset() } catch (_: Throwable) {}
        try { NativeTestBridge.retModReset() } catch (_: Throwable) {}
    }

    @Test
    fun argLogDisabledByDefault() {
        cleanup()
        val gp = NativeTestBridge.getArgLogFixtureGpAddr()
        val h  = NativeTestBridge.getArgLogNoopHandlerAddr()
        assertTrue("chainAttach must succeed", NativeTestBridge.chainAttach(gp, h) == 0)

        // Target is registered as a ChainHook site...
        assertTrue("lookup must find the site", NativeTestBridge.hookArgLogLookup(gp))
        // ...but the arglog is disabled by default.
        assertEquals("count must be 0 (disabled by default)",
            0, NativeTestBridge.hookArgLogCount(gp))

        // A call while disabled must NOT capture.
        NativeTestBridge.callArgLogFixtureGp(1, 2, 3, 4, 5, 6, 7, 8)
        assertEquals("count must still be 0 after a call with capture disabled",
            0, NativeTestBridge.hookArgLogCount(gp))

        NativeTestBridge.chainDetach(gp, h)
    }

    @Test
    fun argLogCapturesGpArgs() {
        cleanup()
        val gp = NativeTestBridge.getArgLogFixtureGpAddr()
        val h  = NativeTestBridge.getArgLogNoopHandlerAddr()
        assertTrue(NativeTestBridge.chainAttach(gp, h) == 0)

        assertTrue("enable must succeed", NativeTestBridge.hookArgLogEnable(gp))
        // Sanity: ChainHook reports FP-capture capability.
        assertTrue("ChainHook must report FP capture", NativeTestBridge.hookArgLogCapturesFp(gp))

        // Fixture returns sum of args: 11+22+33+44+55+66+77+88 = 396.
        val ret = NativeTestBridge.callArgLogFixtureGp(11, 22, 33, 44, 55, 66, 77, 88)
        assertEquals("fixture return must be sum of args", 396, ret)

        assertEquals("count must be 1", 1, NativeTestBridge.hookArgLogCount(gp))
        val arr = NativeTestBridge.hookArgLogRead(gp, 0)
        assertNotNull("ring[0] must be readable after 1 capture", arr)
        arr!!
        assertEquals("array size", 17, arr.size)
        val expected = intArrayOf(11, 22, 33, 44, 55, 66, 77, 88)
        for (i in 0 until 8) {
            assertEquals("gp[$i] must equal ${expected[i]}",
                expected[i], arr[i].toInt())
        }
        assertEquals("pc must equal target entry",
            gp, arr[16])

        NativeTestBridge.chainDetach(gp, h)
    }

    @Test
    fun argLogCapturesFpArgs() {
        cleanup()
        val fp = NativeTestBridge.getArgLogFixtureFpAddr()
        val h  = NativeTestBridge.getArgLogNoopHandlerAddr()
        assertTrue(NativeTestBridge.chainAttach(fp, h) == 0)
        assertTrue(NativeTestBridge.hookArgLogEnable(fp))

        val ret = NativeTestBridge.callArgLogFixtureFp(1.5, 2.5, 3.5, 4.5)
        assertEquals("fixture return must be sum of args", 12.0, ret, 1e-9)

        assertEquals("count must be 1", 1, NativeTestBridge.hookArgLogCount(fp))
        val arr = NativeTestBridge.hookArgLogRead(fp, 0)
        assertNotNull("ring[0] must be readable", arr)
        arr!!
        // fp0..fp3 occupy arr[8..11] as raw 64-bit double bits.
        assertEquals(1.5, Double.fromBits(arr[8]),  1e-9)
        assertEquals(2.5, Double.fromBits(arr[9]),  1e-9)
        assertEquals(3.5, Double.fromBits(arr[10]), 1e-9)
        assertEquals(4.5, Double.fromBits(arr[11]), 1e-9)

        NativeTestBridge.chainDetach(fp, h)
    }

    @Test
    fun argLogRingWraps() {
        cleanup()
        val gp = NativeTestBridge.getArgLogFixtureGpAddr()
        val h  = NativeTestBridge.getArgLogNoopHandlerAddr()
        assertTrue(NativeTestBridge.chainAttach(gp, h) == 0)
        assertTrue(NativeTestBridge.hookArgLogEnable(gp))

        // 20 calls with arg0 = 1..20. Ring size = 16, so ring[0] holds
        // call #17, ring[3] holds #20, ring[4] still holds #5 (the last
        // call whose slot hasn't been overwritten since head wrapped).
        for (i in 1..20) {
            NativeTestBridge.callArgLogFixtureGp(i, 0, 0, 0, 0, 0, 0, 0)
        }

        assertEquals("count must be 20 (total calls)", 20,
            NativeTestBridge.hookArgLogCount(gp))

        val ring0 = NativeTestBridge.hookArgLogRead(gp, 0)
        assertNotNull(ring0)
        assertEquals("ring[0].gp0 must be 17 (call #17 overwrote ring[0] after wrap)",
            17, ring0!![0].toInt())

        val ring3 = NativeTestBridge.hookArgLogRead(gp, 3)
        assertNotNull(ring3)
        assertEquals("ring[3].gp0 must be 20 (most recent call)",
            20, ring3!![0].toInt())

        val ring4 = NativeTestBridge.hookArgLogRead(gp, 4)
        assertNotNull(ring4)
        assertEquals("ring[4].gp0 must be 5 (oldest retained call)",
            5, ring4!![0].toInt())

        NativeTestBridge.chainDetach(gp, h)
    }

    @Test
    fun argLogDisabledStopsCapturing() {
        cleanup()
        val gp = NativeTestBridge.getArgLogFixtureGpAddr()
        val h  = NativeTestBridge.getArgLogNoopHandlerAddr()
        assertTrue(NativeTestBridge.chainAttach(gp, h) == 0)
        assertTrue(NativeTestBridge.hookArgLogEnable(gp))

        for (i in 0 until 5) {
            NativeTestBridge.callArgLogFixtureGp(i, 0, 0, 0, 0, 0, 0, 0)
        }
        assertEquals("count must be 5 after 5 calls", 5,
            NativeTestBridge.hookArgLogCount(gp))

        assertTrue(NativeTestBridge.hookArgLogDisable(gp))

        for (i in 0 until 5) {
            NativeTestBridge.callArgLogFixtureGp(100 + i, 0, 0, 0, 0, 0, 0, 0)
        }
        assertEquals("count must still be 5 after 5 more calls while disabled",
            5, NativeTestBridge.hookArgLogCount(gp))

        NativeTestBridge.chainDetach(gp, h)
    }

    @Test
    fun argLogResetClearsButPreservesEnabled() {
        cleanup()
        val gp = NativeTestBridge.getArgLogFixtureGpAddr()
        val h  = NativeTestBridge.getArgLogNoopHandlerAddr()
        assertTrue(NativeTestBridge.chainAttach(gp, h) == 0)
        assertTrue(NativeTestBridge.hookArgLogEnable(gp))

        for (i in 0 until 3) {
            NativeTestBridge.callArgLogFixtureGp(i, 0, 0, 0, 0, 0, 0, 0)
        }
        assertEquals("count must be 3", 3, NativeTestBridge.hookArgLogCount(gp))

        assertTrue("reset must succeed", NativeTestBridge.hookArgLogReset(gp))
        assertEquals("count must be 0 after reset", 0,
            NativeTestBridge.hookArgLogCount(gp))
        assertTrue("lookup must still succeed (site still active)",
            NativeTestBridge.hookArgLogLookup(gp))

        for (i in 0 until 2) {
            NativeTestBridge.callArgLogFixtureGp(50 + i, 0, 0, 0, 0, 0, 0, 0)
        }
        assertEquals("count must be 2 after 2 more calls (enabled preserved across reset)",
            2, NativeTestBridge.hookArgLogCount(gp))

        NativeTestBridge.chainDetach(gp, h)
    }

    @Test
    fun argLogRetModCapturesGpOnly() {
        cleanup()
        // RetMod on the same GP fixture — proves the second subsystem
        // shares the arglog surface with GP-only capture (its stub does
        // not save q0..q7; §8.2 documents this asymmetry).
        val gp = NativeTestBridge.getArgLogFixtureGpAddr()
        val mod = NativeTestBridge.getArgLogRetModNoopAddr()
        assertTrue("attachRetMod must succeed", NativeTestBridge.attachRetMod(gp, mod) == 0)

        assertTrue(NativeTestBridge.hookArgLogLookup(gp))
        assertTrue("enable must succeed", NativeTestBridge.hookArgLogEnable(gp))
        assertFalse("RetMod must report GP-only (no FP capture)",
            NativeTestBridge.hookArgLogCapturesFp(gp))

        NativeTestBridge.callArgLogFixtureGp(11, 22, 33, 44, 55, 66, 77, 88)
        assertEquals("count must be 1", 1, NativeTestBridge.hookArgLogCount(gp))

        val arr = NativeTestBridge.hookArgLogRead(gp, 0)
        assertNotNull("ring[0] must be readable", arr)
        arr!!
        val expected = intArrayOf(11, 22, 33, 44, 55, 66, 77, 88)
        for (i in 0 until 8) {
            assertEquals("gp[$i] must equal ${expected[i]}",
                expected[i], arr[i].toInt())
        }
        // All FP fields must be zero (RetMod doesn't capture FP).
        for (i in 8 until 16) {
            assertEquals("fp slot ${i - 8} must be zero on RetMod", 0L, arr[i])
        }

        NativeTestBridge.detachRetMod(gp)
    }
}