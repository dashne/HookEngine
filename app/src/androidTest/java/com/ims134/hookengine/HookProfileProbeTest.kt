package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 66 — Feature 8.7 (per-hook profiling) probe.
 *
 * Diagnostic only. No assertions. All output to logcat under "HookProfileProbe".
 *
 * Feature 8.7's one-sentence exit: **per-hook CPU and memory profiling
 * becomes queryable via JNI, with falsifiable assertions that a hooked
 * call path costs measurably more than baseline AND that the recorded
 * profile matches the observed call count and approx. cumulative time.**
 *
 * Five questions this probe answers:
 *
 *   Q1. Per-site dispatch cost. For each of the 5 main hook primitives
 *       (attach / chain / retmod / reginspect-passthru / svc), measure
 *       per-call latency baseline vs. hooked. The delta is what the
 *       per-hook profiler will add overhead to. Informs: is
 *       instrumentation feasible without observable slowdown?
 *
 *   Q2. System.nanoTime() reference cost. Kotlin-level timing is ~30-50ns
 *       on modern ARM64. Informs: whether CNTVCT_EL0 (mrs, ~2ns) is
 *       required for the instrumentation or Kotlin timing is viable.
 *
 *   Q3. Static analysis: which sites have C++ dispatch helpers (extensible
 *       via adding cntvct reads) vs. which have only inline asm relays
 *       (would require asm changes to instrument).
 *
 *   Q4. Static analysis: per-site memory footprint (relay page + tramp
 *       page + auxiliary struct) for each primitive type.
 *
 *   Q5. Static: any existing profiler instrumentation to extend?
 *
 * The probe records baseline vs hooked via loop-with-N-iterations, not
 * per-call timing — JNI overhead (~50-100ns) makes per-call timing
 * unreliable. Loop measurement amortizes JNI, and delta between baseline
 * and hooked loops cancels JNI overhead entirely.
 *
 * @Before/@After uninstallAll() — every measurement is from a clean state,
 * canary harness untouched.
 */
@RunWith(AndroidJUnit4::class)
class HookProfileProbeTest {

    private val TAG = "HookProfileProbe"
    private val N = 50000
    private val WARMUP = 5000

    @Before fun reset() { NativeTestBridge.uninstallAll() }
    @After fun cleanup() { NativeTestBridge.uninstallAll() }

    private fun measurePerCallNs(label: String, body: () -> Unit): Long {
        repeat(WARMUP) { body() }
        val t0 = System.nanoTime()
        repeat(N) { body() }
        val elapsed = System.nanoTime() - t0
        val perCall = elapsed / N
        Log.d(TAG, "  $label: ${perCall}ns/call (elapsed=${elapsed}ns / $N)")
        return perCall
    }

    // ─── Q1: per-site dispatch cost ────────────────────────────────
    private fun q1PerSiteCost() {
        Log.d(TAG, "=== Q1: per-site dispatch cost (baseline vs hooked) ===")
        val t = NativeTestBridge.getFnAttachTargetAddr()
        val fake = NativeTestBridge.getFakeAttachAddr()

        // 1. attachRuntime — hard replacement, no trampoline
        NativeTestBridge.uninstallAll()
        val attachBase = measurePerCallNs("fn_attach_target (no hook)") {
            NativeTestBridge.callFnAttachTarget(10)
        }
        NativeTestBridge.attachRuntime(t, fake)
        val attachHooked = measurePerCallNs("fn_attach_target (attachRuntime→fake)") {
            NativeTestBridge.callFnAttachTarget(10)
        }
        NativeTestBridge.detachRuntime(t)
        Log.d(TAG, "  → attachRuntime overhead: ${attachHooked - attachBase}ns")

        // 2. ChainHook — 1 handler, trampoline
        NativeTestBridge.uninstallAll()
        val h1 = NativeTestBridge.getChainHandler1Addr()
        val chainBase = measurePerCallNs("fn_attach_target (no hook)") {
            NativeTestBridge.callFnAttachTarget(10)
        }
        NativeTestBridge.chainAttach(t, h1)
        val chainHooked = measurePerCallNs("fn_attach_target (chain 1 handler)") {
            NativeTestBridge.callFnAttachTarget(10)
        }
        NativeTestBridge.uninstallAll()
        Log.d(TAG, "  → ChainHook 1-handler overhead: ${chainHooked - chainBase}ns")

        // 3. RetMod
        NativeTestBridge.uninstallAll()
        val mod = NativeTestBridge.getRetModAdd1000Addr()
        val retmodBase = measurePerCallNs("fn_attach_target (no hook)") {
            NativeTestBridge.callFnAttachTarget(10)
        }
        NativeTestBridge.attachRetMod(t, mod)
        val retmodHooked = measurePerCallNs("fn_attach_target (retmod add1000)") {
            NativeTestBridge.callFnAttachTarget(10)
        }
        NativeTestBridge.uninstallAll()
        Log.d(TAG, "  → RetMod overhead: ${retmodHooked - retmodBase}ns")

        // 4. RegInspect PASSTHRU
        NativeTestBridge.uninstallAll()
        val rt = NativeTestBridge.getRegInspectGpTargetAddr()
        val regBase = measurePerCallNs("fn_reginspect_gp (no hook)") {
            NativeTestBridge.callRegInspectGp(10, 20)
        }
        NativeTestBridge.attachRegInspect(rt, 1)  // MODE_PASSTHRU
        val regHooked = measurePerCallNs("fn_reginspect_gp (reginspect passthru)") {
            NativeTestBridge.callRegInspectGp(10, 20)
        }
        NativeTestBridge.uninstallAll()
        Log.d(TAG, "  → RegInspect passthru overhead: ${regHooked - regBase}ns (784B frame + dispatcher)")

        // 5. SvcHook — SIGTRAP delivery (very slow, smaller N)
        NativeTestBridge.uninstallAll()
        val svcN = 1000
        val svcWarm = 100
        val st = NativeTestBridge.getSvcGetpidSvcAddr()
        val sh = NativeTestBridge.getSvcHandlerIdentityAddr()
        repeat(svcWarm) { NativeTestBridge.callSvcGetpid() }
        val sT0 = System.nanoTime()
        repeat(svcN) { NativeTestBridge.callSvcGetpid() }
        val svcBase = (System.nanoTime() - sT0) / svcN
        Log.d(TAG, "  svc_getpid (no hook): ${svcBase}ns/call")
        NativeTestBridge.svcHookAttach(st, 172, sh)
        repeat(svcWarm) { NativeTestBridge.callSvcGetpid() }
        val sT1 = System.nanoTime()
        repeat(svcN) { NativeTestBridge.callSvcGetpid() }
        val svcHooked = (System.nanoTime() - sT1) / svcN
        NativeTestBridge.svcHookDetach(st)
        Log.d(TAG, "  svc_getpid (SIGTRAP hooked): ${svcHooked}ns/call")
        Log.d(TAG, "  → SvcHook overhead: ${svcHooked - svcBase}ns (signal delivery dominates)")
    }

    // ─── Q2: System.nanoTime() reference cost ─────────────────────
    private fun q2NanoTimeCost() {
        Log.d(TAG, "=== Q2: System.nanoTime() reference cost ===")
        val empty = measurePerCallNs("empty loop body") { @Suppress("UNUSED_VARIABLE") val x = 1 }
        val nt = measurePerCallNs("System.nanoTime()") { @Suppress("UNUSED_VARIABLE") val x = System.nanoTime() }
        Log.d(TAG, "  empty body: ${empty}ns")
        Log.d(TAG, "  nanoTime(): ${nt}ns (delta = ${nt - empty}ns pure nanoTime)")
        Log.d(TAG, "  → CNTVCT_EL0 target: ~2ns (10-20× faster than nanoTime)")
        Log.d(TAG, "  → Design: native instrumentation with 'mrs x0, cntvct_el0' in dispatch helpers, not Kotlin nanoTime")
    }

    // ─── Q3: extensibility (static analysis) ──────────────────────
    private fun q3Extensibility() {
        Log.d(TAG, "=== Q3: which sites have C++ dispatch helpers (extensible) ===")
        Log.d(TAG, "  From source (static):")
        Log.d(TAG, "    ChainHook       → chain_dispatch_helper (C++)  → EXTENSIBLE")
        Log.d(TAG, "    ReturnMod       → retmod_dispatch_helper (C++) → EXTENSIBLE")
        Log.d(TAG, "    RegInspect      → reginspect_dispatch_helper (C++) → EXTENSIBLE")
        Log.d(TAG, "    SvcHook         → svc_sigtrap_handler (C++, signal ctx) → EXTENSIBLE, timing scope limited")
        Log.d(TAG, "    ArtMethodTrace  → JavaHook wrapper (C++ calling orig_entry) → EXTENSIBLE (JNI round-trip dominates)")
        Log.d(TAG, "    InlineHooker    → relay = pure asm ('ldr x17; br x17') → NO C++ hook point")
        Log.d(TAG, "    GOT hooks       → Fake_* functions (C++) → EXTENSIBLE (path check dominates, not hook)")
        Log.d(TAG, "")
        Log.d(TAG, "  Design: 5 of 7 sites are directly extensible with CNTVCT_EL0 reads.")
        Log.d(TAG, "  InlineHooker requires asm relay change — defer or use fake function entry (Fake_* cost is user-supplied fake, not relay cost).")
    }

    // ─── Q4: memory footprint (static analysis) ───────────────────
    private fun q4MemoryFootprint() {
        Log.d(TAG, "=== Q4: per-site memory footprint (from struct layout) ===")
        Log.d(TAG, "  InlineHooker: relay page (4KB) + tramp page (4KB) = 8KB/site")
        Log.d(TAG, "  ChainHook:    shares 4KB stub page (32 slots × 32B), + relay + tramp = ~8KB/site")
        Log.d(TAG, "  ReturnMod:    shadow tramp (4KB) + relay page (4KB) = 8KB/site")
        Log.d(TAG, "  RegInspect:   shadow tramp (4KB) + relay page (4KB) + 784B snapshot = ~8.8KB/site")
        Log.d(TAG, "  SvcHook:      32B slot in shared g_slots[MAX_SVC_SLOTS=16] array, no extra pages")
        Log.d(TAG, "  ArtMethodTrace: no extra pages (ArtMethod field patch) + handler array")
        Log.d(TAG, "  GOT hook:     no extra pages (single pointer swap in GOT)")
        Log.d(TAG, "")
        Log.d(TAG, "  Design: HookProfile struct in HookHandle can carry 'mem_bytes' computed at install time")
        Log.d(TAG, "  (relay_size + tramp_size + per-type aux). No new allocations for the profile itself.")
    }

    // ─── Q5: existing profiler instrumentation ────────────────────
    private fun q5ExistingProfiler() {
        Log.d(TAG, "=== Q5: existing profiler instrumentation ===")
        Log.d(TAG, "  Static grep results:")
        Log.d(TAG, "    HookProfile / g_profile / hook_profile: none")
        Log.d(TAG, "    clock_gettime in native: 1 hit (InlineHooker.cpp SigUninstallNowMs)")
        Log.d(TAG, "    cntvct: none")
        Log.d(TAG, "    pmccntr_el0: none")
        Log.d(TAG, "")
        Log.d(TAG, "  → Feature 8.7 starts from zero.")
        Log.d(TAG, "  → Design target: HookProfile in HookRegistry, CNTVCT_EL0 in dispatch helpers,")
        Log.d(TAG, "    JNI surface: hookProfileStats(target): long[6] = {call_count, total_ns,")
        Log.d(TAG, "    min_ns, max_ns, avg_ns, mem_bytes}; hookProfileReset().")
    }

    @Test
    fun probeHookProfileSurface() {
        Log.d(TAG, "########## S66 HookProfileProbeTest START ##########")
        q1PerSiteCost()
        q2NanoTimeCost()
        q3Extensibility()
        q4MemoryFootprint()
        q5ExistingProfiler()
        Log.d(TAG, "########## S66 HookProfileProbeTest END ##########")
    }
}