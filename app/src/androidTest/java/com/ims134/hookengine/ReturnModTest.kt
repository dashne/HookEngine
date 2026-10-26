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
 * Session 46 — Feature 1.9: return-value modification.
 *
 * A modifier function runs after the target's body executes, receives
 * (entry-time x0..x7, orig_ret), and returns the caller-visible value.
 * The body still runs (side effects preserved); the modifier only
 * post-processes the return.
 *
 * Target: fn_attach_target (from Feature 1.5), returns x+1.
 *
 * HookResult codes:
 *   HOOK_OK              = 0
 *   HOOK_ALREADY_HOOKED  = 8
 *   HOOK_NOT_HOOKED      = 9
 *   HOOK_TARGET_INVALID  = 11
 */
@RunWith(AndroidJUnit4::class)
class ReturnModTest {

    private val HOOK_OK = 0
    private val HOOK_ALREADY_HOOKED = 8
    private val HOOK_NOT_HOOKED = 9

    private val SENTINEL_DEAD = 0xDEAD

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
        NativeTestBridge.retModReset()
    }

    @After
    fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun retModReplacesReturnValue() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val mod = NativeTestBridge.getRetModAdd1000Addr()
        assertTrue("target addr", t > 0x1000)
        assertTrue("mod addr", mod > 0x1000)

        assertEquals("original(10)=11", 11, NativeTestBridge.callFnAttachTarget(10))
        assertEquals("no mods yet", 0, NativeTestBridge.retModCount())
        assertEquals("no fires yet", 0, NativeTestBridge.retModCalls())

        val rc = NativeTestBridge.attachRetMod(t, mod)
        assertEquals("attach", HOOK_OK, rc)
        assertEquals("1 retmod active", 1, NativeTestBridge.retModCount())

        // Duplicate attach refused
        val rcDup = NativeTestBridge.attachRetMod(t, mod)
        assertEquals("duplicate refused", HOOK_ALREADY_HOOKED, rcDup)

        // Body still runs (x+1) — modifier adds 1000
        val during = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("caller sees 11+1000", 1011, during)
        assertEquals("modifier fired once", 1, NativeTestBridge.retModCalls())

        val drc = NativeTestBridge.detachRetMod(t)
        assertEquals("detach", HOOK_OK, drc)
        assertEquals("0 retmods", 0, NativeTestBridge.retModCount())

        assertEquals("original restored", 11, NativeTestBridge.callFnAttachTarget(10))
        assertEquals("modifier not called after detach",
            1, NativeTestBridge.retModCalls())

        // Second detach — nothing left
        assertEquals("detach again", HOOK_NOT_HOOKED, NativeTestBridge.detachRetMod(t))
    }

    @Test
    fun retModSeesOriginalReturnValue() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val mod = NativeTestBridge.getRetModAdd1000Addr()
        assertEquals("attach", HOOK_OK, NativeTestBridge.attachRetMod(t, mod))

        NativeTestBridge.callFnAttachTarget(42)
        assertEquals("modifier saw orig_ret = 42+1 = 43",
            43, NativeTestBridge.retModLastOrigRet())

        NativeTestBridge.detachRetMod(t)
    }

    @Test
    fun retModSeesEntryArgs() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val mod = NativeTestBridge.getRetModAdd1000Addr()
        assertEquals("attach", HOOK_OK, NativeTestBridge.attachRetMod(t, mod))

        NativeTestBridge.callFnAttachTarget(10)
        assertEquals("modifier saw x0=10 at entry",
            10, NativeTestBridge.retModLastArg0())

        NativeTestBridge.callFnAttachTarget(77)
        assertEquals("modifier saw x0=77 at entry",
            77, NativeTestBridge.retModLastArg0())

        NativeTestBridge.detachRetMod(t)
    }

    @Test
    fun retModMultipleCallsEachFires() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val mod = NativeTestBridge.getRetModAdd1000Addr()
        assertEquals("attach", HOOK_OK, NativeTestBridge.attachRetMod(t, mod))

        repeat(5) { i ->
            val arg = i * 10
            val r = NativeTestBridge.callFnAttachTarget(arg)
            assertEquals("call $i: orig=${arg + 1} + 1000",
                1000 + arg + 1, r)
        }
        assertEquals("modifier fired 5 times",
            5, NativeTestBridge.retModCalls())

        NativeTestBridge.detachRetMod(t)
    }

    @Test
    fun retModForceDifferentValue() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val mod = NativeTestBridge.getRetModForceDeadAddr()
        assertEquals("attach", HOOK_OK, NativeTestBridge.attachRetMod(t, mod))

        val r = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("forced return 0xDEAD", SENTINEL_DEAD, r)

        NativeTestBridge.detachRetMod(t)
    }

    @Test
    fun retModDetachRestoresBytesAndClampScenario() {
        val t = NativeTestBridge.getFnAttachTargetAddr()

        val before = NativeTestBridge.readU32At(t)
        assertNotEquals("real instruction word", 0L, before)

        val mod = NativeTestBridge.getRetModClamp100Addr()
        assertEquals("attach", HOOK_OK, NativeTestBridge.attachRetMod(t, mod))

        val during = NativeTestBridge.readU32At(t)
        assertNotEquals("install rewrote first insn", before, during)

        // Clamp scenario: x=5 → orig 6 → in range, unmodified
        assertEquals("in range unmodified", 6,
            NativeTestBridge.callFnAttachTarget(5))

        // x=200 → orig 201 → clamped to 100
        assertEquals("out of range clamped", 100,
            NativeTestBridge.callFnAttachTarget(200))

        assertEquals("detach", HOOK_OK, NativeTestBridge.detachRetMod(t))
        val after = NativeTestBridge.readU32At(t)
        assertEquals("original bytes restored byte-identically", before, after)
        assertEquals("original returns 11", 11,
            NativeTestBridge.callFnAttachTarget(10))
        assertEquals("0 retmods", 0, NativeTestBridge.retModCount())
    }
}