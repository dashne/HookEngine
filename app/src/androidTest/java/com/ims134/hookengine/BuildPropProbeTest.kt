package com.ims134.hookengine

import android.content.pm.PackageManager
import android.os.Build
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.lang.reflect.Modifier

/**
 * Session 62 — Feature 5.13 (test-keys / custom build detect) probe.
 *
 * Diagnostic only. No assertions. Rule 12 does not apply to probes — the
 * purpose is to measure, not to gate. All output goes to logcat under
 * tag "BuildPropProbe".
 *
 * The five questions:
 *
 *   Q1. Property surface — are S59's kPropSpoofs live?
 *       ro.build.tags='release-keys', ro.build.type='user',
 *       ro.secure='1', plus boot-state properties.
 *
 *   Q2. Java static-final surface — Build.TAGS, Build.TYPE, etc.
 *       These are captured at zygote init from ro.build.* and are NOT
 *       affected by the property spoof. On this real Xiaomi device they
 *       should naturally read "release-keys" / "user". The probe
 *       cross-checks Build.TAGS against SystemProperties.get(ro.build.tags)
 *       to distinguish "both real" from "both spoofed-consistent" from
 *       "Java field disagrees with property" (which is the interesting
 *       case — would mean the Build.* surface was populated before our
 *       spoof landed).
 *
 *   Q3. Reflection write probe — is Build.TAGS writable via Field.set?
 *       Three possible outcomes:
 *         - field.set() throws (walled — Build.* page is read-only or
 *           hidden-api blocks it even via bypass) → Case C in the
 *           decision matrix, close on property surface only.
 *         - field.set() succeeds and Build.TAGS reads back the new value
 *           → Case B, add BuildSpoof.kt.
 *         - field.set() succeeds but Build.TAGS reads back unchanged
 *           (JIT-baked constant) → surprise case; document and assert
 *           the natural value.
 *
 *   Q4. Signature surface — is S18's PmSpoof signature null-out live?
 *       Detectors read PackageManager.getPackageInfo(own, GET_SIGNATURES)
 *       and GET_SIGNING_CERTIFICATES; if either still returns the real
 *       signature, that's a detector signal distinct from test-keys.
 *
 *   Q5. Boot state — ro.boot.verifiedbootstate, ro.boot.flash.locked.
 *       These are physical-device characteristics, not spoofable from
 *       user-space on a bootloader-unlocked device. Captured so the
 *       eventual BuildPropTest can be honest about what it can and
 *       cannot assert.
 *
 * Session 62 fix #1 — removed Build.FLAVOR from Q2. That field is
 * @SystemApi/hidden on API 37 and is not a public compile-time symbol,
 * so the direct Kotlin reference fails compilation even though the
 * reflection-bypass would have made it readable at runtime. The
 * property-surface measurement (Q1, ro.build.flavor) covers the same
 * value.
 *
 * @Before installFileHooks() for the same reason AdbDetectTest /
 * DebuggerDetectTest / EmulatorDetectTest use it — ConditionalHookTest
 * and AdbDetectTest precede this class alphabetically and both call
 * NativeTestBridge.uninstallAll() in their @After. Idempotent.
 */
@RunWith(AndroidJUnit4::class)
class BuildPropProbeTest {

    private val TAG = "BuildPropProbe"
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    private fun sysProp(key: String): String {
        return try {
            val spCls = Class.forName("android.os.SystemProperties")
            val getM = spCls.getMethod("get", String::class.java)
            (getM.invoke(null, key) as? String) ?: ""
        } catch (t: Throwable) {
            "<error: ${t.javaClass.simpleName}>"
        }
    }

    @Before
    fun reinstallHooks() {
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    private fun probePropertySurface() {
        Log.d(TAG, "=== Q1: property surface (S59 kPropSpoofs — should be live) ===")
        val keys = listOf(
            // Build-tag / test-keys indicators
            "ro.build.tags",
            "ro.build.type",
            "ro.secure",
            "ro.build.fingerprint",
            "ro.boot.verifiedbootstate",
            "ro.boot.flash.locked",
            "ro.boot.vbmeta.device_state",
            "ro.build.host",
            "ro.build.user",
            "ro.build.flavor",
            "ro.product.first_api_level",
            "ro.build.version.security_patch",
            // S59 spoof confirmations
            "ro.debuggable",
            "ro.adb.secure",
            "init.svc.adbd",
            "service.adb.root",
            "sys.oem_unlock_allowed"
        )
        for (k in keys) {
            Log.d(TAG, "  SystemProperties[$k] = '${sysProp(k)}'")
        }
    }

    private fun probeBuildStaticFinals() {
        Log.d(TAG, "=== Q2: Java Build.* static finals ===")
        Log.d(TAG, "  Build.TAGS         = '${Build.TAGS}'")
        Log.d(TAG, "  Build.TYPE         = '${Build.TYPE}'")
        Log.d(TAG, "  Build.FINGERPRINT  = '${Build.FINGERPRINT}'")
        Log.d(TAG, "  Build.HOST         = '${Build.HOST}'")
        Log.d(TAG, "  Build.USER         = '${Build.USER}'")
        Log.d(TAG, "  Build.ID           = '${Build.ID}'")
        Log.d(TAG, "  Build.DISPLAY      = '${Build.DISPLAY}'")
        Log.d(TAG, "  Build.MANUFACTURER = '${Build.MANUFACTURER}'")
        Log.d(TAG, "  Build.MODEL        = '${Build.MODEL}'")
        Log.d(TAG, "  Build.PRODUCT      = '${Build.PRODUCT}'")
        Log.d(TAG, "  Build.DEVICE       = '${Build.DEVICE}'")
        Log.d(TAG, "  Build.BOARD        = '${Build.BOARD}'")
        Log.d(TAG, "  Build.HARDWARE     = '${Build.HARDWARE}'")

        val propTags = sysProp("ro.build.tags")
        val propType = sysProp("ro.build.type")
        val propSec  = sysProp("ro.secure")
        Log.d(TAG, "  cross-check Build.TAGS == sysProp(ro.build.tags): ${Build.TAGS == propTags} " +
                "(prop='$propTags')")
        Log.d(TAG, "  cross-check Build.TYPE == sysProp(ro.build.type): ${Build.TYPE == propType} " +
                "(prop='$propType')")
        Log.d(TAG, "  cross-check Build.TAGS contains 'release-keys' : ${Build.TAGS.contains("release-keys")}")
        Log.d(TAG, "  cross-check Build.TAGS contains 'test-keys'    : ${Build.TAGS.contains("test-keys")}")
        Log.d(TAG, "  cross-check Build.TYPE == 'user'               : ${Build.TYPE == "user"}")
        Log.d(TAG, "  cross-check Build.TYPE == 'eng'                : ${Build.TYPE == "eng"}")
        Log.d(TAG, "  cross-check Build.TYPE == 'userdebug'          : ${Build.TYPE == "userdebug"}")
        Log.d(TAG, "  cross-check ro.secure == '1'                   : ${propSec == "1"}")
    }

    private fun probeReflectionWrite() {
        Log.d(TAG, "=== Q3: reflection write probe on Build.TAGS ===")

        // Step 1: find the field
        val field = try {
            HiddenApiBridge.reflectGetDeclaredField(Build::class.java, "TAGS")
        } catch (t: Throwable) {
            Log.d(TAG, "  reflectGetDeclaredField threw: ${t.javaClass.simpleName}: ${t.message}")
            null
        }
        if (field == null) {
            Log.d(TAG, "  TAGS field: NOT FOUND via HiddenApiBridge — probe aborted")
            return
        }
        Log.d(TAG, "  TAGS field: found")
        Log.d(TAG, "    type       = ${field.type.simpleName}")
        Log.d(TAG, "    modifiers  = '${Modifier.toString(field.modifiers)}'")
        Log.d(TAG, "    isFinal    = ${Modifier.isFinal(field.modifiers)}")
        Log.d(TAG, "    isStatic   = ${Modifier.isStatic(field.modifiers)}")

        // Step 2: setAccessible
        val accessible = try { HiddenApiBridge.reflectSetAccessible(field) }
        catch (t: Throwable) {
            Log.d(TAG, "  setAccessible threw: ${t.javaClass.simpleName}: ${t.message}")
            false
        }
        Log.d(TAG, "  setAccessible: $accessible")
        if (!accessible) {
            Log.d(TAG, "  → accessible refused — write probe aborted (wall on access)")
            return
        }

        // Step 3: read current, write sentinel, read back
        val before = try { field.get(null) as? String } catch (t: Throwable) {
            Log.d(TAG, "  field.get(before) threw: ${t.javaClass.simpleName}: ${t.message}")
            null
        }
        Log.d(TAG, "  before write: '$before'")

        val sentinel = "release-keys-probe-s62"
        try {
            field.set(null, sentinel)
            Log.d(TAG, "  field.set('$sentinel'): NO EXCEPTION")
        } catch (t: Throwable) {
            Log.d(TAG, "  field.set threw: ${t.javaClass.simpleName}: ${t.message}")
            // A thrown exception here is a genuine wall — document and
            // proceed to compare what Build.TAGS currently reads.
            val current = try { Build.TAGS } catch (_: Throwable) { "<threw>" }
            Log.d(TAG, "  → WRITE WALLED. Build.TAGS still reads '$current'")
            return
        }

        val after = try { Build.TAGS } catch (t: Throwable) {
            Log.d(TAG, "  Build.TAGS read threw after write: ${t.javaClass.simpleName}")
            "<threw>"
        }
        Log.d(TAG, "  after write: '$after'")
        Log.d(TAG, "  changed: ${before != after}")
        Log.d(TAG, "  now reads sentinel: ${after == sentinel}")

        // Step 4: restore (best-effort)
        try {
            field.set(null, before)
            val restored = try { Build.TAGS } catch (_: Throwable) { "<threw>" }
            Log.d(TAG, "  restored to '$before' — now reads: '$restored' (restored OK: ${restored == before})")
        } catch (t: Throwable) {
            Log.d(TAG, "  restore write threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    private fun probeSignatureSurface() {
        Log.d(TAG, "=== Q4: signature surface (S18 PmSpoof — should be nulled) ===")

        try {
            @Suppress("DEPRECATION")
            val pi = ctx.packageManager.getPackageInfo(
                ctx.packageName, PackageManager.GET_SIGNATURES)
            @Suppress("DEPRECATION")
            val sigs = pi.signatures
            Log.d(TAG, "  getPackageInfo(own, GET_SIGNATURES).signatures.size = " +
                    "${sigs?.size ?: "null"}")
            if (sigs != null && sigs.isNotEmpty()) {
                Log.d(TAG, "  first sig hashCode = ${sigs[0].hashCode()}")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  GET_SIGNATURES threw: ${t.javaClass.simpleName}: ${t.message}")
        }

        try {
            val pi = ctx.packageManager.getPackageInfo(
                ctx.packageName, PackageManager.GET_SIGNING_CERTIFICATES)
            val si = pi.signingInfo
            Log.d(TAG, "  getPackageInfo(own, GET_SIGNING_CERTIFICATES).signingInfo = ${si != null}")
            if (si != null) {
                val apkCerts = si.apkContentsSigners
                Log.d(TAG, "  signingInfo.apkContentsSigners.size = ${apkCerts?.size ?: "null"}")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  GET_SIGNING_CERTIFICATES threw: ${t.javaClass.simpleName}: ${t.message}")
        }

        try {
            val apk = File(ctx.filesDir, "viber.apk")
            if (apk.exists()) {
                @Suppress("DEPRECATION")
                val api = ctx.packageManager.getPackageArchiveInfo(
                    apk.absolutePath, PackageManager.GET_SIGNATURES)
                @Suppress("DEPRECATION")
                val archSigs = api?.signatures
                Log.d(TAG, "  getPackageArchiveInfo(viber, GET_SIGNATURES).signatures.size = " +
                        "${archSigs?.size ?: "null"}")
            } else {
                Log.d(TAG, "  (viber.apk not staged — archive sig probe skipped)")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  archive threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    private fun probeBootState() {
        Log.d(TAG, "=== Q5: boot state / verified boot / first-api ===")
        for (k in listOf(
            "ro.boot.verifiedbootstate",
            "ro.boot.flash.locked",
            "ro.boot.vbmeta.device_state",
            "ro.secure",
            "ro.boot.veritymode",
            "ro.boot.selinux",
            "ro.build.version.security_patch",
            "ro.product.first_api_level"
        )) {
            Log.d(TAG, "  SystemProperties[$k] = '${sysProp(k)}'")
        }
    }

    @Test
    fun probeBuildSurface() {
        Log.d(TAG, "########## S62 BuildPropProbeTest START ##########")
        probePropertySurface()
        probeBuildStaticFinals()
        probeReflectionWrite()
        probeSignatureSurface()
        probeBootState()
        Log.d(TAG, "########## S62 BuildPropProbeTest END ##########")
    }
}