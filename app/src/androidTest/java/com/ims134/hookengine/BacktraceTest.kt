package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 53 — Feature 2.8: Backtrace from hook site.
 *
 * FP-chain walker. Starting from a captured (fp, pc) pair, walks the
 * ARM64 frame pointer chain by reading [fp+8] (saved LR = return address
 * into caller) and [fp+0] (caller's frame pointer) at each step.
 *
 * Reuses the register-snapshot values (x29, x30) already captured by
 * Feature 2.7 (RegInspect). At a hook site these are the values at target
 * ENTRY, so frame[0] is the caller of the hooked function and frame[1]
 * is the caller of that caller, and so on.
 *
 * Six falsifiable tests:
 *   1. btTriggerReturnsAtLeastTwoFrames   — capture works, self + caller
 *   2. btAllPcsAreExecutable              — every frame in PROT_EXEC map
 *   3. btAllFpsAreAlignedAndMonotonic     — 16-byte align + ascending
 *   4. btChainIncludesAllThreeFixtures    — 3-deep chain all present
 *   5. btFrameOneIsInCaller               — frame[1] is in bt_fixture_c
 *   6. btChainTerminatesWithinMax         — walk bounded, no loop
 *
 * Closes Category 2 (Memory primitives, 7/7).
 */
@RunWith(AndroidJUnit4::class)
class BacktraceTest {

    private val PROT_EXEC = 0x4

    @Before fun setup() {
        NativeTestBridge.uninstallAll()
    }

    @After fun cleanup() {
        NativeTestBridge.uninstallAll()
    }

    @Test
    fun btTriggerReturnsAtLeastTwoFrames() {
        val n = NativeTestBridge.btTrigger()
        assertTrue(
            "BacktraceCaptureHere must return >= 2 frames (self + caller), got $n",
            n >= 2)
        assertEquals(
            "btFrameCount() must match btTrigger() return",
            n, NativeTestBridge.btFrameCount())
    }

    @Test
    fun btAllPcsAreExecutable() {
        NativeTestBridge.btTrigger()
        val n = NativeTestBridge.btFrameCount()
        assertTrue("need at least 1 frame", n >= 1)
        for (i in 0 until n) {
            val pc = NativeTestBridge.btFramePc(i)
            assertTrue(
                "frame[$i] pc=0x${pc.toString(16)} must be >= 0x1000",
                pc >= 0x1000L)
            val region = NativeTestBridge.memFindRegionContaining(pc)
            assertNotNull(
                "frame[$i] pc=0x${pc.toString(16)} must be in a mapped region",
                region)
            val prot = region!![2].toInt()
            assertEquals(
                "frame[$i] pc=0x${pc.toString(16)} must be PROT_EXEC " +
                        "(actual prot=0x${Integer.toHexString(prot)})",
                PROT_EXEC, prot and PROT_EXEC)
        }
    }

    @Test
    fun btAllFpsAreAlignedAndMonotonic() {
        NativeTestBridge.btTrigger()
        val n = NativeTestBridge.btFrameCount()
        assertTrue("need >= 2 frames", n >= 2)
        var prevFp = 0L
        var checked = 0
        for (i in 0 until n) {
            val fp = NativeTestBridge.btFrameFp(i)
            if (fp == 0L) continue  // last frame can legitimately have fp=0
            checked++
            assertEquals(
                "frame[$i] fp=0x${fp.toString(16)} must be 16-byte aligned",
                0L, fp and 0xFL)
            if (prevFp != 0L) {
                assertTrue(
                    "frame[$i] fp=0x${fp.toString(16)} must be > prev " +
                            "fp=0x${prevFp.toString(16)} (monotonic ascending stack)",
                    fp > prevFp)
            }
            prevFp = fp
        }
        assertTrue("must have checked >= 1 fp", checked >= 1)
    }

    @Test
    fun btChainIncludesAllThreeFixtures() {
        val addrA = NativeTestBridge.btFixtureAAddr()
        val addrB = NativeTestBridge.btFixtureBAddr()
        val addrC = NativeTestBridge.btFixtureCAddr()
        assertTrue(
            "fixture addrs valid: a=0x${addrA.toString(16)} b=0x${addrB.toString(16)} c=0x${addrC.toString(16)}",
            addrA > 0x1000 && addrB > 0x1000 && addrC > 0x1000)

        NativeTestBridge.btRunChain()
        val n = NativeTestBridge.btFrameCount()
        assertTrue("3-deep chain must produce >= 3 frames, got $n", n >= 3)

        var sawA = false
        var sawB = false
        var sawC = false
        for (i in 0 until n) {
            val pc = NativeTestBridge.btFramePc(i)
            if (pc >= addrC && pc < addrC + 0x200) sawC = true
            if (pc >= addrB && pc < addrB + 0x200) sawB = true
            if (pc >= addrA && pc < addrA + 0x200) sawA = true
        }
        assertTrue(
            "frame set must include bt_fixture_c (addr=0x${addrC.toString(16)})",
            sawC)
        assertTrue(
            "frame set must include bt_fixture_b (addr=0x${addrB.toString(16)})",
            sawB)
        assertTrue(
            "frame set must include bt_fixture_a (addr=0x${addrA.toString(16)})",
            sawA)
    }

    @Test
    fun btFrameOneIsInCaller() {
        val addrC = NativeTestBridge.btFixtureCAddr()
        NativeTestBridge.btRunChain()
        val n = NativeTestBridge.btFrameCount()
        assertTrue("need >= 2 frames, got $n", n >= 2)
        val pc1 = NativeTestBridge.btFramePc(1)
        assertTrue(
            "frame[1] pc=0x${pc1.toString(16)} must be inside bt_fixture_c " +
                    "(addr=0x${addrC.toString(16)}, range +0x200). This is the " +
                    "return address into the function that called the capture " +
                    "site — proves the walker emits caller frames, not just the " +
                    "capture frame.",
            pc1 >= addrC && pc1 < addrC + 0x200)
    }

    @Test
    fun btChainTerminatesWithinMax() {
        NativeTestBridge.btTrigger()
        val n = NativeTestBridge.btFrameCount()
        assertTrue(
            "walk must terminate within BT_MAX_FRAMES=32 (got $n). Values " +
                    ">= 32 mean the cycle detection / monotonicity guard failed.",
            n in 1..31)
    }
}