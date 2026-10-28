package com.ims134.hookengine

import android.hardware.Sensor
import android.hardware.SensorManager
import android.os.Build
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Session 61 — Feature 5.12 emulator detect blocking probe.
 *
 * Diagnostic only. No assertions. Rule 12 does not apply to probes.
 * All output goes to logcat under tag "EmuDetectProbe".
 *
 * The five questions:
 *
 *   Q1. Property surface — what does the framework return for every
 *       property an anti-emulator library reads? Some will be denied
 *       (return ""), some will be real (return the physical device's
 *       actual value), some may already be spoofed by kPropSpoofs
 *       (ro.kernel.qemu, ro.boot.qemu, ro.build.tags, ro.build.type).
 *
 *   Q2. File existence — do any emulator-specific files exist?
 *       Should not on physical hardware, but MIUI anti-tamper
 *       sometimes leaves decoy files or removes them. Probe, don't
 *       assume.
 *
 *   Q3. Java Build.* fields — what does a detector calling
 *       Build.FINGERPRINT, Build.MODEL, etc. see? These are the
 *       Java-side view of the property surface (Android populates
 *       Build.* from ro.build.* at zygote start), and some
 *       detectors use the Java values instead of reflection.
 *
 *   Q4. Sensor surface — some emulators expose named "Goldfish
 *       *" sensors. Physical devices expose real sensor names.
 *
 *   Q5. Loaded libraries — the process should not have any goldfish
 *       or qemu libraries mmap'd. If it does, it's either an
 *       emulator or a hostile ROM.
 *
 * The output of this probe determines which surface 5.12's test
 * needs to explicitly guard. If all five are clean (expected on
 * physical MediaTek MIUI), the test just asserts the clean state
 * as a regression guard. If any is leaking, that specific surface
 * needs a specific mechanism.
 */
@RunWith(AndroidJUnit4::class)
class EmulatorDetectProbeTest {

    private val TAG = "EmuDetectProbe"
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

    private fun probeProperties() {
        Log.d(TAG, "=== Q1: emulator-relevant property surface ===")
        val keys = listOf(
            // QEMU / goldfish identification
            "ro.kernel.qemu",
            "ro.boot.qemu",
            "ro.boot.qemu.avd_name",
            "ro.boot.qemu.gltransport",
            "ro.boot.qemu.gltransport.name",
            "ro.boot.qemu.hw.mainkeys",
            "ro.hardware",
            "ro.boot.hardware",
            "ro.hardware.chipname",
            // Build identity
            "ro.product.model",
            "ro.product.manufacturer",
            "ro.product.brand",
            "ro.product.device",
            "ro.product.name",
            "ro.product.board",
            "ro.build.fingerprint",
            "ro.build.flavor",
            "ro.build.product",
            "ro.build.tags",
            "ro.build.type",
            // Emulator indicator: "sdk" build flavor
            "ro.build.version.sdk",
            // Build tag indicators (already in kPropSpoofs)
            "ro.secure",
            "ro.debuggable",
            // Debug-related (should be clean for 5.11)
            "init.svc.adbd",
            "service.adb.root"
        )
        for (k in keys) {
            Log.d(TAG, "  SystemProperties[$k] = '${sysProp(k)}'")
        }
    }

    private fun probeFiles() {
        Log.d(TAG, "=== Q2: emulator-specific file existence ===")
        val paths = listOf(
            "/dev/qemu_pipe",
            "/dev/goldfish_pipe",
            "/dev/goldfish_sync",
            "/dev/socket/qemud",
            "/dev/socket/genyd",
            "/dev/socket/baseband_genyd",
            "/system/lib64/libgoldfish_audio.so",
            "/system/lib64/libgoldfish_pipe.so",
            "/system/lib64/libgoldfish_sync.so",
            "/system/lib64/hw/gralloc.goldfish.so",
            "/system/lib64/hw/sensors.goldfish.so",
            "/system/lib64/hw/audio.primary.goldfish.so",
            "/system/bin/qemu-props",
            "/system/bin/qemud",
            "/init.goldfish.rc"
        )
        for (p in paths) {
            val exists = File(p).exists()
            Log.d(TAG, "  File($p).exists() = $exists")
        }
    }

    private fun probeBuildFields() {
        Log.d(TAG, "=== Q3: Java Build.* fields ===")
        Log.d(TAG, "  Build.FINGERPRINT   = '${Build.FINGERPRINT}'")
        Log.d(TAG, "  Build.MODEL         = '${Build.MODEL}'")
        Log.d(TAG, "  Build.MANUFACTURER  = '${Build.MANUFACTURER}'")
        Log.d(TAG, "  Build.BRAND         = '${Build.BRAND}'")
        Log.d(TAG, "  Build.DEVICE        = '${Build.DEVICE}'")
        Log.d(TAG, "  Build.HARDWARE      = '${Build.HARDWARE}'")
        Log.d(TAG, "  Build.PRODUCT       = '${Build.PRODUCT}'")
        Log.d(TAG, "  Build.TAGS          = '${Build.TAGS}'")
        Log.d(TAG, "  Build.TYPE          = '${Build.TYPE}'")
        Log.d(TAG, "  Build.BOARD         = '${Build.BOARD}'")
        Log.d(TAG, "  Build.ID            = '${Build.ID}'")
        Log.d(TAG, "  Build.DISPLAY       = '${Build.DISPLAY}'")
        Log.d(TAG, "  Build.HOST          = '${Build.HOST}'")
        Log.d(TAG, "  Build.USER          = '${Build.USER}'")
    }

    private fun probeSensors() {
        Log.d(TAG, "=== Q4: sensor list (looking for goldfish/qemu names) ===")
        try {
            val sm = ctx.getSystemService(SensorManager::class.java)
            if (sm == null) {
                Log.d(TAG, "  SensorManager null")
                return
            }
            val sensors = sm.getSensorList(Sensor.TYPE_ALL) ?: emptyList()
            Log.d(TAG, "  total sensors = ${sensors.size}")
            var suspicious = 0
            for (s in sensors) {
                val n = s.name ?: ""
                val v = s.vendor ?: ""
                val lowerN = n.lowercase()
                val lowerV = v.lowercase()
                val isSuspicious = lowerN.contains("goldfish") ||
                        lowerN.contains("qemu") ||
                        lowerN.contains("emulator") ||
                        lowerN.contains("android sdk") ||
                        lowerV.contains("goldfish") ||
                        lowerV.contains("qemu") ||
                        lowerV.contains("android sdk")
                if (isSuspicious) {
                    Log.d(TAG, "  ⚠️ SUSPICIOUS: '$n' vendor='$v' type=${s.type}")
                    suspicious++
                }
            }
            Log.d(TAG, "  suspicious sensor count = $suspicious")
            // Print first 5 names as sample for confirmation of naming style
            sensors.take(5).forEachIndexed { i, s ->
                Log.d(TAG, "  sample[$i]: name='${s.name}' vendor='${s.vendor}' type=${s.type}")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  SensorManager probe threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    private fun probeLoadedLibs() {
        Log.d(TAG, "=== Q5: loaded libraries (/proc/self/maps) ===")
        try {
            val maps = File("/proc/self/maps").readLines()
            var suspicious = 0
            val suspiciousPaths = mutableSetOf<String>()
            for (line in maps) {
                val lower = line.lowercase()
                if (lower.contains("goldfish") || lower.contains("qemu")) {
                    suspicious++
                    // Extract path (last space-delimited token)
                    val path = line.substringAfterLast(' ').trim()
                    if (path.isNotEmpty()) suspiciousPaths.add(path)
                }
            }
            Log.d(TAG, "  maps total lines = ${maps.size}")
            Log.d(TAG, "  lines containing goldfish/qemu = $suspicious")
            for (p in suspiciousPaths) {
                Log.d(TAG, "  ⚠️ SUSPICIOUS path: $p")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  /proc/self/maps read threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    @Test
    fun probeEmulatorSurface() {
        Log.d(TAG, "########## S61 EmulatorDetectProbeTest START ##########")
        probeProperties()
        probeFiles()
        probeBuildFields()
        probeSensors()
        probeLoadedLibs()
        Log.d(TAG, "########## S61 EmulatorDetectProbeTest END ##########")
    }
}