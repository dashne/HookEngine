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
 * Session 43 — Feature 1.7: hook chaining (ordered multi-handler).
 *
 * One physical inline hook per target, dispatcher runs N handlers in
 * registration order, then falls through to the trampoline (original
 * function). Handlers receive a read-only view of x0..x7.
 *
 * Target: fn_attach_target (from Feature 1.5), returns x+1. Handlers
 * observe x0 (the int arg) and count calls.
 *
 * HookResult codes referenced below:
 *   HOOK_OK              = 0
 *   HOOK_NOT_HOOKED      = 9
 *   HOOK_INTERNAL_ERROR  = 10
 *   HOOK_TARGET_INVALID  = 11
 */
@RunWith(AndroidJUnit4::class)
class ChainHookTest {

    private val HOOK_OK = 0
    private val HOOK_NOT_HOOKED = 9
    private val HOOK_INTERNAL_ERROR = 10

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
        NativeTestBridge.chainReset()
    }

    @After
    fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun chainOfTwoFiresInOrderAndPreservesResult() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val h2 = NativeTestBridge.getChainHandler2Addr()
        assertTrue("target addr", t > 0x1000)
        assertTrue("h1 addr", h1 > 0x1000)
        assertTrue("h2 addr", h2 > 0x1000)

        assertEquals("original(10)=11", 11, NativeTestBridge.callFnAttachTarget(10))
        assertEquals("no calls yet", 0, NativeTestBridge.chainCalls1())
        assertEquals("no calls yet", 0, NativeTestBridge.chainCalls2())

        assertEquals("attach h1", HOOK_OK, NativeTestBridge.chainAttach(t, h1))
        assertEquals("attach h2", HOOK_OK, NativeTestBridge.chainAttach(t, h2))
        assertEquals("2 handlers", 2, NativeTestBridge.chainHandlerCount(t))

        val during = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("return value preserved through chain", 11, during)

        assertEquals("h1 fired once", 1, NativeTestBridge.chainCalls1())
        assertEquals("h2 fired once", 1, NativeTestBridge.chainCalls2())
        assertEquals("h1 saw x0=10", 10, NativeTestBridge.chainLastX0_1())
        assertEquals("h2 saw x0=10", 10, NativeTestBridge.chainLastX0_2())
        assertEquals("h1 fired first", 1, NativeTestBridge.chainOrderFirst())
    }

    @Test
    fun chainDetachOneHandlerKeepsOther() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val h2 = NativeTestBridge.getChainHandler2Addr()
        NativeTestBridge.chainAttach(t, h1)
        NativeTestBridge.chainAttach(t, h2)

        NativeTestBridge.callFnAttachTarget(10)
        assertEquals(1, NativeTestBridge.chainCalls1())
        assertEquals(1, NativeTestBridge.chainCalls2())

        val rc = NativeTestBridge.chainDetach(t, h1)
        assertEquals("detach h1", HOOK_OK, rc)
        assertEquals("1 handler left", 1, NativeTestBridge.chainHandlerCount(t))

        NativeTestBridge.callFnAttachTarget(10)
        assertEquals("h1 removed — count unchanged", 1, NativeTestBridge.chainCalls1())
        assertEquals("h2 still fires", 2, NativeTestBridge.chainCalls2())
    }

    @Test
    fun chainDetachLastUninstallsAndRestoresBytes() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()

        val before = NativeTestBridge.readU32At(t)
        NativeTestBridge.chainAttach(t, h1)
        val during = NativeTestBridge.readU32At(t)
        assertNotEquals("install rewrote first insn", before, during)
        assertEquals("1 chain active", 1, NativeTestBridge.chainCount())

        assertEquals("detach last", HOOK_OK, NativeTestBridge.chainDetach(t, h1))
        val after = NativeTestBridge.readU32At(t)
        assertEquals("original bytes restored byte-identically", before, after)
        assertEquals("0 chains active", 0, NativeTestBridge.chainCount())
        assertEquals("original returns 11", 11, NativeTestBridge.callFnAttachTarget(10))
    }

    @Test
    fun chainRefusesNinthHandler() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()

        repeat(8) { i ->
            assertEquals("attach #${i + 1}", HOOK_OK, NativeTestBridge.chainAttach(t, h1))
        }
        assertEquals("8 handlers cap", 8, NativeTestBridge.chainHandlerCount(t))

        val rc9 = NativeTestBridge.chainAttach(t, h1)
        assertEquals("9th attach refused with HOOK_INTERNAL_ERROR",
            HOOK_INTERNAL_ERROR, rc9)
    }

    @Test
    fun chainDetachNonExistentReturnsNotHooked() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        // No chain installed yet.
        assertEquals("detach on empty target", HOOK_NOT_HOOKED,
            NativeTestBridge.chainDetach(t, h1))
    }

    @Test
    fun chainHandlerSeesMultipleCalls() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        NativeTestBridge.chainAttach(t, h1)

        NativeTestBridge.callFnAttachTarget(5)
        NativeTestBridge.callFnAttachTarget(7)
        NativeTestBridge.callFnAttachTarget(9)

        assertEquals("h1 fired 3 times", 3, NativeTestBridge.chainCalls1())
        assertEquals("last x0 = 9", 9, NativeTestBridge.chainLastX0_1())
    }
}