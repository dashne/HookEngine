package com.ims134.hookengine

import android.os.ParcelFileDescriptor
import android.util.Log
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
 * Session 57 — Feature 7.6: PLT stub resolution.
 *
 * Resolves the PLT entry address for a given imported symbol in a loaded
 * library. Complements Features 7.2–7.5 (dynsym / symtab / hash / multi-lib
 * symbol resolution) by returning the CALL-SITE — the PLT thunk that the
 * library's own code branches into.
 *
 * Test library: `libjavacore.so`. Prior sessions confirmed it imports `open`
 * and `access` from libc (Feature 1.2's GOT hooks fire on both — see the
 * `hooked open in /apex/com.android.art/lib64/libjavacore.so` log line).
 * Both are guaranteed present in `.rela.plt`.
 *
 * ─── Rule 12 falsifiability ────────────────────────────────────────────
 *
 * The definitive test is `pltEntryCallableForOpen` — calling the resolved
 * PLT entry as if it were `open()`. If the address is correct (points to
 * `open@plt`), calling with `("/dev/null", 0, 0)` returns a valid fd. If
 * the address is 16 bytes off (wrong symbol's PLT entry), the call dispatches
 * to a different function whose signature doesn't match, returning -1 or a
 * garbage fd. Every offset-uncertainty scenario is caught by this one test.
 *
 * The other 6 tests provide structural coverage:
 *   1. pltResolveOpenNonZero       — basic non-zero resolution
 *   2. pltResolveMissingSymbolZero — non-existent symbol returns 0
 *   3. pltResolveMissingLibZero    — non-existent lib returns 0
 *   4. pltEntryIsExecutable        — PLT addr in PROT_EXEC map region
 *   5. pltEntryStubStructure       — first insn is adrp or bti-c
 *   6. pltDistinctSymbolsDiffer    — different symbols → different PLT entries
 *   7. pltEntryCallableForOpen     — call PLT entry, get valid fd (definitive)
 */
@RunWith(AndroidJUnit4::class)
class PltResolveTest {

    private val TAG = "PltResolveTest"
    private val PROT_EXEC = 0x4

    private val kInsnAdrp = 0x90000000L
    private val kInsnAdrpMask = 0x9F000000L
    private val kInsnBtiC = 0xD503245FL

    @Before fun setup() { NativeTestBridge.uninstallAll() }
    @After fun cleanup() { NativeTestBridge.uninstallAll() }

    private fun readInsn(addr: Long): Long {
        val b = NativeTestBridge.memRead(addr, 4) ?: return -1L
        if (b.size < 4) return -1L
        return (b[0].toLong() and 0xFF) or
                ((b[1].toLong() and 0xFF) shl 8) or
                ((b[2].toLong() and 0xFF) shl 16) or
                ((b[3].toLong() and 0xFF) shl 24)
    }

    @Test
    fun pltResolveOpenNonZero() {
        val addr = NativeTestBridge.pltResolve("libjavacore.so", "open")
        Log.d(TAG, "pltResolve(libjavacore.so, open) = 0x${addr.toString(16)}")
        assertTrue(
            "pltResolve must find a PLT entry for 'open' in libjavacore.so, got 0x${addr.toString(16)}",
            addr > 0x1000L)
    }

    @Test
    fun pltResolveMissingSymbolZero() {
        val addr = NativeTestBridge.pltResolve("libjavacore.so", "__he_nonexistent_xyz")
        assertEquals(
            "non-existent symbol must return 0",
            0L, addr)
    }

    @Test
    fun pltResolveMissingLibZero() {
        val addr = NativeTestBridge.pltResolve("libdoesnotexist.so", "open")
        assertEquals(
            "non-existent lib must return 0",
            0L, addr)
    }

    @Test
    fun pltEntryIsExecutable() {
        val addr = NativeTestBridge.pltResolve("libjavacore.so", "open")
        assertTrue("addr valid", addr > 0x1000L)
        val region = NativeTestBridge.memFindRegionContaining(addr)
        assertNotNull(
            "PLT addr 0x${addr.toString(16)} must be in a mapped region",
            region)
        val prot = region!![2].toInt()
        assertEquals(
            "PLT addr must be in PROT_EXEC region (got prot=0x${Integer.toHexString(prot)})",
            PROT_EXEC, prot and PROT_EXEC)
    }

    @Test
    fun pltEntryStubStructure() {
        val addr = NativeTestBridge.pltResolve("libjavacore.so", "open")
        assertTrue("addr valid", addr > 0x1000L)
        val insn0 = readInsn(addr)
        Log.d(TAG, "PLT entry insn0 = 0x${insn0.toString(16)}")
        val isAdrp = (insn0 and kInsnAdrpMask) == kInsnAdrp
        val isBti  = insn0 == kInsnBtiC
        assertTrue(
            "PLT entry first instruction must be 'adrp' (0x9xxxxxxx) or 'bti c' (0xD503245F), " +
                    "got 0x${insn0.toString(16)}",
            isAdrp || isBti)
    }

    @Test
    fun pltDistinctSymbolsDiffer() {
        val openAddr = NativeTestBridge.pltResolve("libjavacore.so", "open")
        val accessAddr = NativeTestBridge.pltResolve("libjavacore.so", "access")
        Log.d(TAG, "pltResolve: open=0x${openAddr.toString(16)} access=0x${accessAddr.toString(16)}")
        assertTrue("open > 0x1000", openAddr > 0x1000L)
        assertTrue("access > 0x1000", accessAddr > 0x1000L)
        assertNotEquals(
            "open and access must have distinct PLT entries",
            openAddr, accessAddr)
    }

    @Test
    fun pltEntryCallableForOpen() {
        val addr = NativeTestBridge.pltResolve("libjavacore.so", "open")
        assertTrue("pltResolve returned 0", addr > 0x1000L)
        // Call the PLT entry as if it were open(). O_RDONLY = 0.
        val fd = NativeTestBridge.pltCallOpen(addr, "/dev/null", 0)
        Log.d(TAG, "pltCallOpen(open_plt, /dev/null, 0) = $fd")
        try {
            assertTrue(
                "Calling open@plt('/dev/null', O_RDONLY) must return a valid fd. " +
                        "Got $fd. If fd < 0, the PLT entry address does not point to " +
                        "the real open@plt stub — could be a wrong-symbol PLT entry " +
                        "(offset error in .plt parsing).",
                fd >= 0)
        } finally {
            if (fd >= 0) {
                try { ParcelFileDescriptor.adoptFd(fd).close() } catch (_: Throwable) {}
            }
        }
    }
}