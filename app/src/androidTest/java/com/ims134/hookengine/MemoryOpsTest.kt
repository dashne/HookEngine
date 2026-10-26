package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 49 — Features 2.1 / 2.2 / 2.3: memory read / write / scan.
 *
 * All operations validate against /proc/self/maps and refuse unmapped
 * regions. MemWrite mprotects each page to RW, writes, and restores the
 * original prot — Rule 10 preserved on every path.
 *
 * Fixtures (native-lib.cpp):
 *   g_memtest_buf[256]  — bytes 0..99 = 0..99; 100..107 = magic;
 *                         108..255 = 108..255
 *   g_memtest_ro_page   — dedicated mmap page, initially PROT_READ
 *
 * Fixture magic at offset 100:
 *   { 0xCA, 0xFE, 0xBA, 0xBE, 0x13, 0x37, 0x42, 0x99 }
 *
 * HookResult codes:
 *   HOOK_OK              = 0
 *   HOOK_MPROTECT_FAILED = 5
 *   HOOK_INTERNAL_ERROR  = 10
 *   HOOK_TARGET_INVALID  = 11
 */
@RunWith(AndroidJUnit4::class)
class MemoryOpsTest {

    private val HOOK_OK = 0
    private val HOOK_TARGET_INVALID = 11

    // PROT_* bit masks (subset used by Android)
    private val PROT_READ  = 0x1
    private val PROT_WRITE = 0x2
    private val PROT_EXEC  = 0x4

    // Guaranteed-unmapped: top 16 bits 0xDEAD, well above 48-bit user-space.
    private val UNMAPPED_ADDR = 0xDEADBEEFDEAD0000UL.toLong()

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
    }

    @After
    fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun memReadReturnsExactBytes() {
        val buf = NativeTestBridge.getMemTestBufAddr()
        assertTrue("buf addr", buf > 0x1000)

        // Read first 8 bytes — should be 0,1,2,3,4,5,6,7.
        val head = NativeTestBridge.memRead(buf, 8)
        assertNotNull("memRead returned null on mapped range", head)
        head!!
        assertEquals(8, head.size)
        assertArrayEquals(
            "first 8 bytes should be 0..7",
            byteArrayOf(0, 1, 2, 3, 4, 5, 6, 7), head)

        // Read across the magic — offset 100..107.
        val magic = NativeTestBridge.memRead(buf + 100, 8)
        assertNotNull("magic region read", magic)
        magic!!
        assertEquals(8, magic.size)
        assertArrayEquals(
            "magic bytes at offset 100",
            byteArrayOf(
                0xCA.toByte(), 0xFE.toByte(), 0xBA.toByte(), 0xBE.toByte(),
                0x13, 0x37, 0x42, 0x99.toByte()), magic)
    }

    @Test
    fun memReadUnmappedRefused() {
        // Reading unmapped should return null (JNI bridge maps HOOK_TARGET_INVALID → null).
        val r = NativeTestBridge.memRead(UNMAPPED_ADDR, 16)
        assertNull("memRead on unmapped must return null", r)

        // Also test a single-byte read just past a page-aligned unmapped addr.
        val r2 = NativeTestBridge.memRead(UNMAPPED_ADDR + 0x1000L, 4)
        assertNull("memRead on higher unmapped page must return null", r2)
    }

    @Test
    fun memWriteReadOnlyPageRestoresProt() {
        NativeTestBridge.memTestResetRoPage()
        val roPage = NativeTestBridge.getMemTestRoPageAddr()
        assertTrue("ro page addr", roPage > 0x1000)

        // Baseline: page should be readable, NOT writable.
        val protBefore = NativeTestBridge.memTestPageProtOf(roPage)
        assertEquals("RO page must start PROT_READ, got 0x${Integer.toHexString(protBefore)}",
            PROT_READ, protBefore and (PROT_READ or PROT_WRITE or PROT_EXEC))

        // Baseline read: should be 16 zero bytes.
        val zeros = NativeTestBridge.memRead(roPage, 16)
        assertNotNull("baseline read", zeros)
        assertArrayEquals("RO page starts zeroed",
            ByteArray(16) { 0 }, zeros)

        // Write a distinctive pattern.
        val pattern = ByteArray(16) { i -> (0x40 + i).toByte() }
        val wrc = NativeTestBridge.memWrite(roPage, pattern)
        assertEquals("memWrite to PROT_READ page must succeed via mprotect dance",
            HOOK_OK, wrc)

        // Confirm the write landed.
        val back = NativeTestBridge.memRead(roPage, 16)
        assertNotNull("read after write", back)
        assertArrayEquals("write landed byte-for-byte", pattern, back)

        // Confirm prot was restored to PROT_READ (Rule 10).
        val protAfter = NativeTestBridge.memTestPageProtOf(roPage)
        assertEquals("page prot restored to PROT_READ after write, got 0x${Integer.toHexString(protAfter)}",
            PROT_READ, protAfter and (PROT_READ or PROT_WRITE or PROT_EXEC))
    }

    @Test
    fun memWriteUnmappedRefused() {
        // Capture the current buffer state so we can prove no partial write.
        val buf = NativeTestBridge.getMemTestBufAddr()
        val baseline = NativeTestBridge.memRead(buf, 32)
        assertNotNull("baseline read for write-unmapped test", baseline)

        // Write to unmapped should fail with HOOK_TARGET_INVALID.
        val pattern = ByteArray(16) { 0xAA.toByte() }
        val rc = NativeTestBridge.memWrite(UNMAPPED_ADDR, pattern)
        assertEquals("memWrite to unmapped must return HOOK_TARGET_INVALID",
            HOOK_TARGET_INVALID, rc)

        // Buffer untouched (Rule 10 adjacent — no partial state on failure).
        val after = NativeTestBridge.memRead(buf, 32)
        assertNotNull("post-failure read", after)
        assertArrayEquals("buffer unchanged after refused write", baseline!!, after)
    }

    @Test
    fun memScanFindsExactMagic() {
        val buf = NativeTestBridge.getMemTestBufAddr()
        val pattern = byteArrayOf(
            0xCA.toByte(), 0xFE.toByte(), 0xBA.toByte(), 0xBE.toByte(),
            0x13, 0x37, 0x42, 0x99.toByte())

        val hits = NativeTestBridge.memScan(buf, 256L, pattern, null, 16)
        assertNotNull("memScan returned null", hits)
        hits!!
        assertEquals("exactly 1 hit expected for exact magic", 1, hits.size)
        assertEquals("hit must be at buf+100", buf + 100, hits[0])
    }

    @Test
    fun memScanMaskedFindsPartial() {
        val buf = NativeTestBridge.getMemTestBufAddr()

        // Pattern that would need last byte to be 0x00 (not 0x99), with
        // mask that ignores the last byte entirely. Should still match
        // because mask[7] = 0 makes the last byte don't-care.
        val pattern = byteArrayOf(
            0xCA.toByte(), 0xFE.toByte(), 0xBA.toByte(), 0xBE.toByte(),
            0x13, 0x37, 0x42, 0x00)
        val mask = byteArrayOf(
            0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(),
            0xFF.toByte(), 0xFF.toByte(), 0xFF.toByte(), 0x00)

        val hits = NativeTestBridge.memScan(buf, 256L, pattern, mask, 16)
        assertNotNull("masked memScan returned null", hits)
        hits!!
        assertEquals("masked scan must find 1 hit (last byte ignored)", 1, hits.size)
        assertEquals("masked hit must be at buf+100", buf + 100, hits[0])
    }
}