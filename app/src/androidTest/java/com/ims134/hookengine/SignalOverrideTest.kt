package com.ims134.hookengine

import android.system.OsConstants
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 52 — Feature 3.6: Signal handling override.
 *
 * ─── Why RT signals (not SIGUSR1/SIGUSR2) ───────────────────────────
 *
 * ART's Signal Catcher thread (thread name "Signal Catcher", loops on
 * sigwait) consumes {SIGQUIT, SIGUSR1, SIGUSR2} at the kernel level
 * before any sigaction handler runs. Those signals are reserved by ART:
 *   - SIGQUIT  → ANR trace dump
 *   - SIGUSR1  → GC / heap dump
 *   - SIGUSR2  → profile dump
 *
 * If we `sigaction(SIGUSR1, our_handler)` and then `raise(SIGUSR1)`,
 * the signal is routed to ART's sigwait waiter, not our handler. Our
 * handler never fires. First iteration of this test used SIGUSR1 and
 * observed "SIGUSR1 forcing GC" in logcat with fire_count=0 on every
 * test — that is the sigwait interception, not a bug in the override.
 *
 * Fix: use real-time signals (SIGRTMIN + 5, + 6). ART does not sigwait
 * on any RT signal, so sigaction + handler path works normally.
 *
 * Rule 12 falsifiability:
 *   1. sigovrInstallFireSuppress    — fire count 0→1; si_code == SI_QUEUE;
 *                                     si_pid > 0; si_uid >= 10000; PC != 0.
 *   2. sigovrHandlerSeesSiginfo     — sival_int payload byte-identical.
 *   3. sigovrSuppressNotChained     — raw counter stays 0 (SUPPRESS chains nothing).
 *   4. sigovrPassThroughChains      — raw counter becomes 1 (PASS_THROUGH chains).
 *   5. sigovrUninstallRestores      — SIG_DFL restored; handler kind queried.
 *   6. sigovrMultipleSignalsIndep   — SIGRTMIN+5 and +6 independent slots.
 */
@RunWith(AndroidJUnit4::class)
class SignalOverrideTest {

    // RT signals — see class header comment for why.
    private val SIGUSR1 = OsConstants.SIGRTMIN + 5
    private val SIGUSR2 = OsConstants.SIGRTMIN + 6

    // Linux siginfo si_code values
    private val SI_QUEUE = -1

    // handlerId constants matching SignalOverrideJni.cpp
    private val PASS_THROUGH = 0
    private val SUPPRESS = 1

    @Before
    fun setup() {
        NativeTestBridge.sigovrResetAll()
        NativeTestBridge.sigovrRestoreRawSigaction(SIGUSR1)
        NativeTestBridge.sigovrRestoreRawSigaction(SIGUSR2)
    }

    @After
    fun cleanup() {
        NativeTestBridge.sigovrResetAll()
        NativeTestBridge.sigovrRestoreRawSigaction(SIGUSR1)
        NativeTestBridge.sigovrRestoreRawSigaction(SIGUSR2)
    }

    /** Raise a signal with payload, bounded-wait for delivery. */
    private fun raiseAndWait(signo: Int, payload: Int = 0) {
        val before = NativeTestBridge.sigovrFireCount(signo)
        val rc = NativeTestBridge.sigovrRaiseWithInt(signo, payload)
        assertTrue("sigqueue($signo) failed rc=$rc — signal delivery broken", rc == 0)
        val deadline = System.currentTimeMillis() + 200
        while (System.currentTimeMillis() < deadline) {
            if (NativeTestBridge.sigovrFireCount(signo) > before) return
            Thread.sleep(2)
        }
    }

    @Test
    fun sigovrInstallFireSuppress() {
        assertEquals("baseline SIG_DFL", 0,
            NativeTestBridge.sigovrCurrentHandlerKind(SIGUSR1))
        assertEquals("baseline fire count", 0,
            NativeTestBridge.sigovrFireCount(SIGUSR1))

        assertEquals("install", 0,
            NativeTestBridge.sigovrInstall(SIGUSR1, SUPPRESS, 0L))
        assertEquals("count == 1", 1, NativeTestBridge.sigovrCount())
        assertEquals("handler installed", 2,
            NativeTestBridge.sigovrCurrentHandlerKind(SIGUSR1))

        raiseAndWait(SIGUSR1)

        assertEquals("fire count incremented", 1,
            NativeTestBridge.sigovrFireCount(SIGUSR1))
        assertEquals("si_code == SI_QUEUE",
            SI_QUEUE, NativeTestBridge.sigovrLastSiCode())
        assertTrue("si_pid > 0", NativeTestBridge.sigovrLastPid() > 0)
        assertTrue("si_uid >= 10000 (Android app uid)",
            NativeTestBridge.sigovrLastUid() >= 10000)
        assertTrue("PC nonzero",
            NativeTestBridge.sigovrLastPc() != 0L)
        // Process survived because SUPPRESS worked. If broken, dead.
    }

    @Test
    fun sigovrHandlerSeesSiginfo() {
        assertEquals("install", 0,
            NativeTestBridge.sigovrInstall(SIGUSR1, SUPPRESS, 0L))

        val payload = 0x42CAFE
        raiseAndWait(SIGUSR1, payload)

        assertEquals("fire count", 1,
            NativeTestBridge.sigovrFireCount(SIGUSR1))
        assertEquals("sival_int delivered", payload,
            NativeTestBridge.sigovrLastSivalInt())
        assertEquals("si_code == SI_QUEUE",
            SI_QUEUE, NativeTestBridge.sigovrLastSiCode())
    }

    @Test
    fun sigovrSuppressNotChained() {
        assertEquals("install raw", 0,
            NativeTestBridge.sigovrInstallRawSigaction(SIGUSR1))
        assertEquals("raw baseline", 0,
            NativeTestBridge.sigovrRawSigactionCount())

        assertEquals("install override", 0,
            NativeTestBridge.sigovrInstall(SIGUSR1, SUPPRESS, 0L))

        raiseAndWait(SIGUSR1)

        assertEquals("our handler fired", 1,
            NativeTestBridge.sigovrFireCount(SIGUSR1))
        assertEquals("raw handler NOT chained (SUPPRESS)", 0,
            NativeTestBridge.sigovrRawSigactionCount())
    }

    @Test
    fun sigovrPassThroughChains() {
        assertEquals("install raw", 0,
            NativeTestBridge.sigovrInstallRawSigaction(SIGUSR1))
        assertEquals("raw baseline", 0,
            NativeTestBridge.sigovrRawSigactionCount())

        assertEquals("install override", 0,
            NativeTestBridge.sigovrInstall(SIGUSR1, PASS_THROUGH, 0L))

        raiseAndWait(SIGUSR1)

        assertEquals("our handler fired", 1,
            NativeTestBridge.sigovrFireCount(SIGUSR1))
        assertEquals("raw handler chained (PASS_THROUGH)", 1,
            NativeTestBridge.sigovrRawSigactionCount())
    }

    @Test
    fun sigovrUninstallRestoresOriginal() {
        assertEquals("baseline SIG_DFL", 0,
            NativeTestBridge.sigovrCurrentHandlerKind(SIGUSR1))

        assertEquals("install", 0,
            NativeTestBridge.sigovrInstall(SIGUSR1, SUPPRESS, 0L))
        assertEquals("handler active", 2,
            NativeTestBridge.sigovrCurrentHandlerKind(SIGUSR1))

        raiseAndWait(SIGUSR1)
        assertEquals("fire 1", 1, NativeTestBridge.sigovrFireCount(SIGUSR1))

        assertEquals("uninstall", 0,
            NativeTestBridge.sigovrUninstall(SIGUSR1))
        assertEquals("count == 0", 0, NativeTestBridge.sigovrCount())
        assertEquals("SIG_DFL restored", 0,
            NativeTestBridge.sigovrCurrentHandlerKind(SIGUSR1))

        // Do NOT raise again — SIG_DFL for this signal terminates the process.
        // The handler-kind assertion above proves restore worked.
    }

    @Test
    fun sigovrMultipleSignalsIndependent() {
        assertEquals("install a", 0,
            NativeTestBridge.sigovrInstall(SIGUSR1, SUPPRESS, 0L))
        assertEquals("install b", 0,
            NativeTestBridge.sigovrInstall(SIGUSR2, SUPPRESS, 0L))
        assertEquals("count == 2", 2, NativeTestBridge.sigovrCount())

        raiseAndWait(SIGUSR1)
        assertEquals("sigA fire", 1,
            NativeTestBridge.sigovrFireCount(SIGUSR1))
        assertEquals("sigB not fired", 0,
            NativeTestBridge.sigovrFireCount(SIGUSR2))

        raiseAndWait(SIGUSR2)
        assertEquals("sigA still 1", 1,
            NativeTestBridge.sigovrFireCount(SIGUSR1))
        assertEquals("sigB fire", 1,
            NativeTestBridge.sigovrFireCount(SIGUSR2))

        assertEquals("uninstall a", 0,
            NativeTestBridge.sigovrUninstall(SIGUSR1))
        assertEquals("count == 1", 1, NativeTestBridge.sigovrCount())

        raiseAndWait(SIGUSR2)
        assertEquals("sigB still fires", 2,
            NativeTestBridge.sigovrFireCount(SIGUSR2))
    }
}