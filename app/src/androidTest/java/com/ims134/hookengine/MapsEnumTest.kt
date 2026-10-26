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
 * Session 50 — Feature 2.5: full maps enumeration.
 *
 * MemEnumMaps / MemEnumMapsFilter / MemFindRegionContaining expose
 * /proc/self/maps as a structured array: start, end, prot, offset,
 * dev major/minor, inode, path.
 *
 * Triplet layout for `memEnumMapsFiltered`: [start, end, prot] × N.
 * Quad layout for `memFindRegionContaining`: [start, end, prot, offset].
 *
 * HookResult codes referenced:
 *   HOOK_OK              = 0
 *   HOOK_TARGET_INVALID  = 11
 */
@RunWith(AndroidJUnit4::class)
class MapsEnumTest {

    private val PROT_READ  = 0x1
    private val PROT_WRITE = 0x2
    private val PROT_EXEC  = 0x4

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
    }

    @After
    fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun memEnumMapsReturnsManyEntries() {
        val count = NativeTestBridge.memEnumMapsAllCount()
        assertTrue("memEnumMaps must return >= 10 entries, got $count",
            count >= 10)
    }

    @Test
    fun memEnumMapsEntriesAreWellFormed() {
        val triplets = NativeTestBridge.memEnumMapsFiltered(0, 512)
        assertNotNull("memEnumMapsFiltered(0) returned null", triplets)
        triplets!!

        assertTrue("must return >= 30 longs (10 triplets), got ${triplets.size}",
            triplets.size >= 30)

        val n = triplets.size / 3
        var wellFormed = 0
        var pageAligned = 0
        for (i in 0 until n) {
            val s = triplets[i*3+0]
            val e = triplets[i*3+1]
            if (s < e) wellFormed++
            if ((s and 0xFFF) == 0L) pageAligned++
        }
        assertEquals("all $n entries must have start < end",
            n, wellFormed)
        assertEquals("all $n entries must be 4K-aligned",
            n, pageAligned)
    }

    @Test
    fun memEnumMapsHasExecutableRegion() {
        val triplets = NativeTestBridge.memEnumMapsFiltered(PROT_EXEC, 512)
        assertNotNull("memEnumMapsFiltered(PROT_EXEC) returned null", triplets)
        triplets!!

        val n = triplets.size / 3
        assertTrue("must find >= 1 PROT_EXEC region, got $n", n >= 1)

        var execOk = 0
        for (i in 0 until n) {
            val prot = triplets[i*3+2].toInt()
            if ((prot and PROT_EXEC) == PROT_EXEC) execOk++
        }
        assertEquals("all $n filter(PROT_EXEC) entries must have PROT_EXEC",
            n, execOk)
    }

    @Test
    fun memEnumMapsHasLoadedLibraries() {
        // Diagnostic dump: first 3 raw lines + parse result. Included in the
        // failure message so we know exactly what the parser sees if the
        // assertion below fails.
        val dump = NativeTestBridge.memEnumMapsDebugDump()

        val libcCount = NativeTestBridge.memEnumMapsPathContains("libc.so")
        assertTrue(
            "libc.so must appear in /proc/self/maps " +
                    "(every Android process loads it); got $libcCount\n" +
                    "=== MemEnumDebugDump ===\n" + dump,
            libcCount >= 1)

        val artCount = NativeTestBridge.memEnumMapsPathContains("libart.so")
        assertTrue(
            "libart.so must appear in /proc/self/maps " +
                    "(loaded by the JVM); got $artCount\n" +
                    "=== MemEnumDebugDump ===\n" + dump,
            artCount >= 1)
    }

    @Test
    fun memFindRegionContainingFixture() {
        val bufAddr = NativeTestBridge.getMemTestBufAddr()
        assertTrue("fixture addr must be real", bufAddr > 0x1000)

        val entry = NativeTestBridge.memFindRegionContaining(bufAddr)
        assertNotNull("memFindRegionContaining returned null for fixture " +
                "at 0x${bufAddr.toString(16)}", entry)
        entry!!

        assertEquals("quad must have 4 longs", 4, entry.size)
        val s = entry[0]
        val e = entry[1]
        val prot = entry[2].toInt()

        assertTrue("start=0x${s.toString(16)} must be <= fixture=0x${bufAddr.toString(16)}",
            s <= bufAddr)
        assertTrue("fixture=0x${bufAddr.toString(16)} must be < end=0x${e.toString(16)}",
            bufAddr < e)
        assertEquals("fixture must be readable",
            PROT_READ, prot and PROT_READ)
    }

    @Test
    fun memEnumMapsFilterIsStrictSubset() {
        val all = NativeTestBridge.memEnumMapsAllCount()
        assertTrue("all count > 0", all > 0)

        val execTriplets = NativeTestBridge.memEnumMapsFiltered(PROT_EXEC, 512)
        assertNotNull("filter(EXEC) returned null", execTriplets)
        execTriplets!!

        val execCount = execTriplets.size / 3
        assertTrue("filter(EXEC) count ($execCount) must be > 0", execCount > 0)
        assertTrue("filter(EXEC) count ($execCount) must be < all ($all)",
            execCount < all)
    }
}