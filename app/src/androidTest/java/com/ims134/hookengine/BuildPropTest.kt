package com.ims134.hookengine

import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 62 — Feature 5.13: test-keys / custom-build detect blocking.
 *
 * ─── Mechanism: two surfaces, four assertions on detect + two on sig ──
 *
 * A test-keys detector reads two surfaces:
 *
 *   (1) PROPERTY SURFACE — `SystemProperties.get("ro.build.tags")` etc.
 *       S59's kPropSpoofs in Hooks.cpp already forces:
 *         ro.build.tags     -> "release-keys"
 *         ro.build.type     -> "user"
 *         ro.secure         -> "1"
 *         ro.debuggable     -> "0"
 *         ro.adb.secure     -> "1"
 *       via the GOT hook on libandroid_runtime.so. Verified live by
 *       BuildPropProbeTest Q1 (S62 probes #1–#4).
 *
 *   (2) JAVA STATIC-FINAL SURFACE — `Build.TAGS`, `Build.TYPE`, etc.
 *       Populated at zygote init from ro.build.* BEFORE any hook can
 *       land. Not affected by the property spoof. Writing to these
 *       fields is refused by the JVM (S62 probe #1, Q3:
 *       `IllegalAccessException: Cannot set public static final field`)
 *       because a `public static final String` with a constant
 *       initializer is compile-time constant-folded into bytecode.
 *       This is a JVM language invariant, not a defeatable wall, so
 *       no BuildSpoof.kt is written. On THIS device they read
 *       naturally clean ("release-keys" / "user") because the device
 *       is a release-signed Xiaomi build.
 *
 * The functional goal — a detector reading test-keys indicators sees
 * only release-keys values — is achieved by the property surface plus
 * the device's real state.
 *
 * ─── Signature surface (paired regression guard) ──────────────────
 *
 * Test-keys detection is sometimes paired with signature-check
 * shielding (a detector verifies the APK's signing certificate to
 * confirm test-keys signing, or the reverse — hiding that a real
 * signature is present). S18's PmSpoof.nullOutSignatures clears the
 * legacy `PackageInfo.signatures`. S62 extended it to cover the modern
 * `PackageInfo.signingInfo.mSigningDetails.{mSignatures,
 * mPastSigningCertificates}` path — MIUI/API 37 renamed AOSP's
 * `signatures` field AND added a second Signature[] field, AND the
 * whole class was `api=blocked` for reflection. The S62 fix uses the
 * HiddenApiBridge bypass to enumerate SigningDetails.declaredFields
 * (which a caller-side read filters to an empty array) and nulls
 * every non-static Signature[] found. See PROJECT_STATE §5.13 for the
 * four-wall iteration history.
 *
 * Both signature paths are asserted here as regression guards — they
 * don't gate 5.13 per se, but a PmSpoof regression would re-open a
 * distinct signature-leak surface that a detector could use as
 * corroborating evidence.
 *
 * ─── Rule 12 — every assertion is falsifiable ─────────────────────
 *
 *   1. roBuildTagsRelease           — fails if property GOT hook
 *                                     regresses, OR device is test-keys-
 *                                     signed, OR S59 kPropSpoofs entry
 *                                     removed.
 *   2. buildTagsStaticRelease       — fails if Build.TAGS contains
 *                                     "test-keys" (device-side or
 *                                     corruption) OR is not
 *                                     "release-keys".
 *   3. roSecureIsOne                — fails on a production build with
 *                                     ro.secure != "1" OR if S59
 *                                     kPropSpoofs loses ro.secure.
 *   4. buildTagsPropertyMatchesStatic — cross-check; fails if the two
 *                                     surfaces diverge (would reveal a
 *                                     future port to a test-keys device
 *                                     where the spoof returns
 *                                     "release-keys" but Build.TAGS
 *                                     still says "test-keys"). This is
 *                                     the honest-position guard.
 *   5. signatureNulledLegacy        — fails if PmSpoof proxy install
 *                                     fails OR the legacy null-out
 *                                     path regresses.
 *   6. signatureNulledModern        — fails if S62's bypass-based
 *                                     mSignatures/mPastSigningCertificates
 *                                     null-out regresses (the modern-
 *                                     signature regression guard).
 *
 * ─── Test isolation ──────────────────────────────────────────────
 *
 * @Before calls NativeTestBridge.installFileHooks() — the property GOT
 * hook lives on libandroid_runtime.so and is torn down by any earlier
 * test's NativeTestBridge.uninstallAll() (ConditionalHookTest and
 * AdbDetectTest precede this class alphabetically and both call it in
 * @After). Idempotent — returns 0 whether hooks are already present or
 * need to be re-installed. Same pattern as AdbDetectTest,
 * DebuggerDetectTest, EmulatorDetectTest.
 */
@RunWith(AndroidJUnit4::class)
class BuildPropTest {

    private val TAG = "BuildPropTest"
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun sysProp(key: String): String {
        return try {
            val spCls = Class.forName("android.os.SystemProperties")
            val getM = spCls.getMethod("get", String::class.java)
            (getM.invoke(null, key) as? String) ?: ""
        } catch (t: Throwable) {
            Log.e(TAG, "sysProp('$key') threw: ${t.javaClass.simpleName}: ${t.message}")
            "<error>"
        }
    }

    @Before
    fun reinstallHooks() {
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    @Test
    fun roBuildTagsRelease() {
        val v = sysProp("ro.build.tags")
        Log.d(TAG, "SystemProperties[ro.build.tags] = '$v'")
        assertEquals(
            "ro.build.tags must be 'release-keys' — S59 kPropSpoofs forces " +
                    "this on the property surface; a test-keys device would " +
                    "naturally return 'test-keys'. Fails if (a) property GOT " +
                    "hook regresses, (b) device is test-keys-signed, or " +
                    "(c) kPropSpoofs entry removed.",
            "release-keys", v)
    }

    @Test
    fun buildTagsStaticRelease() {
        val v = Build.TAGS ?: ""
        Log.d(TAG, "Build.TAGS = '$v'")
        assertFalse(
            "Build.TAGS must NOT contain 'test-keys' — that token is the " +
                    "single most common test-keys detector signal. The field " +
                    "is populated at zygote init from ro.build.tags and " +
                    "refuses reflection writes (public static final + " +
                    "constant init → JVM IllegalAccessException), so on a " +
                    "test-keys-signed device this assertion fails honestly.",
            v.contains("test-keys"))
        assertEquals(
            "Build.TAGS must be 'release-keys' — release-signed devices " +
                    "naturally report this; a corrupted Build.* field would " +
                    "trip this assertion.",
            "release-keys", v)
    }

    @Test
    fun roSecureIsOne() {
        val v = sysProp("ro.secure")
        Log.d(TAG, "SystemProperties[ro.secure] = '$v'")
        assertEquals(
            "ro.secure must be '1' — a debuggable / insecure build " +
                    "(ro.secure='0') is a test-keys-class detector signal. " +
                    "S59 kPropSpoofs forces this on the property surface.",
            "1", v)
    }

    @Test
    fun buildTagsPropertyMatchesStatic() {
        val prop = sysProp("ro.build.tags")
        val stat = Build.TAGS ?: ""
        Log.d(TAG, "property='$prop' static='$stat' match=${prop == stat}")
        assertEquals(
            "Property surface and static-final surface must agree — on " +
                    "THIS device they match because it's a release-keys build. " +
                    "If a future port targets a test-keys device, the property " +
                    "spoof would return 'release-keys' while Build.TAGS stays " +
                    "frozen at 'test-keys'; this assertion fails honestly, " +
                    "flagging that the Java surface is NOT defended on that " +
                    "device and BuildSpoof/Phase-F work is required.",
            prop, stat)
    }

    @Test
    fun signatureNulledLegacy() {
        @Suppress("DEPRECATION")
        val pi = ctx.packageManager.getPackageInfo(
            ctx.packageName, PackageManager.GET_SIGNATURES)
        @Suppress("DEPRECATION")
        val sigs = pi.signatures
        Log.d(TAG, "getPackageInfo(own, GET_SIGNATURES).signatures.size = " +
                "${sigs?.size ?: "null"}")
        assertTrue(
            "signatures must be null or empty — S18 PmSpoof.nullOutSignatures " +
                    "clears this via ParcelRewriter + direct Field.set (legacy " +
                    "`signatures` is api=unsupported, so direct reflection " +
                    "works). Fails if PmSpoof proxy install regresses.",
            sigs == null || sigs.isEmpty())
    }

    @Test
    fun signatureNulledModern() {
        val pi = ctx.packageManager.getPackageInfo(
            ctx.packageName, PackageManager.GET_SIGNING_CERTIFICATES)
        val si = pi.signingInfo
        assertNotNull(
            "signingInfo must be non-null when GET_SIGNING_CERTIFICATES " +
                    "is requested — a null here would mean the framework " +
                    "itself has regressed.",
            si)
        val apkCerts = si!!.apkContentsSigners
        Log.d(TAG, "signingInfo.apkContentsSigners.size = ${apkCerts?.size ?: "null"}")
        assertTrue(
            "apkContentsSigners must be null or empty — S62 extended " +
                    "PmSpoof.nullOutSignatures to null EVERY non-static " +
                    "Signature[] field on SigningDetails (MIUI/API 37 renamed " +
                    "AOSP's `signatures` to `mSignatures` and added " +
                    "`mPastSigningCertificates`; the S62 bypass enumeration " +
                    "handles both, and any future MIUI rename). Fails if the " +
                    "bypass route regresses OR a future ROM adds a Signature[] " +
                    "field that survives null-out.",
            apkCerts == null || apkCerts.isEmpty())
    }
}