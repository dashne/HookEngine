package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 45 — Feature 1.8: conditional hook.
 *
 * A conditional hook attaches a handler with an associated predicate. The
 * dispatcher evaluates the predicate with a read-only view of x0..x7 at
 * the site. Predicate true → handler fires. Predicate false → handler is
 * skipped (no entry callback, no body). In both cases the chain still
 * falls through to the trampoline, so the original function executes and
 * the caller observes its real return value.
 *
 * Predicates and handlers are 1:1 paired per chain slot; different
 * handlers on the same target can carry different predicates.
 *
 * Target: fn_attach_target (from Feature 1.5), returns x+1. Handlers
 * observe x0 (the int arg) and count calls.
 *
 * HookResult codes:
 *   HOOK_OK              = 0
 *   HOOK_NOT_HOOKED      = 9
 *   HOOK_INTERNAL_ERROR  = 10
 *   HOOK_TARGET_INVALID  = 11
 */
@RunWith(AndroidJUnit4::class)
class ConditionalHookTest {

    private val HOOK_OK = 0
    private val HOOK_NOT_HOOKED = 9

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
        NativeTestBridge.chainReset()
    }

    @After
    fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun condPredicateAcceptsHandlerFires() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val pred = NativeTestBridge.getChainPredAcceptAllAddr()
        assertTrue("target addr", t > 0x1000)
        assertTrue("h1 addr", h1 > 0x1000)
        assertTrue("pred addr", pred > 0x1000)

        // Baseline before attach.
        assertEquals("original(10)=11", 11, NativeTestBridge.callFnAttachTarget(10))
        assertEquals("no calls yet", 0, NativeTestBridge.chainCalls1())

        assertEquals("attach cond",
            HOOK_OK, NativeTestBridge.chainAttachCond(t, pred, h1))
        assertEquals("1 handler", 1, NativeTestBridge.chainHandlerCount(t))

        val during = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("return value preserved through cond chain", 11, during)
        assertEquals("handler fired exactly once", 1, NativeTestBridge.chainCalls1())
        assertEquals("handler saw x0=10", 10, NativeTestBridge.chainLastX0_1())
    }

    @Test
    fun condPredicateRejectsHandlerSkipped() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val pred = NativeTestBridge.getChainPredRejectAllAddr()

        assertEquals("attach cond",
            HOOK_OK, NativeTestBridge.chainAttachCond(t, pred, h1))

        // CRITICAL: even with the predicate rejecting, the original
        // function must execute and return its real value. Chain is not
        // an abort — it only gates whether the handler observes the call.
        val during = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("original still returns real value", 11, during)
        assertEquals("handler did NOT fire", 0, NativeTestBridge.chainCalls1())

        // Multiple calls — handler still never fires.
        repeat(4) { NativeTestBridge.callFnAttachTarget(10) }
        assertEquals("handler still did not fire after 5 calls total",
            0, NativeTestBridge.chainCalls1())
    }

    @Test
    fun condPredicateSeesRealX0() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val pred = NativeTestBridge.getChainPredAcceptAllAddr()

        assertEquals("attach cond",
            HOOK_OK, NativeTestBridge.chainAttachCond(t, pred, h1))

        NativeTestBridge.callFnAttachTarget(42)
        assertEquals("predicate was invoked", 1, NativeTestBridge.chainPredCalls())
        assertEquals("predicate saw x0=42", 42, NativeTestBridge.chainPredLastX0())
    }

    @Test
    fun condPerHandlerGranularity() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val h2 = NativeTestBridge.getChainHandler2Addr()
        val pred10 = NativeTestBridge.getChainPredX0Is10Addr()
        val pred20 = NativeTestBridge.getChainPredX0Is20Addr()

        assertEquals("attach h1 (pred x0==10)",
            HOOK_OK, NativeTestBridge.chainAttachCond(t, pred10, h1))
        assertEquals("attach h2 (pred x0==20)",
            HOOK_OK, NativeTestBridge.chainAttachCond(t, pred20, h2))
        assertEquals("2 handlers on chain", 2, NativeTestBridge.chainHandlerCount(t))

        // x0=10 — h1 fires, h2 skipped.
        NativeTestBridge.callFnAttachTarget(10)
        assertEquals("h1 fired for x0=10", 1, NativeTestBridge.chainCalls1())
        assertEquals("h2 did not fire for x0=10", 0, NativeTestBridge.chainCalls2())

        // x0=20 — h1 skipped, h2 fires.
        NativeTestBridge.callFnAttachTarget(20)
        assertEquals("h1 still 1 for x0=20", 1, NativeTestBridge.chainCalls1())
        assertEquals("h2 fired for x0=20", 1, NativeTestBridge.chainCalls2())

        // x0=15 — both skipped.
        NativeTestBridge.callFnAttachTarget(15)
        assertEquals("h1 still 1 for x0=15", 1, NativeTestBridge.chainCalls1())
        assertEquals("h2 still 1 for x0=15", 1, NativeTestBridge.chainCalls2())

        // Return value preserved across all three calls.
        assertEquals("original return preserved", 16,
            NativeTestBridge.callFnAttachTarget(15))
    }

    @Test
    fun condTransitionStress() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val pred10 = NativeTestBridge.getChainPredX0Is10Addr()

        // 5 install → fire-with-match → fire-with-non-match → detach cycles.
        // Each cycle exercises the predicate-to-replacement transition with
        // the exact behavior we care about: fires when predicate is true,
        // does not fire when false, original return value preserved.
        repeat(5) { cycle ->
            assertEquals("cycle $cycle attach",
                HOOK_OK, NativeTestBridge.chainAttachCond(t, pred10, h1))
            assertEquals("cycle $cycle 1 handler",
                1, NativeTestBridge.chainHandlerCount(t))

            val before = NativeTestBridge.chainCalls1()

            // Predicate true — handler must fire exactly once.
            NativeTestBridge.callFnAttachTarget(10)
            assertEquals("cycle $cycle fires on match",
                before + 1, NativeTestBridge.chainCalls1())

            // Predicate false — handler must NOT fire.
            NativeTestBridge.callFnAttachTarget(99)
            assertEquals("cycle $cycle does not fire on non-match",
                before + 1, NativeTestBridge.chainCalls1())

            assertEquals("cycle $cycle detach",
                HOOK_OK, NativeTestBridge.chainDetach(t, h1))
            assertEquals("cycle $cycle 0 handlers",
                0, NativeTestBridge.chainHandlerCount(t))
        }

        // After 5 full cycles the target is back to its original bytes.
        assertEquals("0 chains after stress", 0, NativeTestBridge.chainCount())
        assertEquals("original still returns 11", 11,
            NativeTestBridge.callFnAttachTarget(10))
    }

    @Test
    fun condDetachRestoresBytes() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val pred10 = NativeTestBridge.getChainPredX0Is10Addr()

        val before = NativeTestBridge.readU32At(t)
        assertNotEquals("target must have a real instruction word", 0L, before)

        assertEquals("attach cond",
            HOOK_OK, NativeTestBridge.chainAttachCond(t, pred10, h1))
        val during = NativeTestBridge.readU32At(t)
        assertNotEquals("install rewrote first insn", before, during)

        assertEquals("detach last handler",
            HOOK_OK, NativeTestBridge.chainDetach(t, h1))
        val after = NativeTestBridge.readU32At(t)
        assertEquals("original bytes restored byte-identically", before, after)
        assertEquals("0 chains active", 0, NativeTestBridge.chainCount())
        assertEquals("original returns 11", 11,
            NativeTestBridge.callFnAttachTarget(10))
    }
}