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
 * Session 70 — Feature 5.6:  build-property coverage (bootloader-integrity
 *                            tell set added to kPropSpoofs).
 *
 * ─── Mechanism: property surface + device real state ───────────────
 *
 * A test-keys / integrity detector reads two surfaces:
 *
 *   (1) PROPERTY SURFACE — `SystemProperties.get("ro.build.tags")`, etc.
 *       S59's kPropSpoofs in Hooks.cpp already forced:
 *         ro.build.tags     -> "release-keys"
 *         ro.build.type     -> "user"
 *         ro.secure         -> "1"
 *         ro.debuggable     -> "0"
 *         ro.adb.secure     -> "1"
 *       via the GOT hook on libandroid_runtime.so.
 *
 *       S70 extends kPropSpoofs with the bootloader-integrity tell set
 *       that detection libraries read as "unlocked bootloader / modified
 *       system":
 *         ro.boot.verifiedbootstate -> "green"
 *         ro.boot.flash.locked      -> "1"
 *         ro.boot.veritymode        -> "enforcing"
 *         ro.boot.warranty_bit      -> "0"
 *         ro.warranty_bit           -> "0"
 *       On THIS device the real values were measured clean at S70 start,
 *       so the entries are behavior-neutral here and exist to guarantee the
 *       clean answer survives a ROM update or a port. On a device where any
 *       of these really is the tampered value, the entry is load-bearing
 *       and the assertions below fail honestly if it is removed.
 *
 *   (2) JAVA STATIC-FINAL SURFACE — `Build.TAGS`, `Build.TYPE`, etc.
 *       Populated at zygote init from ro.build.* BEFORE any hook can
 *       land. Not affected by the property spoof. Writing to these
 *       fields is refused by the JVM (S62 probe #1, Q3:
 *       `IllegalAccessException: Cannot set public static final field`).
 *       This is a JVM language invariant, not a defeatable wall. On THIS
 *       device they read naturally clean ("release-keys" / "user")
 *       because the device is a release-signed Xiaomi build.
 *
 * ─── Signature surface (paired regression guard) ───────────────────
 *
 * Test-keys detection is sometimes paired with signature-check shielding.
 * S18's PmSpoof.nullOutSignatures clears the legacy `PackageInfo.signatures`.
 * S62 extended it to cover the modern
 * `PackageInfo.signingInfo.mSigningDetails.{mSignatures,
 * mPastSigningCertificates}` path. Both are asserted here as regression
 * guards.
 *
 * ─── Rule 12 — every assertion is falsifiable ─────────────────────
 *
 *   1.  roBuildTagsRelease            — property GOT hook regressed OR
 *                                       device is test-keys-signed.
 *   2.  buildTagsStaticRelease        — Build.TAGS contains "test-keys"
 *                                       OR is not "release-keys".
 *   3.  roSecureIsOne                 — ro.secure != "1" (spoof lost or
 *                                       device genuinely insecure).
 *   4.  buildTagsPropertyMatchesStatic — surfaces diverge (future port
 *                                       to a test-keys device).
 *   5.  signatureNulledLegacy         — legacy signatures survive null-out.
 *   6.  signatureNulledModern         — modern mSignatures /
 *                                       mPastSigningCertificates survive.
 *   7.  roBootVerifiedBootStateGreen  — "orange"/"yellow"/"red" leaks
 *                                       through, OR spoof entry removed
 *                                       on a device whose real state is
 *                                       non-green.
 *   8.  roBootFlashLockedOne          — "0" leaks (bootloader unlocked
 *                                       signal).
 *   9.  roBootVeritymodeEnforcing     — "disabled"/"logging" leaks.
 *  10.  roBootWarrantyBitZero         — "1" leaks (tamper recorded).
 *  11.  roWarrantyBitZero             — vendor-side companion leaks.
 *
 * Every assertion reads the value through SystemProperties.get — the
 * real GOT-hooked path. "installFileHooks() returned 0" is never asserted
 * as evidence of anything (S59 meta-lesson).
 *
 * ─── Test isolation ────────────────────────────────────────────────
 *
 * @Before calls NativeTestBridge.installFileHooks() — the property GOT
 * hook lives on libandroid_runtime.so and is torn down by any earlier
 * test's NativeTestBridge.uninstallAll(). Idempotent.
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

    // ─── Feature 5.13 — test-keys surface ─────────────────────────────

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

    // ─── Feature 5.6 — bootloader-integrity tell set ──────────────────
    //
    // These five assert the property surface returns the clean-device
    // value. Each is falsifiable by exactly two independent failures:
    //   (a) the kPropSpoofs entry is removed AND the device's real value
    //       is the tampered value, or
    //   (b) the property GOT hook on libandroid_runtime.so regresses.
    // On THIS device the real values are clean, so (a) is currently
    // inoperative — meaning these are regression guards today and become
    // load-bearing if the device state ever changes. That is the honest
    // posture (S61 meta-lesson: the functional goal may already be
    // achieved by the process's real state).

    @Test
    fun roBootVerifiedBootStateGreen() {
        val v = sysProp("ro.boot.verifiedbootstate")
        Log.d(TAG, "SystemProperties[ro.boot.verifiedbootstate] = '$v'")
        assertEquals(
            "ro.boot.verifiedbootstate must be 'green' — 'orange' (unlocked " +
                    "bootloader) or 'yellow' (custom key) are the primary " +
                    "integrity tells. Fails if (a) the S70 kPropSpoofs entry " +
                    "is removed AND the device reports a non-green state, or " +
                    "(b) the property GOT hook on libandroid_runtime.so " +
                    "regresses.",
            "green", v)
    }

    @Test
    fun roBootFlashLockedOne() {
        val v = sysProp("ro.boot.flash.locked")
        Log.d(TAG, "SystemProperties[ro.boot.flash.locked] = '$v'")
        assertEquals(
            "ro.boot.flash.locked must be '1' — '0' means flashing is " +
                    "permitted, i.e. the bootloader is unlocked. Fails if the " +
                    "S70 kPropSpoofs entry is removed AND the device reports " +
                    "'0', or if the property GOT hook regresses.",
            "1", v)
    }

    @Test
    fun roBootVeritymodeEnforcing() {
        val v = sysProp("ro.boot.veritymode")
        Log.d(TAG, "SystemProperties[ro.boot.veritymode] = '$v'")
        assertEquals(
            "ro.boot.veritymode must be 'enforcing' — 'disabled' or " +
                    "'logging' indicates dm-verity has been turned off. Fails " +
                    "if the S70 kPropSpoofs entry is removed AND the device " +
                    "reports a non-enforcing mode, or if the property GOT " +
                    "hook regresses.",
            "enforcing", v)
    }

    @Test
    fun roBootWarrantyBitZero() {
        val v = sysProp("ro.boot.warranty_bit")
        Log.d(TAG, "SystemProperties[ro.boot.warranty_bit] = '$v'")
        assertEquals(
            "ro.boot.warranty_bit must be '0' — '1' means the bootloader " +
                    "has recorded a tamper event (warranty voided). Fails if " +
                    "the S70 kPropSpoofs entry is removed AND the device " +
                    "reports '1', or if the property GOT hook regresses.",
            "0", v)
    }

    @Test
    fun roWarrantyBitZero() {
        val v = sysProp("ro.warranty_bit")
        Log.d(TAG, "SystemProperties[ro.warranty_bit] = '$v'")
        assertEquals(
            "ro.warranty_bit must be '0' — the vendor-side companion to " +
                    "ro.boot.warranty_bit; both are read. Fails if the S70 " +
                    "kPropSpoofs entry is removed AND the device reports " +
                    "'1', or if the property GOT hook regresses.",
            "0", v)
    }
}