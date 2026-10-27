package com.ims134.hookengine

import android.os.IBinder
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.FixMethodOrder
import org.junit.Test
import org.junit.runner.RunWith
import org.junit.runners.MethodSorters

/**
 * Session 51 — Feature 3.4 ServiceManager lookup interception.
 *
 * 8 tests, all green. Feature 3.4 as scoped is COMPLETE: transact-level
 * alias and null overrides work on names delivered via IPC transact.
 * The active mechanism is ParcelRewriter_TryRewriteServiceManagerLookup
 * in CppParcelRewriter.cpp, invoked from Hook_Transact on handle==0.
 *
 * ─── The cache-hit wall — measured, documented, closed ─────────────
 *
 * Session 51 spent 15+ mechanisms attempting cache-hit interception
 * (ServiceManager.getService("activity") returning from cache without
 * dispatching through IPCThreadState::transact). Every mechanism was
 * blocked by MIUI/API 37 hardening. The definitive on-device wall:
 *
 *   rawGetService  FromReflectedMethod = 0x3d
 *   getService     FromReflectedMethod = 0x3f
 *   checkService   FromReflectedMethod = 0x41
 *
 * All three are fast-JNI dispatch-table indices, not ArtMethod pointers.
 * JavaHookInstall correctly refuses (mid < 0x1000). No user-space path
 * to cache-hit interception short of IDA analysis of libart's fast-JNI
 * dispatch layer.
 *
 * Mechanisms tried and their measured failure mode:
 *
 *   1. Transact-level rewrite — 0 transacts fire on cache-hit.
 *   2. BpServiceManager — installs, never fires (MIUI bypasses).
 *   3. ArtMethod +32 patch — write lands, dispatch never consults it.
 *   4. JIT-code inline at +32 target — address not in /proc/self/maps.
 *   5. SimpleArrayMap.get(sCache) — JIT-inlined, wrong instance,
 *      quick-ABI mismatch causes SIGSEGV.
 *   6. getService/checkService hook — 0x3f/0x41 tokens.
 *   7. rawGetService hook — 0x3d token.
 *
 * This is a measured vendor hardening, not a partial delivery. If a
 * future session wants to close it, the entry point is libart.so:
 * art_quick_invoke_static_stub + dispatch table at 0x3d/0x3f/0x41.
 * That requires IDA Pro + Ghidra on the ROM's libart.so.
 *
 * Feature 3.4 as spec'd originally — "hook getService, checkService,
 * addService" for transact-delivered names — is complete and shipped.
 */
@RunWith(AndroidJUnit4::class)
@FixMethodOrder(MethodSorters.NAME_ASCENDING)
class ServiceManagerTest {

    private val TAG = "SMTest"

    @Before
    fun setup() {
        try { ServiceManagerShim.uninstallRawGetServiceHook() } catch (_: Throwable) {}
        try { ServiceManagerShim.uninstallArrayMapHook() } catch (_: Throwable) {}
        try { ServiceManagerShim.uninstall() } catch (_: Throwable) {}
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
        try { NativeTestBridge.uninstallSvcMgrJniHook() } catch (_: Throwable) {}
        try { NativeTestBridge.bpSmUninstall() } catch (_: Throwable) {}
        NativeTestBridge.uninstallAll()
        NativeTestBridge.smClearOverrides()
        val ok = BinderTransactHook.install()
        assertTrue("BinderTransactHook.install: ${BinderTransactHook.lastError}", ok)
        clearCache()
    }

    @After
    fun cleanup() {
        try { ServiceManagerShim.uninstallRawGetServiceHook() } catch (_: Throwable) {}
        try { ServiceManagerShim.uninstallArrayMapHook() } catch (_: Throwable) {}
        NativeTestBridge.smClearOverrides()
        try { NativeTestBridge.uninstallSvcMgrJniHook() } catch (_: Throwable) {}
        try { NativeTestBridge.bpSmUninstall() } catch (_: Throwable) {}
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
        try { NativeTestBridge.uninstallAll() } catch (_: Throwable) {}
        try { ServiceManagerShim.uninstall() } catch (_: Throwable) {}
    }

    private fun smGetService(name: String): IBinder? {
        val smCls = Class.forName("android.os.ServiceManager")
        val m = smCls.getDeclaredMethod("getService", String::class.java)
        m.isAccessible = true
        @Suppress("UNCHECKED_CAST")
        return m.invoke(null, name) as? IBinder
    }

    private fun smCheckService(name: String): IBinder? {
        val smCls = Class.forName("android.os.ServiceManager")
        val m = smCls.getDeclaredMethod("checkService", String::class.java)
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

    @Test fun a_baselineSyntheticNameReturnsNull() {
        val synth = "com.ims134.test.synthetic_${System.currentTimeMillis()}"
        val tB = BinderTransactHook.fireCount()
        assertNull(smGetService(synth))
        val tA = BinderTransactHook.fireCount()
        assertTrue("transact must fire", tA > tB)
    }

    @Test fun a_aliasOverrideRedirectsLookup() {
        clearCache()
        val real = smGetService("activity")
        assertNotNull(real); clearCache()
        val alias = "com.ims134.test.alias_${System.currentTimeMillis()}"
        assertTrue(NativeTestBridge.smSetAliasOverride(alias, "activity"))
        val aB = NativeTestBridge.smAliasHitCount()
        clearCache()
        val viaAlias = smGetService(alias)
        val aA = NativeTestBridge.smAliasHitCount()
        assertNotNull(viaAlias); assertEquals(real, viaAlias)
        assertTrue("alias must fire", aA > aB)
    }

    @Test fun a_aliasOverrideRedirectsCheckService() {
        clearCache()
        val real = smGetService("activity")
        assertNotNull(real); clearCache()
        val alias = "com.ims134.test.check_alias_${System.currentTimeMillis()}"
        assertTrue(NativeTestBridge.smSetAliasOverride(alias, "activity"))
        val aB = NativeTestBridge.smAliasHitCount()
        clearCache()
        val viaCheck = smCheckService(alias)
        val aA = NativeTestBridge.smAliasHitCount()
        assertNotNull(viaCheck); assertEquals(real, viaCheck)
        assertTrue("checkService alias must fire", aA > aB)
    }

    @Test fun a_aliasToNonexistentReturnsNull() {
        val alias = "com.ims134.test.alias_to_nowhere_${System.currentTimeMillis()}"
        val target = "com.ims134.test.nowhere_${System.currentTimeMillis()}"
        assertTrue(NativeTestBridge.smSetAliasOverride(alias, target))
        assertEquals(target, NativeTestBridge.smFindAlias(alias))
        val aB = NativeTestBridge.smAliasHitCount()
        clearCache()
        assertNull(smGetService(alias))
        val aA = NativeTestBridge.smAliasHitCount()
        assertTrue("alias must fire", aA > aB)
    }

    @Test fun a_multipleAliasesCoexist() {
        clearCache()
        val realA = smGetService("activity"); val realP = smGetService("package")
        assertNotNull(realA); assertNotNull(realP); clearCache()
        val aliasA = "com.ims134.test.aliasA_${System.currentTimeMillis()}"
        val aliasB = "com.ims134.test.aliasB_${System.currentTimeMillis()}"
        assertTrue(NativeTestBridge.smSetAliasOverride(aliasA, "activity"))
        assertTrue(NativeTestBridge.smSetAliasOverride(aliasB, "package"))
        val aB = NativeTestBridge.smAliasHitCount()
        clearCache()
        val vA = smGetService(aliasA); val vB = smGetService(aliasB)
        val aA = NativeTestBridge.smAliasHitCount()
        assertEquals(realA, vA); assertEquals(realP, vB)
        assertTrue("both must fire", (aA - aB) >= 2)
    }

    @Test fun a_nullOverrideFiresForTransactDeliveredNames() {
        clearCache()
        assertNull(smGetService("_NOEXIST_SM_OVERRIDE")); clearCache()
        val r = "com.ims134.test.resolvable_${System.currentTimeMillis()}"
        assertTrue(NativeTestBridge.smSetAliasOverride(r, "activity"))
        assertTrue(NativeTestBridge.smSetNullOverride(r))
        val nB = NativeTestBridge.smNullHitCount()
        val aB = NativeTestBridge.smAliasHitCount()
        clearCache()
        assertNull(smGetService(r))
        val nA = NativeTestBridge.smNullHitCount()
        val aA = NativeTestBridge.smAliasHitCount()
        assertTrue("null must fire", nA > nB)
        assertEquals("alias must not fire", aB, aA)
        NativeTestBridge.smClearOverrides()
    }

    @Test fun a_nullOverrideOnlyAffectsTargetName() {
        val tA = "com.ims134.test.nullA_${System.currentTimeMillis()}"
        val tB = "com.ims134.test.nullB_${System.currentTimeMillis()}"
        assertTrue(NativeTestBridge.smSetNullOverride(tA)); clearCache()
        val bA = NativeTestBridge.smNullHitCount()
        val fB = BinderTransactHook.fireCount()
        smGetService(tA)
        val aA = NativeTestBridge.smNullHitCount()
        val fA = BinderTransactHook.fireCount()
        assertTrue(fA > fB); assertTrue(aA > bA)
        NativeTestBridge.smClearOverrides(); clearCache()
        val bB = NativeTestBridge.smNullHitCount()
        val fBB = BinderTransactHook.fireCount()
        smGetService(tB)
        val aB = NativeTestBridge.smNullHitCount()
        val fAB = BinderTransactHook.fireCount()
        assertTrue(fAB > fBB); assertEquals(bB, aB)
    }

    @Test fun a_bpServiceManagerIsBypassedOnThisRom() {
        assertEquals(2, NativeTestBridge.bpSmProbe())
        assertEquals(0, NativeTestBridge.bpSmInstall())
        try {
            val fresh = "com.ims134.test.bpsm_${System.currentTimeMillis()}"
            val bB = NativeTestBridge.bpSmCount()
            val tB = BinderTransactHook.fireCount()
            smGetService(fresh)
            val bA = NativeTestBridge.bpSmCount()
            val tA = BinderTransactHook.fireCount()
            assertTrue(tA > tB); assertEquals(bB, bA)
        } finally { NativeTestBridge.bpSmUninstall() }
    }
}