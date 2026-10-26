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
 * Session 47 — Feature 1.6: SVC instruction hooking.
 *
 * Patching individual `svc #0` sites to `brk #0x4242`, dispatching from a
 * SIGTRAP handler. Handler runs in signal context, sees (nr, a0..a5,
 * orig_ret) and returns the caller-visible value.
 *
 * Encoding reference (confirmed):
 *   svc #0        = 0xD4000001
 *   brk #0x4242   = 0xD4284840  (0xD4200000 | (0x4242 << 5))
 *
 * Fixtures (native-lib.cpp):
 *   svc_getpid_target    — mov x8,#172; svc #0; ret  (svc at +4)
 *   svc_getppid_target   — mov x8,#173; svc #0; ret  (svc at +4)
 *   svc_close_invalid(fd)— mov x8,#57;  svc #0; ret  (svc at +4)
 */
@RunWith(AndroidJUnit4::class)
class SvcHookTest {

    private val HOOK_OK = 0
    private val HOOK_NOT_HOOKED = 9

    private val SENTINEL_7ACE = 0x7ACE      // 31438
    private val NR_GETPID = 172
    private val NR_CLOSE  = 57
    private val EBADF     = -9

    private val SVC_ZERO    = 0xD4000001L
    private val BRK_0x4242  = 0xD4284840L

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
        NativeTestBridge.svcResetCounters()
    }

    @After
    fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun svcAttachFireAndModify() {
        val targetAddr = NativeTestBridge.getSvcGetpidTargetAddr()
        val svcAddr    = NativeTestBridge.getSvcGetpidSvcAddr()
        val handler    = NativeTestBridge.getSvcHandlerForce7aceAddr()
        assertTrue("target addr", targetAddr > 0x1000)
        assertTrue("svc addr", svcAddr > 0x1000)
        assertTrue("svc = target + 4", svcAddr == targetAddr + 4)
        assertTrue("handler addr", handler > 0x1000)

        val realPid = NativeTestBridge.callSvcGetpid()
        assertTrue("real pid > 0, got $realPid", realPid > 0)
        assertEquals("handler not fired yet", 0L, NativeTestBridge.svcFires())

        val rc = NativeTestBridge.svcHookAttach(svcAddr, NR_GETPID, handler)
        assertEquals("attach", HOOK_OK, rc)
        assertEquals("1 svc hook", 1, NativeTestBridge.svcHookCount())

        val hooked = NativeTestBridge.callSvcGetpid()
        assertEquals("caller sees 0x7ACE", SENTINEL_7ACE, hooked)
        assertEquals("handler fired once", 1L, NativeTestBridge.svcFires())
        assertEquals("handler saw nr=$NR_GETPID",
            NR_GETPID.toLong(), NativeTestBridge.svcLastNr())
        assertEquals("handler saw orig_ret=$realPid",
            realPid.toLong(), NativeTestBridge.svcLastOrig())

        val drc = NativeTestBridge.svcHookDetach(svcAddr)
        assertEquals("detach", HOOK_OK, drc)
        assertEquals("0 svc hooks", 0, NativeTestBridge.svcHookCount())

        val restored = NativeTestBridge.callSvcGetpid()
        assertEquals("real pid restored", realPid, restored)
        assertEquals("handler not called after detach",
            1L, NativeTestBridge.svcFires())

        assertEquals("detach again", HOOK_NOT_HOOKED, NativeTestBridge.svcHookDetach(svcAddr))
    }

    @Test
    fun svcHandlerSeesArgsAndOrigRet() {
        val svcAddr = NativeTestBridge.getSvcCloseInvalidSvcAddr()
        val handler = NativeTestBridge.getSvcHandlerIdentityAddr()

        assertEquals("attach", HOOK_OK,
            NativeTestBridge.svcHookAttach(svcAddr, NR_CLOSE, handler))

        val ret = NativeTestBridge.callSvcCloseInvalid(-1)
        assertEquals("caller sees EBADF=-9", EBADF, ret)
        assertEquals("handler fired", 1L, NativeTestBridge.svcFires())
        assertEquals("handler saw nr=57", NR_CLOSE.toLong(),
            NativeTestBridge.svcLastNr())
        assertEquals("handler saw orig_ret=-9", EBADF.toLong(),
            NativeTestBridge.svcLastOrig())
        // x0 at the SVC is fd. When caller passes fd=-1 as int32, the top
        // 32 bits of x0 are ABI-unspecified — check only the low 32 bits.
        val arg0 = NativeTestBridge.svcLastArg0()
        assertEquals("handler saw a0 low 32 = 0xFFFFFFFF (fd=-1)",
            0xFFFFFFFFL, arg0 and 0xFFFFFFFFL)

        NativeTestBridge.svcHookDetach(svcAddr)
    }

    @Test
    fun svcIsolationBetweenSites() {
        val pidSvc  = NativeTestBridge.getSvcGetpidSvcAddr()
        val ppidSvc = NativeTestBridge.getSvcGetppidSvcAddr()
        val handler = NativeTestBridge.getSvcHandlerForce7aceAddr()

        val realPpid = NativeTestBridge.callSvcGetppid()
        assertTrue("real ppid > 0", realPpid > 0)

        assertEquals("attach getpid only", HOOK_OK,
            NativeTestBridge.svcHookAttach(pidSvc, NR_GETPID, handler))
        assertEquals("1 hook", 1, NativeTestBridge.svcHookCount())

        assertEquals("getpid intercepted", SENTINEL_7ACE, NativeTestBridge.callSvcGetpid())
        assertEquals("1 fire", 1L, NativeTestBridge.svcFires())

        val ppidAfter = NativeTestBridge.callSvcGetppid()
        assertEquals("getppid returns real ppid (not intercepted)",
            realPpid, ppidAfter)
        assertEquals("handler NOT fired for getppid",
            1L, NativeTestBridge.svcFires())

        NativeTestBridge.svcHookDetach(pidSvc)
    }

    @Test
    fun svcMultipleSitesIndependentHandlers() {
        val pidSvc   = NativeTestBridge.getSvcGetpidSvcAddr()
        val ppidSvc  = NativeTestBridge.getSvcGetppidSvcAddr()
        val forceFn  = NativeTestBridge.getSvcHandlerForce7aceAddr()
        val add1000  = NativeTestBridge.getSvcHandlerAdd1000Addr()

        assertEquals("attach pid", HOOK_OK,
            NativeTestBridge.svcHookAttach(pidSvc, NR_GETPID, forceFn))
        assertEquals("attach ppid", HOOK_OK,
            NativeTestBridge.svcHookAttach(ppidSvc, NR_GETPID + 1 /* 173 */, add1000))
        assertEquals("2 hooks", 2, NativeTestBridge.svcHookCount())

        assertEquals("pid forced", SENTINEL_7ACE, NativeTestBridge.callSvcGetpid())
        assertEquals("1 fire", 1L, NativeTestBridge.svcFires())
        assertEquals("last nr=172", NR_GETPID.toLong(), NativeTestBridge.svcLastNr())

        val ppidHooked = NativeTestBridge.callSvcGetppid()
        assertEquals("2 fires", 2L, NativeTestBridge.svcFires())
        assertEquals("last nr=173", (NR_GETPID + 1).toLong(),
            NativeTestBridge.svcLastNr())
        val ppidOrig = NativeTestBridge.svcLastOrig()
        assertEquals("ppid transformed as orig+1000",
            (ppidOrig + 1000).toInt(), ppidHooked)

        NativeTestBridge.svcHookDetach(pidSvc)
        NativeTestBridge.svcHookDetach(ppidSvc)
    }

    @Test
    fun svcDetachRestoresBytes() {
        val svcAddr = NativeTestBridge.getSvcGetpidSvcAddr()
        val handler = NativeTestBridge.getSvcHandlerForce7aceAddr()

        val before = NativeTestBridge.readU32At(svcAddr)
        assertEquals("initial insn is svc #0", SVC_ZERO, before)

        assertEquals("attach", HOOK_OK,
            NativeTestBridge.svcHookAttach(svcAddr, NR_GETPID, handler))

        val during = NativeTestBridge.readU32At(svcAddr)
        assertNotEquals("install rewrote bytes", before, during)
        assertEquals("patched to brk #0x4242", BRK_0x4242, during)

        assertEquals("detach", HOOK_OK, NativeTestBridge.svcHookDetach(svcAddr))

        val after = NativeTestBridge.readU32At(svcAddr)
        assertEquals("original svc #0 restored byte-identically", before, after)
    }

    @Test
    fun svcScanLibFindsSites() {
        val count = NativeTestBridge.svcHookScanLib("libc.so")
        assertTrue("libc.so should have >= 100 svc #0 sites, found $count",
            count >= 100)
    }
}