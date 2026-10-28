package com.ims134.hookengine

import android.os.Binder
import android.os.IBinder
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 58 — Feature 3.7: Native ServiceManager addition.
 *
 * Register a synthetic binder under any name. Composes 3.4's alias rewrite:
 *   - Register: C++ builds flat_binder_object bytes by calling
 *     Parcel.writeStrongBinder(binder) then reading the parcel's native
 *     buffer directly. (Parcel.marshall() refuses parcels containing
 *     binders — that was the first iteration's failure.)
 *   - Alias: name -> "activity" via ParcelRewriter_SetServiceManagerAlias
 *   - Pre-intercept: detect synthetic name in request BEFORE alias rewrite
 *   - Post-intercept: swap reply buffer (malloc + copy + pointer swap) to
 *     present our binder in place of the driver's activity BinderProxy.
 *     In-place mutation is blocked by the binder driver's read-only mmap;
 *     buffer replacement works.
 *
 * Rule 12:
 *   1. syntheticAddReturnsZero            — rc = 0
 *   2. syntheticGetBeforeAddReturnsNull   — null baseline
 *   3. syntheticGetAfterAddReturnsBinder  — non-null after
 *   4. syntheticGetReturnsSameInstance    — identity preserved
 *   5. syntheticRemoveClears              — null after remove (via fresh name)
 *   6. syntheticMultipleIndependent       — 2 synthetics, remove 1, other works
 *
 * ─── MIUI/API 37 cache-hit wall (same as 3.4) ─────────────────────
 *
 * `ServiceManager.sCache.clear()` (Kotlin reflection) does NOT clear the
 * deeper cache inside `getIServiceManager().getService2(name)` — this is
 * the exact wall 3.4 documented in Session 51. Once a name resolves via
 * getService once, subsequent lookups for that same name return the
 * cached value WITHOUT dispatching through IPCThreadState::transact.
 *
 * Consequence for this test class: any test that observes a name twice
 * via `smGetService(name)` — add-then-get, or add, remove, get — will
 * see stale cached values on the second lookup. Tests that need to
 * observe a second time use a FRESH name whose cache is cold, forcing a
 * transact and letting the alias/removal path actually fire. This is
 * the same test pattern 3.4's ServiceManagerTest uses (`a_baseline...`,
 * `a_aliasToNonexistent...` all use fresh timestamped names).
 */
@RunWith(AndroidJUnit4::class)
class SyntheticServiceTest {

    private val TAG = "SynthSvcTest"

    private fun smGetService(name: String): IBinder? {
        val smCls = Class.forName("android.os.ServiceManager")
        val m = smCls.getDeclaredMethod("getService", String::class.java)
        m.isAccessible = true
        @Suppress("UNCHECKED_CAST")
        return m.invoke(null, name) as? IBinder
    }

    private fun clearCache() {
        val smCls = Class.forName("android.os.ServiceManager")
        val f = smCls.getDeclaredField("sCache")
        f.isAccessible = true
        @Suppress("UNCHECKED_CAST")
        (f.get(null) as MutableMap<*, *>).clear()
    }

    @Before fun setup() {
        NativeTestBridge.syntheticClearAll()
        NativeTestBridge.uninstallAll()
        val ok = BinderTransactHook.install()
        assertTrue("BinderTransactHook.install: ${BinderTransactHook.lastError}", ok)
        clearCache()
    }

    @After fun cleanup() {
        NativeTestBridge.syntheticClearAll()
        NativeTestBridge.smClearOverrides()
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
        NativeTestBridge.uninstallAll()
    }

    @Test
    fun syntheticAddReturnsZero() {
        val synth = "com.ims134.test.synth_${System.currentTimeMillis()}"
        val binder = Binder()
        val rc = NativeTestBridge.syntheticAdd(synth, binder)
        Log.d(TAG, "syntheticAdd rc=$rc")
        assertEquals("syntheticAdd must return 0", 0, rc)
        assertEquals("count", 1, NativeTestBridge.syntheticCount())
    }

    @Test
    fun syntheticGetBeforeAddReturnsNull() {
        val synth = "com.ims134.test.synth_before_${System.currentTimeMillis()}"
        clearCache()
        assertNull("unregistered name must return null", smGetService(synth))
    }

    @Test
    fun syntheticGetAfterAddReturnsBinder() {
        val synth = "com.ims134.test.synth_after_${System.currentTimeMillis()}"
        val binder = Binder()
        assertEquals("add", 0, NativeTestBridge.syntheticAdd(synth, binder))
        clearCache()
        val retrieved = smGetService(synth)
        Log.d(TAG, "getService('$synth') = $retrieved")
        assertNotNull("registered name must return non-null", retrieved)
    }

    @Test
    fun syntheticGetReturnsSameInstance() {
        val synth = "com.ims134.test.synth_same_${System.currentTimeMillis()}"
        val binder = Binder()
        assertEquals("add", 0, NativeTestBridge.syntheticAdd(synth, binder))
        clearCache()
        val retrieved = smGetService(synth)
        Log.d(TAG, "retrieved=$retrieved expected=$binder")
        assertNotNull("must be non-null", retrieved)
        assertSame("must be the SAME Binder instance via JavaBBinderHolder cache",
            binder, retrieved)
    }

    /**
     * Remove-path proof via cache-cold lookups.
     *
     * Part 1 (synthVisible): proves add is observable — add, then a
     * cold-cache getService returns non-null (fires transact, alias
     * rewrites to "activity", driver returns real activity, reply buffer
     * swapped to our binder).
     *
     * Part 2 (synthGone): proves remove produces null — add then remove
     * WITHOUT any intermediate getService call (so synthGone stays cache-
     * cold), then a cold-cache getService fires transact. The remove path
     * rewrote the alias to _NOEXIST_SM_OVERRIDE, driver returns null for
     * unknown service, caller gets null.
     *
     * Two names (rather than one) is required by the getService2 cache
     * wall documented in the class header. Same-name re-lookup after
     * remove hits the deeper cache and returns the pre-remove binder.
     */
    @Test
    fun syntheticRemoveClears() {
        // Part 1: add is observable
        val synthVisible = "com.ims134.test.synth_vis_${System.currentTimeMillis()}"
        assertEquals("add vis", 0, NativeTestBridge.syntheticAdd(synthVisible, Binder()))
        assertEquals("count = 1", 1, NativeTestBridge.syntheticCount())
        clearCache()
        assertNotNull("visible after add", smGetService(synthVisible))

        // Clean up the vis probe
        assertEquals("remove vis", 0, NativeTestBridge.syntheticRemove(synthVisible))
        assertEquals("count = 0 after vis cleanup", 0, NativeTestBridge.syntheticCount())

        // Part 2: remove is observable (fresh name, never queried before removal)
        val synthGone = "com.ims134.test.synth_gone_${System.currentTimeMillis()}"
        assertEquals("add gone", 0, NativeTestBridge.syntheticAdd(synthGone, Binder()))
        assertEquals("count = 1", 1, NativeTestBridge.syntheticCount())
        assertEquals("remove gone", 0, NativeTestBridge.syntheticRemove(synthGone))
        assertEquals("count = 0", 0, NativeTestBridge.syntheticCount())
        clearCache()
        assertNull("gone after remove", smGetService(synthGone))
    }

    /**
     * Slot isolation without cache contamination.
     *
     * Add both, remove A — no getService between add and remove so both
     * names stay cache-cold. Then:
     *   - getService(A) → transact fires → alias _NOEXIST → null
     *   - getService(B) → transact fires → alias activity → non-null
     *
     * Proves removing A doesn't disturb B's registration, without
     * depending on same-name cache-invalidation (blocked by getService2's
     * own cache — see class header).
     */
    @Test
    fun syntheticMultipleIndependent() {
        val synthA = "com.ims134.test.synth_A_${System.currentTimeMillis()}"
        val synthB = "com.ims134.test.synth_B_${System.currentTimeMillis()}"
        val binderA = Binder()
        val binderB = Binder()

        // Add both — no getService yet, both cache-cold
        assertEquals("add A", 0, NativeTestBridge.syntheticAdd(synthA, binderA))
        assertEquals("add B", 0, NativeTestBridge.syntheticAdd(synthB, binderB))
        assertEquals("count = 2", 2, NativeTestBridge.syntheticCount())

        // Remove A — B's slot untouched
        assertEquals("remove A", 0, NativeTestBridge.syntheticRemove(synthA))
        assertEquals("count = 1", 1, NativeTestBridge.syntheticCount())

        // Both lookups are cache-cold → transact fires → alias applied.
        clearCache()
        assertNull("A removed", smGetService(synthA))
        assertNotNull("B still works", smGetService(synthB))
    }
}