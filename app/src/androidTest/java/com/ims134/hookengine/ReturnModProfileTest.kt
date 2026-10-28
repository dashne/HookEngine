package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 67 — Feature 8.7 extension: per-hook CPU / memory profiling
 * on ReturnMod sites (parallel to S66's ChainHook path).
 *
 * ─── Mechanism: identical pattern to HookProfileTest ─────────────
 *
 * A HookProfile is attached to every RetMod slot. Disabled by default;
 * enabled via NativeTestBridge.hookProfileEnable(target). When enabled,
 * retmod_dispatch_helper wraps the user modifier call in two
 * HookProfileReadNs() (CLOCK_MONOTONIC, VDSO-accelerated) calls and
 * calls HookProfileAccumulate(delta).
 *
 * mem_bytes = 3 × pageSize (12288): our shadow tramp (4K) + inline hook
 * relay (4K) + inline hook trampoline (4K, unused by RetMod but still
 * allocated by InstallInlineHookEx). Higher than ChainHook's 8224
 * because RetMod owns a dedicated shadow tramp plus the inline hook
 * tramp that we don't consume. Honest accounting — includes the wasted
 * page rather than hiding it.
 *
 * ─── Rule 12 — every assertion is falsifiable ──────────────────────
 *
 * Same 6 structural assertions as HookProfileTest, applied to the
 * ReturnMod path. Each can fail on a specific real defect.
 */
@RunWith(AndroidJUnit4::class)
class ReturnModProfileTest {

    private val TAG = "ReturnModProfileTest"
    private val add1000 get() = NativeTestBridge.getRetModAdd1000Addr()

    @Before fun reset() { NativeTestBridge.uninstallAll() }
    @After fun cleanup() { NativeTestBridge.uninstallAll() }

    private fun attachWithProfile(target: Long): Boolean {
        val rc = NativeTestBridge.attachRetMod(target, add1000)
        if (rc != 0) {
            Log.e(TAG, "attachRetMod rc=$rc")
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
    fun retModProfileDisabledByDefault() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        assertTrue("attach", attachWithProfile(t))
        val arr = readProfile(t)
        assertNotNull("profile must exist after AttachRetMod", arr)
        assertEquals("enabled must be 0 (opt-in default)", 0L, arr!![0])
        assertEquals("call_count must be 0 (no calls yet)", 0L, arr[1])
        assertEquals("total_ns must be 0", 0L, arr[2])
        assertEquals("max_ns must be 0", 0L, arr[4])
    }

    // ─── 2. Call count tracks modifier invocations ──────────────────
    @Test
    fun retModCallCountMatchesInvocations() {
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
    fun retModTimingAccumulatesPositively() {
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
    fun retModResetClearsAllCounters() {
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
    fun retModDisabledStopsCounting() {
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
    fun retModMemBytesReportsPages() {
        val t = NativeTestBridge.getFnAttachTargetAddr()
        assertTrue("attach", attachWithProfile(t))
        val arr = readProfile(t)
        assertNotNull(arr)
        val mem = arr!![5]
        Log.d(TAG, "  mem_bytes = $mem (expect >= 8192: 2 pages minimum)")
        assertTrue(
            "mem_bytes must be >= 8192 (2 pages minimum), got $mem",
            mem >= 8192L)
        // Sanity ceiling — pageSize 4096 → 12288 for 3 pages
        assertTrue(
            "mem_bytes must be <= 65536 (16 pages sanity ceiling), got $mem",
            mem <= 65536L)
    }
}