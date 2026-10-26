package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 48 — Feature 2.7: register state inspection.
 *
 * AttachRegInspect installs a hook whose stub captures the full ARM64
 * register file (x0..x30, sp, nzcv, q0..q31) at target entry. Two modes:
 *   PROBE      — capture, skip body (caller sees arg0 as return)
 *   PASSTHRU   — capture, execute body (caller sees real return)
 *
 * Fixtures:
 *   fn_reginspect_gp(a, b) → a + b    (int, args in w0/w1)
 *   fn_reginspect_fp(a, b) → a + b    (float, args in s0/s1)
 *
 * Snapshot array layout (see NativeTestBridge):
 *   [0..31]   x[]      (x0..x30, sp_orig at [31])
 *   [32]      nzcv     (RAW mrs nzcv value — flags in bits 31:28;
 *                       bits 27:0 and 63:32 guaranteed zero per ARM ARM
 *                       C5.2.53; use (nzcv >> 28) & 0xF to read flags)
 *   [33]      pc
 *   [34]      slot_id
 *   [35..98]  q[]      (32 Q-regs × low/high u64 pairs)
 *
 * HookResult codes:
 *   HOOK_OK              = 0
 *   HOOK_ALREADY_HOOKED  = 8
 *   HOOK_NOT_HOOKED      = 9
 */
@RunWith(AndroidJUnit4::class)
class RegInspectTest {

    private val HOOK_OK = 0
    private val HOOK_ALREADY_HOOKED = 8
    private val HOOK_NOT_HOOKED = 9

    private val MODE_PROBE = 0
    private val MODE_PASSTHRU = 1

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
    }

    @After
    fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun reginspectCapturesGpArgs() {
        val t = NativeTestBridge.getRegInspectGpTargetAddr()
        assertTrue("target addr", t > 0x1000)

        // Baseline call before hook.
        assertEquals("baseline: 10+20=30", 30, NativeTestBridge.callRegInspectGp(10, 20))
        assertEquals("0 hooks active", 0, NativeTestBridge.regInspectCount())

        // Attach passthrough.
        assertEquals("attach", HOOK_OK,
            NativeTestBridge.attachRegInspect(t, MODE_PASSTHRU))
        assertEquals("1 hook", 1, NativeTestBridge.regInspectCount())

        // Call with distinctive args.
        val ret = NativeTestBridge.callRegInspectGp(101, 202)
        assertEquals("body executed: 101+202=303", 303, ret)

        val snap = NativeTestBridge.regInspectSnapshot(t)
        assertNotNull("snapshot must be populated", snap)
        snap!!

        // AAPCS64: int a in w0, int b in w1. Kernel might zero or sign
        // extend; check the low 32 bits.
        assertEquals("x0 = arg a", 101L, snap[0] and 0xFFFFFFFFL)
        assertEquals("x1 = arg b", 202L, snap[1] and 0xFFFFFFFFL)

        // pc = target address (dispatcher fills from slot.target).
        assertEquals("pc = target addr", t, snap[33])

        // sp_orig = valid stack pointer (not zero, plausible 64-bit range).
        val sp = snap[31]
        assertTrue("sp_orig nonzero", sp != 0L)
        assertTrue("sp_orig > 0x1000", sp > 0x1000L)

        // nzcv is the raw mrs nzcv value. Per ARM ARM C5.2.53 the flags
        // live in bits 31:28, and bits 27:0 + 63:32 are guaranteed zero.
        // Rule 12 safe: if we captured garbage (e.g. mode flag, uninit
        // memory), the low 28 bits would very likely be nonzero.
        val nzcv = snap[32]
        assertEquals("nzcv bits 27:0 must be zero", 0L, nzcv and 0x0FFFFFFFL)

        assertEquals("slot id is 0 (first attach)", 0L, snap[34])
    }

    @Test
    fun reginspectCapturesFpArgs() {
        val t = NativeTestBridge.getRegInspectFpTargetAddr()
        assertTrue("target addr", t > 0x1000)

        assertEquals("baseline: 1.5+2.5=4.0", 4.0f,
            NativeTestBridge.callRegInspectFp(1.5f, 2.5f), 0.0001f)

        assertEquals("attach", HOOK_OK,
            NativeTestBridge.attachRegInspect(t, MODE_PASSTHRU))

        val ret = NativeTestBridge.callRegInspectFp(1.5f, 2.5f)
        assertEquals("body executed: 1.5+2.5=4.0", 4.0f, ret, 0.0001f)

        val snap = NativeTestBridge.regInspectSnapshot(t)
        assertNotNull("snapshot", snap)
        snap!!

        // Float arg a is in s0 = low 32 bits of q0 = snap[35] & 0xFFFFFFFF.
        val q0Low = snap[35] and 0xFFFFFFFFL
        assertEquals("q0 low32 == bits(1.5f)",
            java.lang.Float.floatToRawIntBits(1.5f).toLong(), q0Low)

        // Float arg b is in s1 = low 32 bits of q1 = snap[37] & 0xFFFFFFFF.
        val q1Low = snap[37] and 0xFFFFFFFFL
        assertEquals("q1 low32 == bits(2.5f)",
            java.lang.Float.floatToRawIntBits(2.5f).toLong(), q1Low)
    }

    @Test
    fun reginspectPassthroughExecutesBody() {
        val t = NativeTestBridge.getRegInspectGpTargetAddr()

        assertEquals("attach passthru", HOOK_OK,
            NativeTestBridge.attachRegInspect(t, MODE_PASSTHRU))

        // Multiple calls — body must execute each time.
        assertEquals("call1: 5+7=12", 12, NativeTestBridge.callRegInspectGp(5, 7))
        assertEquals("call2: 100+1=101", 101, NativeTestBridge.callRegInspectGp(100, 1))
        assertEquals("call3: -3+3=0", 0, NativeTestBridge.callRegInspectGp(-3, 3))

        // Snapshot reflects the LAST call.
        val snap = NativeTestBridge.regInspectSnapshot(t)!!
        // -3 as int32, zero-extended to u64 low 32 bits = 0xFFFFFFFD
        assertEquals("x0 = -3 (last call, low32)",
            0xFFFFFFFDL, snap[0] and 0xFFFFFFFFL)
        assertEquals("x1 = 3 (last call)", 3L, snap[1] and 0xFFFFFFFFL)

        NativeTestBridge.detachRegInspect(t)
    }

    @Test
    fun reginspectProbeSkipsBody() {
        val t = NativeTestBridge.getRegInspectGpTargetAddr()

        // Verify body normally executes.
        assertEquals("baseline: 10+20=30", 30, NativeTestBridge.callRegInspectGp(10, 20))

        assertEquals("attach probe", HOOK_OK,
            NativeTestBridge.attachRegInspect(t, MODE_PROBE))

        // Probe mode: body skipped, caller sees x0 = arg0 (= 10).
        val ret = NativeTestBridge.callRegInspectGp(10, 20)
        assertEquals("probe returns arg0 (body skipped)", 10, ret)

        // Snapshot still populated correctly.
        val snap = NativeTestBridge.regInspectSnapshot(t)!!
        assertEquals("x0 = arg a", 10L, snap[0] and 0xFFFFFFFFL)
        assertEquals("x1 = arg b", 20L, snap[1] and 0xFFFFFFFFL)

        // Different call still captures + skips.
        val ret2 = NativeTestBridge.callRegInspectGp(77, 88)
        assertEquals("probe returns arg0 again", 77, ret2)
        val snap2 = NativeTestBridge.regInspectSnapshot(t)!!
        assertEquals("x0 = 77", 77L, snap2[0] and 0xFFFFFFFFL)

        NativeTestBridge.detachRegInspect(t)
    }

    @Test
    fun reginspectDetachRestoresBytes() {
        val t = NativeTestBridge.getRegInspectGpTargetAddr()

        val before = NativeTestBridge.readU32At(t)
        assertNotEquals("real instruction word", 0L, before)

        assertEquals("attach", HOOK_OK,
            NativeTestBridge.attachRegInspect(t, MODE_PASSTHRU))

        val during = NativeTestBridge.readU32At(t)
        assertNotEquals("install rewrote first insn", before, during)

        assertEquals("detach", HOOK_OK, NativeTestBridge.detachRegInspect(t))

        val after = NativeTestBridge.readU32At(t)
        assertEquals("original bytes restored byte-identically", before, after)
        assertEquals("0 hooks", 0, NativeTestBridge.regInspectCount())

        // Body callable normally after detach.
        assertEquals("body works after detach", 30,
            NativeTestBridge.callRegInspectGp(10, 20))

        // Second detach → not hooked.
        assertEquals("detach again", HOOK_NOT_HOOKED,
            NativeTestBridge.detachRegInspect(t))
    }

    @Test
    fun reginspectDuplicateRefused() {
        val t = NativeTestBridge.getRegInspectGpTargetAddr()

        assertEquals("first attach", HOOK_OK,
            NativeTestBridge.attachRegInspect(t, MODE_PASSTHRU))
        assertEquals("1 hook", 1, NativeTestBridge.regInspectCount())

        val rc = NativeTestBridge.attachRegInspect(t, MODE_PASSTHRU)
        assertEquals("duplicate refused", HOOK_ALREADY_HOOKED, rc)
        assertEquals("still 1 hook", 1, NativeTestBridge.regInspectCount())

        NativeTestBridge.detachRegInspect(t)
    }
}