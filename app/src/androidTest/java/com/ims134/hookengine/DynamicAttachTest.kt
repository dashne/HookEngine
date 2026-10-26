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
 * Session 42 — Feature 1.5: dynamic runtime attach by computed address.
 *
 * The engine primitive `InstallInlineHookEx` has always accepted any
 * `void*` — this feature adds a caller-facing API for the case where the
 * address came from `dlsym`, `/proc/maps`, a pattern scan, or a captured
 * return address, not from a linker symbol reference.
 *
 * `AttachRuntime` adds validation the raw path never had:
 *   - refuse misaligned (addr & 3 != 0)
 *   - refuse unmapped (not in /proc/self/maps)
 *   - refuse non-executable target page (would SIGSEGV on the patched word)
 *   - refuse non-executable replacement (relay branches straight into repl)
 *
 * All refusals return `HOOK_TARGET_INVALID` = 11 from HookRegistry.h.
 *
 * "Computed address" is enforced structurally: the target is `fn_attach_target`
 * in libhookengine.so, obtained *only* through `getFnAttachTargetAddr()` as a
 * jlong. The Kotlin side never sees the symbol name — only a number.
 *
 * @Before uninstallAll() sacrifices the startup-installed anti-detection
 * hooks, but each test manages its own state and no subsequent test class
 * in this suite depends on them (same pattern as RelocTest / ProcessHooksTest).
 */
@RunWith(AndroidJUnit4::class)
class DynamicAttachTest {

    // HookResult from HookRegistry.h
    private val HOOK_OK = 0
    private val HOOK_TARGET_INVALID = 11

    private val SENTINEL = 0xDA77

    @Before fun reset() = NativeTestBridge.uninstallAll()
    @After  fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun attachRuntimeByComputedAddress() {
        val targetAddr = NativeTestBridge.getFnAttachTargetAddr()
        val fakeAddr   = NativeTestBridge.getFakeAttachAddr()

        assertTrue("target address must be real (> 0x1000), got 0x${targetAddr.toString(16)}",
            targetAddr > 0x1000)
        assertTrue("fake address must be real (> 0x1000), got 0x${fakeAddr.toString(16)}",
            fakeAddr > 0x1000)

        val before = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("fn_attach_target(10) initial: 10+1", 11, before)

        val rc = NativeTestBridge.attachRuntime(targetAddr, fakeAddr)
        assertEquals("attachRuntime must return HOOK_OK, got $rc", HOOK_OK, rc)

        val during = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("hook must fire — replacement returns sentinel", SENTINEL, during)

        val detRc = NativeTestBridge.detachRuntime(targetAddr)
        assertEquals("detachRuntime must return HOOK_OK, got $detRc", HOOK_OK, detRc)

        val after = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("fn_attach_target(10) restored", 11, after)
    }

    @Test
    fun attachRuntimeUnmappedAddressRefused() {
        val fakeAddr = NativeTestBridge.getFakeAttachAddr()
        // 0xDEADBEEF is definitely not in /proc/self/maps on any 64-bit process.
        val rc = NativeTestBridge.attachRuntime(0xDEADBEEFL, fakeAddr)
        assertEquals("unmapped address must return HOOK_TARGET_INVALID, got $rc",
            HOOK_TARGET_INVALID, rc)
    }

    @Test
    fun attachRuntimeMisalignedAddressRefused() {
        val targetAddr = NativeTestBridge.getFnAttachTargetAddr()
        val fakeAddr   = NativeTestBridge.getFakeAttachAddr()
        // ARM64 instructions are 4-byte aligned. targetAddr+1 would sit in
        // the middle of the first instruction word; a B.imm26 written there
        // would be nonsense. Refuse.
        val rc = NativeTestBridge.attachRuntime(targetAddr + 1, fakeAddr)
        assertEquals("misaligned address must return HOOK_TARGET_INVALID, got $rc",
            HOOK_TARGET_INVALID, rc)
    }

    @Test
    fun attachRuntimeNonExecutableTargetRefused() {
        val dataAddr = NativeTestBridge.getTestDataVarAddr()
        val fakeAddr = NativeTestBridge.getFakeAttachAddr()
        assertTrue("data var address must be real", dataAddr > 0x1000)
        // g_test_data_var is in .data/.bss (rw-, no exec). The target guard
        // fires before InstallInlineHookEx can corrupt data.
        val rc = NativeTestBridge.attachRuntime(dataAddr, fakeAddr)
        assertEquals("non-executable target must return HOOK_TARGET_INVALID, got $rc",
            HOOK_TARGET_INVALID, rc)
    }

    @Test
    fun attachRuntimeNonExecutableReplacementRefused() {
        val targetAddr = NativeTestBridge.getFnAttachTargetAddr()
        val dataAddr   = NativeTestBridge.getTestDataVarAddr()
        // Replacement must also be executable — the relay's `br x17` jumps
        // straight to it. Refusing here prevents a SIGSEGV on first call.
        val rc = NativeTestBridge.attachRuntime(targetAddr, dataAddr)
        assertEquals("non-executable replacement must return HOOK_TARGET_INVALID, got $rc",
            HOOK_TARGET_INVALID, rc)
    }

    @Test
    fun attachRuntimeDetachRestoresBytes() {
        val targetAddr = NativeTestBridge.getFnAttachTargetAddr()
        val fakeAddr   = NativeTestBridge.getFakeAttachAddr()

        val before = NativeTestBridge.readU32At(targetAddr)
        assertNotEquals("target must have a real instruction word", 0L, before)

        val rc = NativeTestBridge.attachRuntime(targetAddr, fakeAddr)
        assertEquals("install", HOOK_OK, rc)

        val during = NativeTestBridge.readU32At(targetAddr)
        assertNotEquals("install must overwrite the first instruction word " +
                "(before=0x${before.toString(16)})", before, during)

        val detRc = NativeTestBridge.detachRuntime(targetAddr)
        assertEquals("detach", HOOK_OK, detRc)

        val after = NativeTestBridge.readU32At(targetAddr)
        assertEquals("detach must restore original bytes byte-identically " +
                "(before=0x${before.toString(16)}, after=0x${after.toString(16)})",
            before, after)
    }
}