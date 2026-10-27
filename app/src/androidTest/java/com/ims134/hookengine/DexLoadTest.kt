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

/**
 * Session 55 — Feature 4.3: Dynamic DEX loading.
 *
 * Six falsifiable assertions on the native DexClassLoader construction
 * and its integration with Feature 4.2's chaining primitive.
 *
 * ─── ART quirk (discovered Session 55) ──────────────────────────────
 *
 * When a DexClassLoader loads a class, ART binds (dex_file, class_def) to
 * the DexClassLoader's defining classloader. If a different classloader
 * then asks for the SAME class through the SAME dex (via a chained
 * Element), ART refuses with ClassNotFoundException — it will not define
 * the class again from a dex already "owned" by a different initiator.
 *
 * Therefore the chain test must NOT pre-load the class via the
 * DexClassLoader before chaining. `independentGuestsStayIndependent`
 * proves the chain works precisely because it never pre-loads. And
 * `dexLoaderResolvesAppClass` / `loadedClassInstantiable` prove the
 * DexClassLoader itself works — separate tests, separate loaders.
 *
 * Rule 12 matrix:
 *   1. dexLoaderCreateReturnsDexClassLoader — non-null, correct type
 *   2. dexLoaderResolvesAppClass             — app class resolvable from new loader
 *   3. guestLoaderCannotResolveBeforeChain   — pre-chain assert: guest truly empty
 *   4. chainMakesAppClassVisibleToGuest      — post-chain assert: chaining works
 *   5. loadedClassInstantiable               — resolved class can be instantiated
 *   6. independentGuestsStayIndependent      — chaining one guest leaves other untouched
 */
@RunWith(AndroidJUnit4::class)
class DexLoadTest {

    private val TAG = "DexLoadTest"
    private val targetCtx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val testCtx get() = InstrumentationRegistry.getInstrumentation().context

    // App-only class — not in test APK, not in framework
    private val appClass = "com.ims134.hookengine.ApplicationProxy"

    private fun makeOptDir(tag: String): String {
        val d = File(targetCtx.codeCacheDir, "dexload-$tag-${System.currentTimeMillis()}")
        d.mkdirs()
        return d.absolutePath
    }

    private fun canLoad(loader: ClassLoader, name: String): Boolean = try {
        loader.loadClass(name); true
    } catch (_: ClassNotFoundException) { false }
    catch (_: Throwable) { false }

    @Before fun setup() { NativeTestBridge.uninstallAll() }
    @After fun cleanup() { NativeTestBridge.uninstallAll() }

    @Test
    fun dexLoaderCreateReturnsDexClassLoader() {
        val optDir = makeOptDir("create")
        val loader = DexLoader.create(targetCtx.applicationInfo.sourceDir, optDir, null)
        assertNotNull("DexLoader.create must return non-null", loader)
        assertTrue(
            "result must be DexClassLoader, got ${loader!!.javaClass.name}",
            loader is DexClassLoader)
    }

    @Test
    fun dexLoaderResolvesAppClass() {
        val optDir = makeOptDir("resolve")
        val loader = DexLoader.create(targetCtx.applicationInfo.sourceDir, optDir, null)
        assertNotNull("loader must be non-null", loader)
        assertTrue(
            "DexClassLoader must resolve $appClass",
            canLoad(loader!!, appClass))
    }

    @Test
    fun guestLoaderCannotResolveBeforeChain() {
        // guest = test APK only, boot parent (framework only). No app classes.
        val guest = PathClassLoader(testCtx.applicationInfo.sourceDir, null)
        assertTrue(
            "guest must NOT resolve $appClass (test APK + framework only)",
            !canLoad(guest, appClass))
    }

    @Test
    fun chainMakesAppClassVisibleToGuest() {
        val guest = PathClassLoader(testCtx.applicationInfo.sourceDir, null)
        assertTrue("guest can't find $appClass before", !canLoad(guest, appClass))

        val optDir = makeOptDir("chain")
        val dexLoader = DexLoader.create(targetCtx.applicationInfo.sourceDir, optDir, null)
        assertNotNull("dexLoader must be non-null", dexLoader)

        // CRITICAL: do NOT pre-load appClass via dexLoader before chaining.
        // ART binds (dex_file, class_def) to the DexClassLoader's defining
        // classloader the moment the class is loaded via that loader. Once
        // that binding exists, asking guest to define the same class from
        // the SAME dex (through the chained Element) fails with CNF — ART
        // refuses to redefine a class from a dex owned by a different
        // initiator. Chain first, load second → guest becomes the initiator.
        assertTrue("installFor", ClassLoaderBridge.installFor(guest, dexLoader!!))
        assertTrue("guest can find $appClass after chain", canLoad(guest, appClass))
    }

    @Test
    fun loadedClassInstantiable() {
        val optDir = makeOptDir("inst")
        val loader = DexLoader.create(targetCtx.applicationInfo.sourceDir, optDir, null)
        assertNotNull("loader", loader)

        val cls = try { loader!!.loadClass(appClass) } catch (_: Throwable) { null }
        assertNotNull("class must be loaded", cls)

        // ApplicationProxy has a public no-arg constructor (implicit from
        // Application). Its companion object has no eager native init, so
        // instantiation is safe.
        val instance = try {
            cls!!.getDeclaredConstructor().apply { isAccessible = true }.newInstance()
        } catch (_: Throwable) { null }
        assertNotNull("instance must be created", instance)
        assertTrue(
            "instance must be Application, got ${instance!!.javaClass.name}",
            instance is android.app.Application)
    }

    @Test
    fun independentGuestsStayIndependent() {
        // Two guests with identical construction — proving the chain does not
        // leak via static state or shared dex cache. Neither guest is
        // pre-loaded via the DexClassLoader, which is the key that makes the
        // chain's first-define work for guestA.
        val guestA = PathClassLoader(testCtx.applicationInfo.sourceDir, null)
        val guestB = PathClassLoader(testCtx.applicationInfo.sourceDir, null)
        assertTrue("A can't find before", !canLoad(guestA, appClass))
        assertTrue("B can't find before", !canLoad(guestB, appClass))

        val optDir = makeOptDir("indep")
        val dexLoader = DexLoader.create(targetCtx.applicationInfo.sourceDir, optDir, null)
        assertNotNull("dexLoader", dexLoader)
        assertTrue("chain into A", ClassLoaderBridge.installFor(guestA, dexLoader!!))

        assertTrue("A can find after", canLoad(guestA, appClass))
        assertTrue("B still can't find", !canLoad(guestB, appClass))
    }
}