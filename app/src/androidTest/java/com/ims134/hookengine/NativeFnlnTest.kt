package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import dalvik.system.PathClassLoader
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.lang.reflect.Field

/**
 * Session 54 — Feature 4.2: Native ClassLoader chaining.
 *
 * Six falsifiable assertions on the native FNLN behavior, exercised via
 * ClassLoaderBridge.installFor (which delegates to nativeFnln).
 *
 * The existing ClassLoaderBridgeTest (3 tests) covers the Kotlin API
 * surface stability. This test covers the native merge mechanics at a
 * finer granularity: order preservation, dedup, idempotency, and
 * cross-loader independence.
 *
 * Rule 12 matrix:
 *   1. fnlnReturnsSuccessOnFreshLoader     — fails if installFor returns false
 *   2. fnlnSameLoaderIsNoOp                — fails if host chain modifies host
 *   3. fnlnHostElementsPrependedInOrder    — fails if merge order wrong
 *   4. fnlnGuestElementsPreservedAfterHost — fails if guest elements lost
 *   5. fnlnIdempotentOnSecondCall          — fails if dedup doesn't run
 *   6. fnlnDistinctLoaderInstancesIndependent — fails if chains leak across loaders
 */
@RunWith(AndroidJUnit4::class)
class NativeFnlnTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

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

    @Test
    fun fnlnReturnsSuccessOnFreshLoader() {
        val host = ClassLoaderBridge::class.java.classLoader!!
        val guest = PathClassLoader(ctx.applicationInfo.sourceDir, host.parent)
        assertTrue(
            "installFor must succeed on a fresh guest loader (nativeFnln >= 0)",
            ClassLoaderBridge.installFor(guest, host))
    }

    @Test
    fun fnlnSameLoaderIsNoOp() {
        val host = ClassLoaderBridge::class.java.classLoader!!
        val beforeCount = readDexElements(host)?.size ?: 0
        assertTrue(
            "installFor(host, host) must return true",
            ClassLoaderBridge.installFor(host, host))
        val afterCount = readDexElements(host)?.size ?: 0
        assertEquals(
            "installFor(host, host) must NOT modify host's dexElements",
            beforeCount, afterCount)
    }

    @Test
    fun fnlnHostElementsPrependedInOrder() {
        val host = ClassLoaderBridge::class.java.classLoader!!
        val guest = PathClassLoader(ctx.applicationInfo.sourceDir, host.parent)
        assertTrue("installFor", ClassLoaderBridge.installFor(guest, host))

        val guestElems = readDexElements(guest)
        val hostElems = readDexElements(host)
        assertNotNull("guest dexElements must be readable", guestElems)
        assertNotNull("host dexElements must be readable", hostElems)
        assertTrue("host elems must be non-empty", hostElems!!.isNotEmpty())

        val n = hostElems.size
        assertTrue(
            "after chain guest must have >= $n elements, got ${guestElems!!.size}",
            guestElems.size >= n)
        for (i in 0 until n) {
            assertEquals(
                "guest[$i] must equal host[$i] — host elements prepended in order",
                hostElems[i], guestElems[i])
        }
    }

    @Test
    fun fnlnGuestElementsPreservedAfterHost() {
        val host = ClassLoaderBridge::class.java.classLoader!!
        val guest = PathClassLoader(ctx.applicationInfo.sourceDir, host.parent)

        val beforeGuest = readDexElements(guest)?.filterNotNull() ?: emptyList<Any>()
        val beforeCount = beforeGuest.size
        assertTrue("fresh guest must have >= 1 element, got $beforeCount", beforeCount >= 1)

        assertTrue("installFor", ClassLoaderBridge.installFor(guest, host))

        val afterGuest = readDexElements(guest)?.filterNotNull() ?: emptyList<Any>()
        assertTrue(
            "after chain must have >= $beforeCount elements, got ${afterGuest.size}",
            afterGuest.size >= beforeCount)

        // Every original guest element must still be present (identity)
        for (orig in beforeGuest) {
            val present = afterGuest.any { it === orig }
            assertTrue(
                "original guest element must be preserved in merged array",
                present)
        }
    }

    @Test
    fun fnlnIdempotentOnSecondCall() {
        val host = ClassLoaderBridge::class.java.classLoader!!
        val guest = PathClassLoader(ctx.applicationInfo.sourceDir, host.parent)

        assertTrue("first installFor", ClassLoaderBridge.installFor(guest, host))
        val countAfterFirst = readDexElements(guest)?.size ?: 0
        assertTrue("chain must have produced elements", countAfterFirst > 0)

        assertTrue("second installFor", ClassLoaderBridge.installFor(guest, host))
        val countAfterSecond = readDexElements(guest)?.size ?: 0
        assertEquals(
            "second installFor must not change count — host elements " +
                    "already present are deduped by identity",
            countAfterFirst, countAfterSecond)
    }

    @Test
    fun fnlnDistinctLoaderInstancesIndependent() {
        val host = ClassLoaderBridge::class.java.classLoader!!

        val guestA = PathClassLoader(ctx.applicationInfo.sourceDir, host.parent)
        val guestB = PathClassLoader(ctx.applicationInfo.sourceDir, host.parent)
        val aBefore = readDexElements(guestA)?.size ?: 0
        val bBefore = readDexElements(guestB)?.size ?: 0
        assertEquals("both fresh loaders start with same count", aBefore, bBefore)
        assertTrue("fresh guest A must have elements", aBefore > 0)

        assertTrue("chain A", ClassLoaderBridge.installFor(guestA, host))
        val aAfter = readDexElements(guestA)?.size ?: 0
        val bAfter1 = readDexElements(guestB)?.size ?: 0
        assertTrue("A must grow", aAfter > aBefore)
        assertEquals("B must be unchanged by A's chain", bBefore, bAfter1)

        assertTrue("chain B", ClassLoaderBridge.installFor(guestB, host))
        val aFinal = readDexElements(guestA)?.size ?: 0
        val bFinal = readDexElements(guestB)?.size ?: 0
        assertEquals("A must be unchanged by B's chain", aAfter, aFinal)
        assertEquals("B must grow to match A", aAfter, bFinal)
    }
}