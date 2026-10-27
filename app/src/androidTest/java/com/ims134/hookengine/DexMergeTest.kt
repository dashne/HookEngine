package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import dalvik.system.DexClassLoader
import dalvik.system.PathClassLoader
import org.junit.After
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.lang.reflect.Field

/**
 * Session 56 — Feature 4.4: DEX merging at runtime.
 *
 * Composes Feature 4.3's DexLoader with a multi-path `:`-separated
 * argument. `DexClassLoader` handles the multi-path form natively — it
 * creates one loader whose `dexElements` contains one entry per dex
 * source, in the order given. Session 56 has no new native mechanism;
 * it proves the multi-path argument works with the existing native
 * `DexLoaderCreate`.
 *
 * Two DEX sources:
 *   A: app APK (`targetCtx.applicationInfo.sourceDir`) → `ApplicationProxy`
 *   B: staged viber APK (`files/viber.apk`) → `ManifestViberApplication`
 *
 * Distinct class namespaces per source → the merged loader's resolution
 * is unambiguous.
 *
 * ─── ART initiator-binding rule (recap from S55) ──────────────────
 *
 * A class loaded via a `DexClassLoader` binds (dex_file, class_def) to
 * that loader. Chaining that loader's dex into a *different* loader and
 * asking for the same class fails with CNF. Therefore:
 *   - `dexMergedChainWorks` uses a FRESH merged loader and does NOT
 *     pre-load via it — chain first, load second.
 *   - `dexMergedIndependentInstances` creates two separate merged
 *     loaders, each loading appClass independently — separate DexFile
 *     instances per loader, so no cross-binding. S55's
 *     `dexLoaderResolvesAppClass` + `loadedClassInstantiable` both
 *     loaded appClass through separate loaders successfully — the
 *     binding rule is per-loader, not global.
 *
 * Rule 12 matrix (each fails on a distinct real defect):
 *   1. dexMergedCreate                        — non-null, correct type
 *   2. dexMergedResolvesFromSourceA           — app class visible
 *   3. dexMergedResolvesFromSourceB           — viber class visible
 *   4. dexMergedElementCount                  — merged >= A + B
 *   5. dexMergedChainWorks                    — chain into guest works
 *   6. dexMergedIndependentInstances          — separate loaders don't interfere
 */
@RunWith(AndroidJUnit4::class)
class DexMergeTest {

    private val TAG = "DexMergeTest"
    private val targetCtx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val testCtx get() = InstrumentationRegistry.getInstrumentation().context

    private val appClass = "com.ims134.hookengine.ApplicationProxy"
    private val viberClass = "com.viber.voip.ManifestViberApplication"

    private fun pathA(): String = targetCtx.applicationInfo.sourceDir
    private fun pathB(): String = File(targetCtx.filesDir, "viber.apk").absolutePath

    private fun makeOptDir(tag: String): String {
        val d = File(targetCtx.codeCacheDir, "dexmerge-$tag-${System.currentTimeMillis()}")
        d.mkdirs()
        return d.absolutePath
    }

    private fun canLoad(loader: ClassLoader, name: String): Boolean = try {
        loader.loadClass(name); true
    } catch (_: ClassNotFoundException) { false }
    catch (_: Throwable) { false }

    private fun readDexElements(loader: ClassLoader): Array<*>? {
        return try {
            val pathListField = findField(loader.javaClass, "pathList") ?: return null
            pathListField.isAccessible = true
            val pathList = pathListField.get(loader) ?: return null
            val elementsField = findField(pathList.javaClass, "dexElements") ?: return null
            elementsField.isAccessible = true
            @Suppress("UNCHECKED_CAST")
            elementsField.get(pathList) as? Array<*>
        } catch (_: Throwable) { null }
    }

    private fun findField(cls: Class<*>, name: String): Field? {
        var c: Class<*>? = cls
        while (c != null) {
            try { return c.getDeclaredField(name) }
            catch (_: NoSuchFieldException) {}
            c = c.superclass
        }
        return null
    }

    @Before fun setup() { NativeTestBridge.uninstallAll() }
    @After fun cleanup() { NativeTestBridge.uninstallAll() }

    @Test
    fun dexMergedCreate() {
        val merged = DexLoader.create("${pathA()}:${pathB()}", makeOptDir("create"), null)
        assertNotNull("DexLoader.create must return non-null for multi-path", merged)
        assertTrue(
            "result must be DexClassLoader, got ${merged!!.javaClass.name}",
            merged is DexClassLoader)
    }

    @Test
    fun dexMergedResolvesFromSourceA() {
        val merged = DexLoader.create("${pathA()}:${pathB()}", makeOptDir("resolveA"), null)
        assertNotNull("merged loader", merged)
        assertTrue(
            "merged DexClassLoader must resolve $appClass (source A)",
            canLoad(merged!!, appClass))
    }

    @Test
    fun dexMergedResolvesFromSourceB() {
        val merged = DexLoader.create("${pathA()}:${pathB()}", makeOptDir("resolveB"), null)
        assertNotNull("merged loader", merged)
        assertTrue(
            "merged DexClassLoader must resolve $viberClass (source B)",
            canLoad(merged!!, viberClass))
    }

    @Test
    fun dexMergedElementCount() {
        // Load A and B separately to establish baseline element counts.
        // Fresh opt dirs avoid cache collisions between the three loads.
        val loaderA = DexLoader.create(pathA(), makeOptDir("countA"), null)
        val loaderB = DexLoader.create(pathB(), makeOptDir("countB"), null)
        assertNotNull("loaderA", loaderA)
        assertNotNull("loaderB", loaderB)

        val countA = readDexElements(loaderA!!)?.size ?: 0
        val countB = readDexElements(loaderB!!)?.size ?: 0
        assertTrue("A must have >= 1 element, got $countA", countA >= 1)
        assertTrue("B must have >= 1 element, got $countB", countB >= 1)

        val merged = DexLoader.create("${pathA()}:${pathB()}", makeOptDir("countM"), null)
        assertNotNull("merged loader", merged)
        val mergedCount = readDexElements(merged!!)?.size ?: 0

        assertTrue(
            "merged count ($mergedCount) must be >= A count ($countA) + B count ($countB)",
            mergedCount >= countA + countB)
    }

    @Test
    fun dexMergedChainWorks() {
        // Guest = test APK only, boot parent. Cannot resolve app or viber
        // classes before chaining.
        val guest = PathClassLoader(testCtx.applicationInfo.sourceDir, null)
        assertTrue("guest can't find $appClass before", !canLoad(guest, appClass))
        assertTrue("guest can't find $viberClass before", !canLoad(guest, viberClass))

        val merged = DexLoader.create("${pathA()}:${pathB()}", makeOptDir("chain"), null)
        assertNotNull("merged loader", merged)

        // CRITICAL: do not pre-load via merged. Chain first (S55 ART rule).
        assertTrue("installFor(guest, merged)",
            ClassLoaderBridge.installFor(guest, merged!!))

        assertTrue(
            "guest must find $appClass after chain (source A element)",
            canLoad(guest, appClass))
        assertTrue(
            "guest must find $viberClass after chain (source B element)",
            canLoad(guest, viberClass))
    }

    @Test
    fun dexMergedIndependentInstances() {
        // Two separate merged loaders — each loads appClass independently.
        // Per-loader DexFile instances mean no cross-binding; S55's
        // dexLoaderResolvesAppClass + loadedClassInstantiable both proved
        // that pattern works for single-source loaders.
        val m1 = DexLoader.create("${pathA()}:${pathB()}", makeOptDir("indep1"), null)
        val m2 = DexLoader.create("${pathA()}:${pathB()}", makeOptDir("indep2"), null)
        assertNotNull("m1", m1)
        assertNotNull("m2", m2)

        assertTrue("m1 must resolve $appClass", canLoad(m1!!, appClass))
        assertTrue("m2 must resolve $appClass", canLoad(m2!!, appClass))
        assertTrue("m1 must resolve $viberClass", canLoad(m1, viberClass))
        assertTrue("m2 must resolve $viberClass", canLoad(m2, viberClass))
    }
}