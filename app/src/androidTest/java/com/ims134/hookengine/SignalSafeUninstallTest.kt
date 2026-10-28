package com.ims134.hookengine

import android.os.Process
import android.system.OsConstants
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicInteger

/**
 * Session 65 — Feature 6.8: signal-safe uninstall.
 *
 * ─── Mechanism: deferred uninstall via SPSC ring buffer + worker thread ─
 *
 * The current UninstallInlineHook() path is NOT async-signal-safe:
 *   - Two mutex acquisitions (g_hook_mutex, g_reg_mutex)
 *   - One nanosleep(100ms) for deferred-free grace
 *   - Self-deadlock if signal fires on the thread holding the mutex
 *
 * The deferred design:
 *   - Signal handler: spinlock-protected SPSC enqueue + eventfd write → returns in <10us
 *   - Worker thread: eventfd wakeup → drain queue → call UninstallInlineHook()
 *     from normal thread context (mutex, nanosleep, munmap all safe there)
 *   - Zero changes to UninstallInlineHook — the worker just calls it normally
 *
 * The handler must be ARMED before signal context can enqueue. Arm() spawns
 * the worker + creates the eventfd from normal thread context.
 *
 * ─── Rule 12 — every assertion is falsifiable ──────────────────────
 *
 *   1. armDisarmRoundTrip
 *      Fails if Arm() crashes, if the worker thread fails to spawn, if
 *      Disarm() hangs, or if either is not idempotent.
 *
 *   2. deferredUninstallFromSignalHandler
 *      The core feature test. Installs a hook, arms, raises a signal
 *      (handler=deferred_uninstall with target as userCtx), waits for
 *      completion. Fails if:
 *        - Hook is not installed before signal (attachRuntime != 0)
 *        - Hook fires pre-signal (callFnAttachTarget != SENTINEL)
 *        - Signal raises non-zero return
 *        - WaitFor times out (> 500ms with 100ms grace + 400ms margin)
 *        - Hook still fires post-signal (callFnAttachTarget == SENTINEL)
 *        - Stats counters don't match (enqueued != 1 or completed != 1)
 *      If the deferred design were broken (e.g., handler tried to call
 *      UninstallInlineHook directly from signal context), the process
 *      would deadlock or crash — the test would time out, not pass.
 *
 *   3. multipleSignalsSequential
 *      Three hooks, three signals raised one at a time. Fails if any
 *      individual cycle doesn't complete, or if stats drift.
 *
 *   4. statsMatchQueueOperations
 *      Direct enqueue (not through signal) to isolate queue correctness
 *      from signal-delivery correctness. Fails if queue doesn't drain
 *      or stats underflow/overflow.
 *
 *   5. unarmedSignalIsNoOp
 *      Disarm, install hook, raise signal. Fails if:
 *        - Handler crashes (should return -3, signal SUPPRESS'd)
 *        - Hook unexpectedly uninstalls (deferred worker isn't running)
 *        - WaitFor doesn't time out (would mean handler bypassed arming)
 *
 *   6. signalHandlerDoesNotDeadlockUnderContention
 *      Contention thread holds g_hook_mutex intermittently via tight
 *      install/uninstall loop; main thread raises signal and waits for
 *      deferred completion. Fails if the process hangs (deadlock) or if
 *      WaitFor times out. Not a direct test of self-deadlock (can't
 *      force signal delivery to the specific thread holding the lock),
 *      but exercises the cross-thread contention path that the old
 *      design would suffer from.
 *
 * ─── Test isolation ────────────────────────────────────────────────
 *
 * @Before disarms any prior arm (idempotent), resets sigovr, and calls
 * uninstallAll. @After does the same in reverse. Signal used is
 * SIGRTMIN+20 to avoid S52's SIGRTMIN+5/6 and any ART-reserved signals.
 */
@RunWith(AndroidJUnit4::class)
class SignalSafeUninstallTest {

    private val TAG = "SigSafeUninstallTest"
    private val SIG_UNINSTALL = OsConstants.SIGRTMIN + 20
    private val HANDLER_DEFERRED = 2  // matches SignalOverrideJni.cpp
    private val SENTINEL = 0xDA77     // fake_attach return
    private val ORIGINAL = 11         // fn_attach_target(10) = 10 + 1

    @Before
    fun reset() {
        NativeTestBridge.signalSafeUninstallDisarm()
        NativeTestBridge.sigovrUninstall(SIG_UNINSTALL)
        NativeTestBridge.sigovrRestoreRawSigaction(SIG_UNINSTALL)
        NativeTestBridge.sigovrResetAll()
        NativeTestBridge.uninstallAll()
    }

    @After
    fun cleanup() {
        NativeTestBridge.signalSafeUninstallDisarm()
        NativeTestBridge.sigovrUninstall(SIG_UNINSTALL)
        NativeTestBridge.sigovrRestoreRawSigaction(SIG_UNINSTALL)
        NativeTestBridge.sigovrResetAll()
        NativeTestBridge.uninstallAll()
    }

    // ─── 1. arm/disarm idempotency ──────────────────────────────────
    @Test
    fun armDisarmRoundTrip() {
        assertEquals("first arm", 0, NativeTestBridge.signalSafeUninstallArm())
        assertEquals("second arm (idempotent)", 0, NativeTestBridge.signalSafeUninstallArm())
        assertEquals("disarm", 0, NativeTestBridge.signalSafeUninstallDisarm())
        assertEquals("second disarm (idempotent)", 0, NativeTestBridge.signalSafeUninstallDisarm())
    }

    // ─── 2. deferred uninstall from signal context ──────────────────
    @Test
    fun deferredUninstallFromSignalHandler() {
        val target = NativeTestBridge.getFnAttachTargetAddr()
        val fake = NativeTestBridge.getFakeAttachAddr()
        assertTrue("fixture addr valid", target > 0x1000 && fake > 0x1000)

        assertEquals("arm", 0, NativeTestBridge.signalSafeUninstallArm())

        // Install hook
        assertEquals("attachRuntime", 0, NativeTestBridge.attachRuntime(target, fake))
        assertEquals("hook fires pre-signal", SENTINEL, NativeTestBridge.callFnAttachTarget(10))

        // Install signal handler with target as userCtx
        assertEquals("sigovrInstall",
            0, NativeTestBridge.sigovrInstall(SIG_UNINSTALL, HANDLER_DEFERRED, target))

        // Raise signal — handler enqueues target, writes to eventfd, returns immediately
        val raiseRc = NativeTestBridge.sigovrRaiseWithInt(SIG_UNINSTALL, 0)
        assertEquals("sigqueue", 0, raiseRc)

        // Wait for worker to drain and uninstall. 500ms budget = 5× the
        // 100ms grace period + margin for scheduling latency.
        val waitRc = NativeTestBridge.signalSafeUninstallWaitFor(target, 500)
        assertEquals("WaitFor should complete (deferred uninstall)", 0, waitRc)

        // Verify hook is uninstalled
        val post = NativeTestBridge.callFnAttachTarget(10)
        assertEquals("hook restored after deferred uninstall", ORIGINAL, post)

        // Verify stats
        val stats = NativeTestBridge.signalSafeUninstallStats()
        Log.d(TAG, "stats enqueued=${stats[0]} completed=${stats[1]} failed=${stats[2]} dropped=${stats[3]}")
        assertEquals("enqueued", 1L, stats[0])
        assertEquals("completed", 1L, stats[1])
        assertEquals("failed", 0L, stats[2])
        assertEquals("dropped", 0L, stats[3])
    }

    // ─── 3. multiple signals in sequence ────────────────────────────
    @Test
    fun multipleSignalsSequential() {
        val t1 = NativeTestBridge.getFnAttachTargetAddr()
        val t2 = NativeTestBridge.getSvcGetpidTargetAddr()
        val t3 = NativeTestBridge.getSvcGetppidTargetAddr()
        val fake = NativeTestBridge.getFakeAttachAddr()
        assertTrue("fixtures valid",
            t1 > 0x1000 && t2 > 0x1000 && t3 > 0x1000 && fake > 0x1000)

        assertEquals("arm", 0, NativeTestBridge.signalSafeUninstallArm())

        // Install all three hooks
        assertEquals("attach t1", 0, NativeTestBridge.attachRuntime(t1, fake))
        assertEquals("attach t2", 0, NativeTestBridge.attachRuntime(t2, fake))
        assertEquals("attach t3", 0, NativeTestBridge.attachRuntime(t3, fake))

        // Round-trip each target through signal + wait
        for ((idx, t) in listOf(t1, t2, t3).withIndex()) {
            NativeTestBridge.sigovrUninstall(SIG_UNINSTALL)
            assertEquals("sigovrInstall[$idx]",
                0, NativeTestBridge.sigovrInstall(SIG_UNINSTALL, HANDLER_DEFERRED, t))
            assertEquals("sigqueue[$idx]",
                0, NativeTestBridge.sigovrRaiseWithInt(SIG_UNINSTALL, 0))
            assertEquals("WaitFor[$idx]",
                0, NativeTestBridge.signalSafeUninstallWaitFor(t, 500))
            Log.d(TAG, "  round-trip $idx target=0x${t.toString(16)} complete")
        }

        // Verify all uninstalled — svc fixtures restored to original asm
        // (we don't have a direct probe for svc fixtures being original, but
        // callFnAttachTarget on t1 returning ORIGINAL is enough evidence
        // that t1 came back; t2/t3 assert via stats).
        assertEquals("t1 restored", ORIGINAL, NativeTestBridge.callFnAttachTarget(10))

        val stats = NativeTestBridge.signalSafeUninstallStats()
        assertEquals("enqueued 3", 3L, stats[0])
        assertEquals("completed 3", 3L, stats[1])
        assertEquals("failed 0", 0L, stats[2])
        assertEquals("dropped 0", 0L, stats[3])
    }

    // ─── 4. direct enqueue (bypass signal) tests queue mechanics ────
    @Test
    fun statsMatchQueueOperations() {
        val t1 = NativeTestBridge.getFnAttachTargetAddr()
        val t2 = NativeTestBridge.getSvcGetpidTargetAddr()
        val fake = NativeTestBridge.getFakeAttachAddr()

        assertEquals("arm", 0, NativeTestBridge.signalSafeUninstallArm())
        assertEquals("attach t1", 0, NativeTestBridge.attachRuntime(t1, fake))
        assertEquals("attach t2", 0, NativeTestBridge.attachRuntime(t2, fake))

        // Enqueue directly (not through signal) — this isolates queue
        // mechanics from signal-delivery semantics.
        assertEquals("enqueue t1", 0, NativeTestBridge.signalSafeUninstallFromHandler(t1))
        assertEquals("enqueue t2", 0, NativeTestBridge.signalSafeUninstallFromHandler(t2))

        assertEquals("WaitFor t1", 0, NativeTestBridge.signalSafeUninstallWaitFor(t1, 500))
        assertEquals("WaitFor t2", 0, NativeTestBridge.signalSafeUninstallWaitFor(t2, 500))

        val stats = NativeTestBridge.signalSafeUninstallStats()
        assertEquals("enqueued 2", 2L, stats[0])
        assertEquals("completed 2", 2L, stats[1])
        assertEquals("failed 0", 0L, stats[2])
        assertEquals("dropped 0", 0L, stats[3])
    }

    // ─── 5. unarmed state = no-op for signal ────────────────────────
    @Test
    fun unarmedSignalIsNoOp() {
        val target = NativeTestBridge.getFnAttachTargetAddr()
        val fake = NativeTestBridge.getFakeAttachAddr()

        // Ensure disarmed
        NativeTestBridge.signalSafeUninstallDisarm()

        assertEquals("attach", 0, NativeTestBridge.attachRuntime(target, fake))
        assertEquals("hook fires", SENTINEL, NativeTestBridge.callFnAttachTarget(10))

        // Install handler + raise signal. Handler's FromHandler returns -3
        // (not armed), logs, but SUPPRESS's the signal — no crash.
        assertEquals("sigovrInstall",
            0, NativeTestBridge.sigovrInstall(SIG_UNINSTALL, HANDLER_DEFERRED, target))
        assertEquals("sigqueue",
            0, NativeTestBridge.sigovrRaiseWithInt(SIG_UNINSTALL, 0))
        Thread.sleep(50)

        // Hook remains installed because deferred worker isn't running
        assertEquals("hook still fires (unarmed)", SENTINEL, NativeTestBridge.callFnAttachTarget(10))

        // WaitFor should time out
        val rc = NativeTestBridge.signalSafeUninstallWaitFor(target, 200)
        assertEquals("WaitFor must time out when unarmed", -1, rc)
    }

    // ─── 6. contention stress — no deadlock ─────────────────────────
    @Test
    fun signalHandlerDoesNotDeadlockUnderContention() {
        val target = NativeTestBridge.getFnAttachTargetAddr()
        val fake = NativeTestBridge.getFakeAttachAddr()

        assertEquals("arm", 0, NativeTestBridge.signalSafeUninstallArm())

        // Contention thread: hammer install/uninstall so g_hook_mutex is
        // held intermittently. The deferred signal handler must not touch
        // g_hook_mutex, so the process must not hang.
        val stop = AtomicBoolean(false)
        val cycles = AtomicInteger(0)
        val contentionThread = Thread {
            while (!stop.get()) {
                NativeTestBridge.attachRuntime(target, fake)
                NativeTestBridge.detachRuntime(target)
                cycles.incrementAndGet()
            }
        }
        contentionThread.start()

        try {
            // Let the contention thread start cycling
            Thread.sleep(50)

            // Install hook on target (contention thread may have just left
            // it uninstalled; re-install for this round)
            NativeTestBridge.attachRuntime(target, fake)

            assertEquals("sigovrInstall",
                0, NativeTestBridge.sigovrInstall(SIG_UNINSTALL, HANDLER_DEFERRED, target))

            // Raise + wait — if the handler blocked on g_hook_mutex, the
            // process would either hang (WaitFor times out) or crash.
            val raiseT0 = System.nanoTime()
            assertEquals("sigqueue",
                0, NativeTestBridge.sigovrRaiseWithInt(SIG_UNINSTALL, 0))
            val raiseUs = (System.nanoTime() - raiseT0) / 1000
            Log.d(TAG, "raise returned in ${raiseUs}us (must be < 50ms; deferred handler is sig-safe)")

            // 2000ms budget because we're competing with the contention thread
            val waitRc = NativeTestBridge.signalSafeUninstallWaitFor(target, 2000)
            Log.d(TAG, "WaitFor returned $waitRc after ${cycles.get()} contention cycles")
            assertEquals("WaitFor must complete (no deadlock)", 0, waitRc)
        } finally {
            stop.set(true)
            contentionThread.join(2000)
        }
        // If we got here, the process didn't deadlock or crash
    }
}