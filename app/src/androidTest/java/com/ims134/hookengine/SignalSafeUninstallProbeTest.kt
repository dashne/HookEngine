package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 65 — Feature 6.8: signal-safe uninstall. Probe.
 *
 * Diagnostic only. No assertions. Rule 12 does not apply to probes — the
 * purpose is to measure, not to gate. All output goes to logcat under
 * tag "SigSafeUninstallProbe".
 *
 * Feature 6.8's one-sentence exit: **uninstall is safe from any signal
 * handler context.** The current `UninstallInlineHook` in InlineHooker.cpp
 * acquires `std::lock_guard<std::mutex> g_hook_mutex` and then calls
 * `FindHandle` / `RegistryFree` which both acquire `std::lock_guard<std::mutex>
 * g_reg_mutex`. Neither primitive is async-signal-safe. Then it calls
 * `nanosleep(100ms)` which is also not async-signal-safe.
 *
 * The probe measures:
 *
 *   Q1. Current uninstall path (code inspection) — audit every call in
 *       UninstallInlineHook, classify each as sig-safe / not sig-safe.
 *
 *   Q2. Baseline normal-thread uninstall latency — how long does one
 *       install/uninstall cycle take? This informs the deferred-worker
 *       poll timeout in the eventual test class (100ms grace × N hooks).
 *
 *   Q3. Signal-context uninstall feasibility check — document what's
 *       needed and why the current JNI surface can't test it directly.
 *       (The Kotlin sigaction handler goes through SignalOverride.cpp
 *       which uses sigaction + so_dispatch, but so_dispatch is not
 *       wired to call UninstallInlineHook.)
 *
 *   Q4. Deferred-free grace period — confirm the current 100ms default
 *       and note its impact on signal-handler return time.
 *
 *   Q5. Signal stack requirement — MINSIGSTKSZ / SIGSTKSZ analysis for
 *       the eventual handler. Deferred approach needs << 1KB.
 *
 *   Q6. POSIX async-signal-safety audit — enumerate which calls in the
 *       current path are / aren't on the POSIX async-signal-safe list.
 *
 * The deliverable after this probe: implement deferred uninstall via
 * lock-free SPSC ring buffer + worker thread + eventfd wakeup. This is
 * the industry-standard pattern (Frida, Cydia Substrate, Xposed all use
 * deferred uninstall for the same reason).
 *
 * @Before installFileHooks() — same pattern as every probe since S60.
 */
@RunWith(AndroidJUnit4::class)
class SignalSafeUninstallProbeTest {

    private val TAG = "SigSafeUninstallProbe"

    @Before
    fun reinstallHooks() {
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    // ─── Q1: current uninstall path audit ───────────────────────────
    private fun probeCurrentUninstallPath() {
        Log.d(TAG, "=== Q1: current UninstallInlineHook audit (code inspection) ===")
        Log.d(TAG, "  Source: InlineHooker.cpp, UninstallInlineHook(void* targetFunc)")
        Log.d(TAG, "  Step-by-step:")
        Log.d(TAG, "    1. std::lock_guard<std::mutex> lock(g_hook_mutex)")
        Log.d(TAG, "       → pthread_mutex_lock — NOT on POSIX async-signal-safe list")
        Log.d(TAG, "       → Self-deadlock if signal fires on thread already holding it")
        Log.d(TAG, "    2. FindHandle(targetFunc)")
        Log.d(TAG, "       → internally: std::lock_guard<mutex> g_reg_mutex")
        Log.d(TAG, "       → NOT sig-safe (second mutex)")
        Log.d(TAG, "    3. mprotect(target_page, RW)")
        Log.d(TAG, "       → syscall; sig-safe per POSIX (all syscalls are async-signal-safe)")
        Log.d(TAG, "    4. *(volatile uint32_t*)target = saved")
        Log.d(TAG, "       → memory write; sig-safe")
        Log.d(TAG, "    5. __builtin___clear_cache")
        Log.d(TAG, "       → compiler builtin → cacheflush syscall; sig-safe")
        Log.d(TAG, "    6. mprotect(target_page, origProt)")
        Log.d(TAG, "       → syscall; sig-safe")
        Log.d(TAG, "    7. RegistryFree(h)")
        Log.d(TAG, "       → internally: std::lock_guard<mutex> g_reg_mutex")
        Log.d(TAG, "       → NOT sig-safe (same second mutex, now on cleanup path)")
        Log.d(TAG, "    8. nanosleep(grace_ms=100)")
        Log.d(TAG, "       → internally: timer_create / clock_nanosleep")
        Log.d(TAG, "       → NOT on POSIX async-signal-safe list")
        Log.d(TAG, "       → blocks the handler for 100ms — signal latency spikes")
        Log.d(TAG, "    9. munmap(relay_page, relay_size)")
        Log.d(TAG, "       → POSIX async-signal-safe")
        Log.d(TAG, "   10. munmap(tramp_page, tramp_size)")
        Log.d(TAG, "       → POSIX async-signal-safe")
        Log.d(TAG, "")
        Log.d(TAG, "  Two mutex acquisitions + nanosleep = three async-signal-safety violations.")
        Log.d(TAG, "  Deadlock classes:")
        Log.d(TAG, "    Class A (self): signal fires on thread HOLDING g_hook_mutex,")
        Log.d(TAG, "                    handler tries to acquire → deadlock")
        Log.d(TAG, "    Class B (cross): signal fires on thread A while thread B holds")
        Log.d(TAG, "                     g_hook_mutex → A blocks in mutex_wait until B releases.")
        Log.d(TAG, "                     Not deadlock but 100ms+ signal latency, and if B itself")
        Log.d(TAG, "                     was interrupted by a signal handler wanting the same lock,")
        Log.d(TAG, "                     transitive deadlock via AB-BA.")
    }

    // ─── Q2: baseline normal-thread uninstall latency ───────────────
    private fun probeNormalThreadUninstallLatency() {
        Log.d(TAG, "=== Q2: baseline uninstall latency (normal thread) ===")
        val target = NativeTestBridge.getFnAttachTargetAddr()
        val fakeAddr = NativeTestBridge.getFakeAttachAddr()
        if (target <= 0x1000L || fakeAddr <= 0x1000L) {
            Log.d(TAG, "  fixture addr invalid: target=0x${target.toString(16)} fake=0x${fakeAddr.toString(16)}")
            return
        }
        var totalInstallNs = 0L
        var totalUninstallNs = 0L
        val cycles = 3
        repeat(cycles) { i ->
            val t0 = System.nanoTime()
            val iRc = NativeTestBridge.attachRuntime(target, fakeAddr)
            val t1 = System.nanoTime()
            // verify hook fires
            val hooked = NativeTestBridge.callFnAttachTarget(10)
            val uRc = NativeTestBridge.detachRuntime(target)
            val t2 = System.nanoTime()
            // verify restore
            val restored = NativeTestBridge.callFnAttachTarget(10)
            val installMs = (t1 - t0) / 1_000_000.0
            val uninstallMs = (t2 - t1) / 1_000_000.0
            totalInstallNs += (t1 - t0)
            totalUninstallNs += (t2 - t1)
            Log.d(TAG, "  cycle $i: install=${installMs}ms($iRc) uninstall=${uninstallMs}ms($uRc) hooked=$hooked restored=$restored")
        }
        val avgInstallMs = (totalInstallNs.toDouble() / cycles) / 1_000_000.0
        val avgUninstallMs = (totalUninstallNs.toDouble() / cycles) / 1_000_000.0
        Log.d(TAG, "  avg install:   ${avgInstallMs}ms")
        Log.d(TAG, "  avg uninstall: ${avgUninstallMs}ms (includes 100ms nanosleep grace)")
        Log.d(TAG, "  → deferred worker poll timeout in test should be >= ${avgUninstallMs.toInt() + 100}ms")
    }

    // ─── Q3: signal-context uninstall feasibility ───────────────────
    private fun probeSignalContextUninstall() {
        Log.d(TAG, "=== Q3: signal-context uninstall feasibility ===")
        Log.d(TAG, "  The Kotlin sigaction infrastructure exists (SignalOverride.cpp, S52).")
        Log.d(TAG, "  SignalOverrideInstall(SIGRTMIN+N, handler, ctx) + sigqueue raises the signal.")
        Log.d(TAG, "  BUT: so_dispatch in SignalOverride.cpp calls the user handler, which is a")
        Log.d(TAG, "       Kotlin lambda reference. From Kotlin signal context, calling")
        Log.d(TAG, "       NativeTestBridge.detachRuntime(...) is:")
        Log.d(TAG, "         (a) JNI call — illegal in signal handler on bionic")
        Log.d(TAG, "         (b) JVM allocation risk — instantiates jlong boxing, stack traces, etc.")
        Log.d(TAG, "         (c) NOT a valid test of the C++ uninstall path anyway")
        Log.d(TAG, "")
        Log.d(TAG, "  → The correct probe requires a C-level sigaction handler that directly")
        Log.d(TAG, "     calls UninstallInlineHook(target) — no JNI, no JVM.")
        Log.d(TAG, "  → This JNI entry point does not exist today. Adding it is the deliverable")
        Log.d(TAG, "     of Session 65 alongside the deferred queue.")
        Log.d(TAG, "")
        Log.d(TAG, "  Conclusion: the current JNI surface can measure Q2 latency but cannot")
        Log.d(TAG, "  exercise the C++ uninstall from signal context. Session 65 implements:")
        Log.d(TAG, "    - NativeSignalSafeUninstall(target) — SPSC enqueue, sig-safe")
        Log.d(TAG, "    - NativeSignalSafeUninstallWaitFor(target, timeoutMs) — poll for completion")
        Log.d(TAG, "    - NativeSignalSafeUninstallStats() — check queue/worker counters")
        Log.d(TAG, "    - Deferred worker thread spawned at engine init")
    }

    // ─── Q4: grace period ───────────────────────────────────────────
    private fun probeGracePeriod() {
        Log.d(TAG, "=== Q4: deferred-free grace period ===")
        Log.d(TAG, "  From InlineHooker.cpp:")
        Log.d(TAG, "    static long g_uninstall_grace_ms = 100;")
        Log.d(TAG, "    extern \"C\" void SetUninstallGraceMs(long ms)")
        Log.d(TAG, "    extern \"C\" long GetUninstallGraceMs()")
        Log.d(TAG, "")
        Log.d(TAG, "  Impact on signal-context uninstall:")
        Log.d(TAG, "    - Current path: 100ms nanosleep blocks the handler's return")
        Log.d(TAG, "    - This delays any nested signals, blocks related signal threads,")
        Log.d(TAG, "      and pushes the handler past the app's timing expectations")
        Log.d(TAG, "")
        Log.d(TAG, "  Deferred design:")
        Log.d(TAG, "    - Signal handler: SPSC enqueue + write(fd) → returns immediately")
        Log.d(TAG, "    - Worker thread: reads from eventfd, drains queue, calls")
        Log.d(TAG, "      UninstallInlineHook(target) → 100ms nanosleep happens HERE")
        Log.d(TAG, "    - Signal latency: microseconds")
        Log.d(TAG, "    - Test wait: poll FindHandle(target)==nullptr with timeout > 100ms")
    }

    // ─── Q5: signal stack requirement ──────────────────────────────
    private fun probeSigStack() {
        Log.d(TAG, "=== Q5: signal stack size analysis ===")
        Log.d(TAG, "  POSIX MINSIGSTKSZ (arm64 bionic): ~8192 bytes")
        Log.d(TAG, "  POSIX SIGSTKSZ:                    ~32768 bytes (often 4× MINSIGSTKSZ)")
        Log.d(TAG, "  Android apps: default thread stack is 8MB for main, but")
        Log.d(TAG, "  sigaction handlers without SA_ONSTACK use the interrupted thread's stack.")
        Log.d(TAG, "  On Android with SA_ONSTACK (which the S52 SignalOverride uses),")
        Log.d(TAG, "  the signal stack is the thread's altstack, typically 16-32KB.")
        Log.d(TAG, "")
        Log.d(TAG, "  Deferred design signal-handler stack usage:")
        Log.d(TAG, "    - Read g_tail, g_head (atomic, ~16 bytes locals)")
        Log.d(TAG, "    - Write g_queue[tail].target (8 bytes)")
        Log.d(TAG, "    - Store g_tail (atomic)")
        Log.d(TAG, "    - write(wakeup_fd, \"1\", 1) (single syscall)")
        Log.d(TAG, "  → << 256 bytes of stack. Even MINSIGSTKSZ is 32× more than needed.")
        Log.d(TAG, "")
        Log.d(TAG, "  Current impl signal-handler stack usage (if it were sig-safe):")
        Log.d(TAG, "    - pthread_mutex_lock: mutex internals + potential futex syscall")
        Log.d(TAG, "    - nanosleep: 2× struct timespec (32 bytes) + syscall frames")
        Log.d(TAG, "    - mprotect×2, munmap×2: syscall frames")
        Log.d(TAG, "  → ~2-4KB minimum. Tight against MINSIGSTKSZ. Deferred is strictly safer.")
    }

    // ─── Q6: POSIX async-signal-safety audit ───────────────────────
    private fun probeSigSafeViolations() {
        Log.d(TAG, "=== Q6: POSIX async-signal-safety audit ===")
        Log.d(TAG, "  Full list of calls in UninstallInlineHook path:")
        Log.d(TAG, "")
        Log.d(TAG, "  Call                                       Signal-safe?  Reason")
        Log.d(TAG, "  ─────────────────────────────────────────  ────────────  ──────────────────────────────")
        Log.d(TAG, "  std::lock_guard<mutex>(g_hook_mutex)        NO            pthread_mutex_lock not on list")
        Log.d(TAG, "  FindHandle → lock_guard(g_reg_mutex)        NO            same class")
        Log.d(TAG, "  Read /proc/self/maps (fopen/fgets/fclose)   NO            fopen/fgets not on list; but")
        Log.d(TAG, "                                                             only called from Install, not Uninstall")
        Log.d(TAG, "  mprotect                                    YES (syscall)  all syscalls are on POSIX list")
        Log.d(TAG, "  Volatile memory read/write                  YES            plain load/store, no sync primitives")
        Log.d(TAG, "  __builtin___clear_cache                     YES            emits cacheflush syscall")
        Log.d(TAG, "  RegistryFree → lock_guard(g_reg_mutex)      NO            pthread_mutex_lock not on list")
        Log.d(TAG, "  nanosleep                                   NO            internally calls clock_nanosleep (syscall")
        Log.d(TAG, "                                                             on POSIX safe list is clock_gettime, not")
        Log.d(TAG, "                                                             nanosleep; libc wrapper is not guaranteed safe)")
        Log.d(TAG, "  munmap                                      YES            POSIX async-signal-safe")
        Log.d(TAG, "")
        Log.d(TAG, "  Runtime hazard summary:")
        Log.d(TAG, "    - Two mutex acquisitions in the uninstall path (three if we count")
        Log.d(TAG, "      FindHandle and RegistryFree separately)")
        Log.d(TAG, "    - One nanosleep (100ms default)")
        Log.d(TAG, "    - Zero async-signal-safe synchronization primitives today")
        Log.d(TAG, "")
        Log.d(TAG, "  Minimum-viable signal-safe design:")
        Log.d(TAG, "    - Signal handler: SPSC ring-buffer enqueue (no locks) + eventfd write")
        Log.d(TAG, "    - Worker thread: drains ring buffer, calls UninstallInlineHook from")
        Log.d(TAG, "      normal thread context (all current lock/nanosleep/munmap calls safe)")
        Log.d(TAG, "    - Test API: enqueue from signal handler + poll FindHandle from main")
        Log.d(TAG, "                thread until nullptr (bounded wait)")
        Log.d(TAG, "    - Zero changes to UninstallInlineHook itself — it stays normal-context")
        Log.d(TAG, "    - Zero new mutexes — the ring buffer is SPSC, lock-free")
    }

    @Test
    fun probeSignalSafeUninstallSurface() {
        Log.d(TAG, "########## S65 SigSafeUninstallProbeTest START ##########")
        probeCurrentUninstallPath()
        probeNormalThreadUninstallLatency()
        probeSignalContextUninstall()
        probeGracePeriod()
        probeSigStack()
        probeSigSafeViolations()
        Log.d(TAG, "########## S65 SigSafeUninstallProbeTest END ##########")
    }
}