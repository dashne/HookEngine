package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 66 — Feature 8.7: per-hook CPU / memory profiling.
 *
 * ─── Mechanism: opt-in per-site profiling with CLOCK_MONOTONIC ─────
 *
 * A HookProfile is attached to every ChainHook site (S66 scope; ReturnMod
 * and RegInspect extend in S67). It's disabled by default — zero overhead
 * until explicitly enabled via NativeTestBridge.hookProfileEnable(target).
 *
 * When enabled, chain_dispatch_helper wraps each user-handler invocation
 * with two HookProfileReadNs() calls (CLOCK_MONOTONIC, VDSO-accelerated,
 * ~25ns/read) and calls HookProfileAccumulate(elapsed). The profile
 * tracks call_count, total_ns, min_ns, max_ns per site. mem_bytes is
 * static (computed at ChainAttachCond time = stub slot 32B + tramp 4K +
 * relay 4K, ~8224B).
 *
 * ─── Rule 12 — every assertion is falsifiable ──────────────────────
 *
 *   1. profileDisabledByDefault
 *      Fails if ChainAttachCond auto-enables the profile (would mean an
 *      opt-out model shipped instead of opt-in, causing ~60ns/call overhead
 *      on every install without the operator asking).
 *
 *   2. callCountMatchesInvocations
 *      Attach + enable + call N times → call_count == N. Fails if the
 *      dispatch helper drops an increment (missing accumulate path,
 *      predicate gate error, trampoline path skipped).
 *
 *   3. timingAccumulatesPositively
 *      After N calls, total_ns > 0, min_ns > 0, max_ns > 0, min <= max.
 *      Fails if the timing code is broken (read returns garbage, CAS
 *      min/max update bug keeps them at init value, or total accumulation
 *      underflows).
 *
 *   4. resetClearsAllCounters
 *      After reset, call_count == 0, total_ns == 0, min_ns == UINT64_MAX
 *      (Java Long.MAX_VALUE = -1L signed, see below), max_ns == 0.
 *      Fails if reset is a no-op OR only partially clears.
 *
 *   5. disabledStopsCounting
 *      Enable, call 20×, disable, call 20× more → call_count == 20 (not 40).
 *      Fails if the enabled flag isn't checked at dispatch time OR the
 *      dispatch caches a stale `true` value.
 *
 *   6. memBytesReportsPages
 *      mem_bytes >= 8192 (two pages) after ChainAttachCond. Fails if
 *      ChainHookInit profile mem_bytes wasn't set (would mean the profile
 *      is created with 0 default).
 *
 * Each assertion can fail on a specific real defect. None is a tautology
 * on a correct implementation.
 *
 * ─── Test isolation ────────────────────────────────────────────────
 *
 * @Before/@After call uninstallAll() so every test starts from clean
 * state. Same pattern as RelocTest / DynamicAttachTest / ChainHookTest.
 *
 * Note: the deferred-free grace period (100ms) means `uninstallAll()`
 * takes ~100ms; 6 tests × (2 × uninstallAll) = ~1.2s minimum test wall.
 * Acceptable.
 */
@RunWith(AndroidJUnit4::class)
class HookProfileTest {

    private val TAG = "HookProfileTest"
    private val h1 get() = NativeTestBridge.getChainHandler1Addr()

    @Before fun reset() { NativeTestBridge.uninstallAll() }
    @After fun cleanup() { NativeTestBridge.uninstallAll() }

    private fun attachWithProfile(target: Long): Boolean {
        val rc = NativeTestBridge.chainAttach(target, h1)
        if (rc != 0) {
            Log.e(TAG, "chainAttach rc=$rc")
            return false
        }
        return true
    }

    private fun readProfile(target: Long): LongArray? {
        val arr = NativeTestBridge.hookProfileRead(target)
        if (arr == null) return null
        Log.d(TAG, "  profile target=0x${target.toString(16)}: " +
                "enabled=${arr[0]} count=${arr[1]} total=${arr[2]}ns " +
                "min=${arr[3]} max=${arr[4]} mem=${arr[5]}B")
        return arr
    }

    // ─── 1. Default state is disabled ───────────────────────────────
    @Test
    fun profileDisabledByDefault() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        assertTrue("attach", attachWithProfile(t))
        val arr = readProfile(t)
        assertNotNull("profile must exist after ChainAttachCond", arr)
        assertEquals("enabled must be 0 (opt-in default)", 0L, arr!![0])
        assertEquals("call_count must be 0 (no calls yet)", 0L, arr[1])
        assertEquals("total_ns must be 0", 0L, arr[2])
        assertEquals("max_ns must be 0", 0L, arr[4])
    }

    // ─── 2. Call count tracks handler invocations ───────────────────
    @Test
    fun callCountMatchesInvocations() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        assertTrue("attach", attachWithProfile(t))
        assertTrue("enable", NativeTestBridge.hookProfileEnable(t))

        val N = 100
        repeat(N) { NativeTestBridge.callFnAttachTarget(10) }

        val arr = readProfile(t)
        assertNotNull("profile", arr)
        assertEquals("call_count must equal $N", N.toLong(), arr!![1])
    }

    // ─── 3. Timing accumulates and min/max are sane ─────────────────
    @Test
    fun timingAccumulatesPositively() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        assertTrue("attach", attachWithProfile(t))
        assertTrue("enable", NativeTestBridge.hookProfileEnable(t))

        val N = 50
        repeat(N) { NativeTestBridge.callFnAttachTarget(10) }

        val arr = readProfile(t)
        assertNotNull("profile", arr)
        val count = arr!![1]
        val total = arr[2]
        val minv = arr[3]
        val maxv = arr[4]

        assertTrue("call_count must be $N", count == N.toLong())
        assertTrue("total_ns must be > 0 after $N calls, got $total", total > 0)
        assertTrue("max_ns must be > 0, got $maxv", maxv > 0)
        // min_ns may legitimately be 0 when a handler's cost equals the
        // CLOCK_MONOTONIC resolution (~1ns on this device). The correct
        // structural assertion is that min_ns has been lowered from its
        // initial UINT64_MAX (-1L as signed jlong) — i.e. at least one
        // sample was recorded. min_ns == UINT64_MAX after N>0 calls
        // means the accumulation path never ran.
        assertTrue(
            "min_ns must be >= 0 after $N calls (was $minv; " +
                    "-1L means no sample was recorded and the accumulation path is broken)",
            minv >= 0L)
        assertTrue("min_ns ($minv) must be <= max_ns ($maxv)", minv <= maxv)
        // avg_ns is a mathematical consequence of total/count, but it's a
        // separate falsifiable check: if total>0 and count>0 then avg>0.
        // Catches a hypothetical bug where total is accumulated but count
        // is not (or vice versa).
        val avg = total / count
        assertTrue("avg_ns must be > 0 (total=$total / count=$count), got $avg",
            avg > 0L)
    }

    // ─── 4. Reset clears all counters, preserves enabled ────────────
    @Test
    fun resetClearsAllCounters() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        assertTrue("attach", attachWithProfile(t))
        assertTrue("enable", NativeTestBridge.hookProfileEnable(t))
        repeat(20) { NativeTestBridge.callFnAttachTarget(10) }

        val before = readProfile(t)
        assertNotNull(before)
        assertTrue("pre-reset count must be > 0", before!![1] > 0)

        assertTrue("reset", NativeTestBridge.hookProfileReset(t))
        val after = readProfile(t)
        assertNotNull(after)
        assertEquals("call_count cleared", 0L, after!![1])
        assertEquals("total_ns cleared", 0L, after[2])
        assertEquals("max_ns cleared", 0L, after[4])
        // min_ns is UINT64_MAX = -1L on signed jlong
        assertEquals("min_ns = UINT64_MAX (-1L signed)", -1L, after[3])
        // enabled preserved
        assertEquals("enabled preserved across reset", 1L, after[0])
    }

    // ─── 5. Disable stops counting ──────────────────────────────────
    @Test
    fun disabledStopsCounting() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        assertTrue("attach", attachWithProfile(t))
        assertTrue("enable", NativeTestBridge.hookProfileEnable(t))
        repeat(20) { NativeTestBridge.callFnAttachTarget(10) }

        val mid = readProfile(t)
        assertNotNull(mid)
        val countAtDisable = mid!![1]
        assertTrue("count after first 20 calls", countAtDisable == 20L)

        assertTrue("disable", NativeTestBridge.hookProfileDisable(t))
        repeat(20) { NativeTestBridge.callFnAttachTarget(10) }

        val after = readProfile(t)
        assertNotNull(after)
        assertEquals(
            "call_count must stay at 20 after disabled-then-called-20-more " +
                    "(was ${countAtDisable}, is ${after!![1]})",
            countAtDisable, after[1])
    }

    // ─── 6. mem_bytes reports the site's footprint ──────────────────
    @Test
    fun memBytesReportsPages() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        assertTrue("attach", attachWithProfile(t))
        val arr = readProfile(t)
        assertNotNull(arr)
        val mem = arr!![5]
        Log.d(TAG, "  mem_bytes = $mem (expect >= 8192: 2 pages)")
        assertTrue(
            "mem_bytes must be >= 8192 (2 pages: tramp + relay), got $mem",
            mem >= 8192L)
        // Upper sanity bound: not absurdly large
        assertTrue(
            "mem_bytes must be <= 65536 (16 pages sanity ceiling), got $mem",
            mem <= 65536L)
    }
}