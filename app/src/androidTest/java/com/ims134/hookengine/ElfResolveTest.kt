package com.ims134.hookengine

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Feature 7.1 — unified ELF symbol resolution.
 *
 * Rule 12: every assertion below can fail.
 *
 * Test #6 (symtabFallbackFires) pivoted to libc.so after measuring
 * that AGP strips .symtab from libhookengine.so even in debug builds
 * on this toolchain (keepDebugSymbols did not help — see S68 log).
 * libc.so on Android ships with .symtab (7738 symbols measured), so
 * the fallback path can be exercised against it by discovering a
 * .symtab-only symbol at runtime and asserting lastPath()==3.
 */
class ElfResolveTest {

    @Test
    fun lookupExportedSymbolInLibc() {
        val addr = NativeTestBridge.elfSymbolLookup("libc.so", "malloc")
        assertTrue("malloc must resolve in libc.so, got 0x${addr.toString(16)}", addr != 0L)
    }

    @Test
    fun lookupMissingSymbolReturnsNull() {
        val addr = NativeTestBridge.elfSymbolLookup(
            "libc.so", "__elf_resolve_test_definitely_missing_zzz_134")
        assertEquals("missing symbol must return 0", 0L, addr)
        assertEquals("lastPath must be -1 on failure", -1,
            NativeTestBridge.elfSymbolLookupLastPath())
    }

    @Test
    fun lookupMissingLibReturnsNull() {
        val addr = NativeTestBridge.elfSymbolLookup(
            "libtotally_fake_134_nonexistent.so", "malloc")
        assertEquals("missing lib must return 0", 0L, addr)
    }

    @Test
    fun multiLibFindsMalloc() {
        val addr = NativeTestBridge.elfSymbolLookupMulti("malloc")
        assertTrue("multi-lib malloc must resolve, got 0x${addr.toString(16)}", addr != 0L)
        val explicit = NativeTestBridge.elfSymbolLookup("libc.so", "malloc")
        assertEquals("multi-lib and explicit must agree", explicit, addr)
    }

    @Test
    fun lastPathReportsGnuHashForMalloc() {
        NativeTestBridge.elfSymbolLookup("libc.so", "malloc")
        assertEquals("GNU hash branch must fire for libc.so", 0,
            NativeTestBridge.elfSymbolLookupLastPath())
    }

    @Test
    fun symtabFallbackFires() {
        // S68 revision 3 — pivot to libc.so.
        // AGP strips .symtab from libhookengine.so even in debug builds
        // on this toolchain (keepDebugSymbols did not help). libc.so on
        // this device ships with .symtab (7738 symbols, proven by prior
        // log). Discover a .symtab-only symbol at runtime, then assert
        // the fallback path (lastPath==3) fires.
        //
        // Note: NOT depending on .symtab being present in the engine's
        // own .so — that's a build-config artifact, not a code property.
        val handle = NativeTestBridge.elfMapOpen("libc.so")
        assertNotEquals("ElfMappedLib_Open must succeed on libc.so", 0L, handle)
        try {
            assertEquals(".symtab must be present in libc.so on this device",
                1, NativeTestBridge.elfSectionPresent(handle, ".symtab"))
            val symCount = NativeTestBridge.elfSymtabCount(handle)
            assertTrue("libc.so .symtab must have > 0 symbols, got $symCount",
                symCount > 0)

            // Enumerate .symtab. For each name, do an ElfSymbolLookup and
            // check lastPath(). The first symbol that resolves via
            // lastPath()==3 proves the fallback path works end-to-end.
            var iterations = 0
            var proved = false
            var provedSymbol = ""
            for (i in 1 until symCount) {
                val n = NativeTestBridge.elfSymtabNthName(handle, i) ?: continue
                iterations++
                val addr = NativeTestBridge.elfSymbolLookup("libc.so", n)
                if (addr != 0L && NativeTestBridge.elfSymbolLookupLastPath() == 3) {
                    proved = true
                    provedSymbol = n
                    break
                }
                // Stop after 200 non-matching symbols — if a .symtab-only
                // symbol isn't found that quickly, the .symtab walker is
                // broken, not just unlucky.
                if (iterations >= 200) break
            }
            assertTrue(
                "must find at least one .symtab-only symbol that resolves " +
                        "via the fallback path within 200 iterations — " +
                        "proves .symtab walker works (libc.so .symtab has $symCount " +
                        "symbols total). Iterations tried: $iterations",
                proved)
            // Extra sanity: the address must not be null.
            val recheck = NativeTestBridge.elfSymbolLookup("libc.so", provedSymbol)
            assertTrue("proved symbol '$provedSymbol' must still resolve",
                recheck != 0L)
            assertEquals("lastPath still 3 on recheck", 3,
                NativeTestBridge.elfSymbolLookupLastPath())
        } finally {
            NativeTestBridge.elfMapClose(handle)
        }
    }

    @Test
    fun mapOpenCloseRoundTrip() {
        val h1 = NativeTestBridge.elfMapOpen("libc.so")
        assertNotEquals("ElfMappedLib_Open must succeed", 0L, h1)
        val n1 = NativeTestBridge.elfMapSectionCount(h1)
        assertTrue("libc.so must have > 0 sections, got $n1", n1 > 0)
        NativeTestBridge.elfMapClose(h1)

        val h2 = NativeTestBridge.elfMapOpen("libc.so")
        assertNotEquals("second open must succeed after clean close", 0L, h2)
        NativeTestBridge.elfMapClose(h2)
    }

    @Test
    fun repeatedLookupsAreStable() {
        val first = NativeTestBridge.elfSymbolLookup("libc.so", "malloc")
        assertTrue(first != 0L)
        for (i in 0 until 100) {
            val next = NativeTestBridge.elfSymbolLookup("libc.so", "malloc")
            assertEquals("lookup #$i must return stable address", first, next)
        }
    }
}