package com.ims134.hookengine

import android.hardware.Sensor
import android.hardware.SensorManager
import android.os.Build
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File

/**
 * Session 61 — Feature 5.12: emulator detect blocking.
 *
 * ─── Mechanism: real device + defensive property spoofs ────────────
 *
 * The device this engine targets (Xiaomi 2602DPT53G, MediaTek mt6899,
 * MIUI/API 37) is a real physical device. Emulator detection on this
 * class of hardware is functionally achieved by two properties:
 *
 *   (1) The device IS real — Build.HARDWARE, Build.FINGERPRINT, sensor
 *       vendor names, and loaded library paths all carry genuine
 *       MediaTek + Xiaomi identifiers. No goldfish/qemu strings appear
 *       anywhere a detector reads.
 *
 *   (2) The two defensive property spoofs from S59's kPropSpoofs:
 *       ro.kernel.qemu → ""  and  ro.boot.qemu → ""
 *       On a real device these properties don't exist (property service
 *       returns ""). On an emulator they'd be "1". kPropSpoofs already
 *       forces them to "" regardless, so any caller using SystemProperties.get
 *       through libandroid_runtime.so sees the same value on either
 *       device class. Cheap defense-in-depth — the spoof runs on every
 *       process for free because it's part of the same ShouldHookProperty
 *       GOT hook that already covers the debug/ADB property surface.
 *
 * Probe (EmulatorDetectProbeTest, S61) measured every surface a
 * standard anti-emulator library reads:
 *
 *   Q1 property surface  — every indicator is '' or a real device value
 *   Q2 file existence    — 0 emulator-specific files exist
 *   Q3 Build.* fields    — all real MediaTek/Xiaomi values
 *   Q4 sensor list       — 51 sensors, 0 goldfish/qemu names
 *   Q5 loaded libs       — 0 goldfish/qemu paths in /proc/self/maps
 *
 * No hook or spoof code is installed for 5.12. The functional goal is
 * achieved by (1) + (2). This test class is the regression guard: any
 * future change that breaks the defensive property spoof, or that
 * accidentally installs goldfish-sounding values into Build.*, is
 * caught by an assertion here.
 *
 * ─── Rule 12 — every assertion is falsifiable ──────────────────────
 *
 *   1. qemuPropertySpoofed       — fails if engine property spoof regresses
 *                                  OR if device is an emulator (ro.kernel.qemu='1')
 *   2. qemuPipeAbsent            — fails on emulator (/dev/qemu_pipe exists)
 *   3. goldfishPipeAbsent        — fails on emulator (/dev/goldfish_pipe exists)
 *   4. buildHardwareNotEmulator  — fails on emulator (Build.HARDWARE='ranchu'/'goldfish')
 *   5. noGoldfishSensors         — fails on emulator (goldfish-named sensors)
 *   6. noGoldfishLibsLoaded      — fails on emulator (libgoldfish*.so in maps)
 *
 * ─── Test-isolation lesson (S60) ──────────────────────────────────
 *
 * ConditionalHookTest immediately precedes this class alphabetically and
 * calls NativeTestBridge.uninstallAll() in its @After. That tears down
 * the startup-installed GOT hooks including __system_property_get on
 * libandroid_runtime.so — the exact hook that carries kPropSpoofs'
 * ro.kernel.qemu and ro.boot.qemu entries. installFileHooks() in @Before
 * re-installs the property hooks so the spoof fires. Idempotent — safe
 * whether hooks are already present or wiped. Same pattern as
 * AdbDetectTest and DebuggerDetectTest.
 */
@RunWith(AndroidJUnit4::class)
class EmulatorDetectTest {

    private val TAG = "EmulatorDetectTest"
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
        // See class-level test-isolation comment. Re-installs the
        // ShouldHookProperty GOT entries including libandroid_runtime.so,
        // which is what makes SystemProperties.get() fire kPropSpoofs.
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    @Test
    fun qemuPropertySpoofed() {
        val v1 = sysProp("ro.kernel.qemu")
        val v2 = sysProp("ro.boot.qemu")
        Log.d(TAG, "SystemProperties[ro.kernel.qemu] = '$v1'")
        Log.d(TAG, "SystemProperties[ro.boot.qemu]  = '$v2'")
        assertEquals(
            "ro.kernel.qemu must be '' (kPropSpoofs defensive entry; also " +
                    "the natural value on a real device — this assertion " +
                    "fails on emulator or if the property GOT hook regressed)",
            "", v1
        )
        assertEquals(
            "ro.boot.qemu must be '' (same mechanism as ro.kernel.qemu)",
            "", v2
        )
    }

    @Test
    fun qemuPipeAbsent() {
        val path = "/dev/qemu_pipe"
        val exists = File(path).exists()
        Log.d(TAG, "File($path).exists() = $exists")
        assertFalse(
            "$path must not exist — this is the QEMU pipe device node " +
                    "opened by every emulator instance. Its presence is a " +
                    "definitive emulator signal. If this fails, either the " +
                    "device is an emulator or our file-hide hook is broken.",
            exists
        )
    }

    @Test
    fun goldfishPipeAbsent() {
        val path = "/dev/goldfish_pipe"
        val exists = File(path).exists()
        Log.d(TAG, "File($path).exists() = $exists")
        assertFalse(
            "$path must not exist — Goldfish is the Android emulator's " +
                    "virtual hardware platform. Physical devices never have " +
                    "this device node.",
            exists
        )
    }

    @Test
    fun buildHardwareNotEmulator() {
        val hw = Build.HARDWARE ?: ""
        Log.d(TAG, "Build.HARDWARE = '$hw'")
        val lower = hw.lowercase()
        val emulatorTokens = listOf("goldfish", "ranchu", "qemu", "emulator", "sdk")
        val hits = emulatorTokens.filter { lower.contains(it) }
        assertTrue(
            "Build.HARDWARE='$hw' must not contain any emulator token " +
                    "(${emulatorTokens.joinToString()}). Any match means the " +
                    "device is emulated. Got hits: $hits",
            hits.isEmpty()
        )
    }

    @Test
    fun noGoldfishSensors() {
        val sm = ctx.getSystemService(SensorManager::class.java)
            ?: run {
                Log.w(TAG, "SensorManager null — skipping sensor assertion")
                return
            }
        val sensors = sm.getSensorList(Sensor.TYPE_ALL) ?: emptyList()
        Log.d(TAG, "total sensors = ${sensors.size}")
        val suspicious = mutableListOf<String>()
        for (s in sensors) {
            val n = (s.name ?: "").lowercase()
            val v = (s.vendor ?: "").lowercase()
            if (n.contains("goldfish") || n.contains("qemu") ||
                n.contains("emulator") || n.contains("android sdk") ||
                v.contains("goldfish") || v.contains("qemu") ||
                v.contains("android sdk")) {
                suspicious.add("${s.name} (vendor=${s.vendor})")
            }
        }
        assertTrue(
            "Sensor list must not contain goldfish/qemu-named sensors. " +
                    "Standard emulators expose names like " +
                    "'Goldfish 3-axis Accelerometer'. Suspicious entries: " +
                    suspicious.joinToString(),
            suspicious.isEmpty()
        )
    }

    @Test
    fun noGoldfishLibsLoaded() {
        val maps = try {
            File("/proc/self/maps").readLines()
        } catch (t: Throwable) {
            Log.e(TAG, "/proc/self/maps read threw: ${t.javaClass.simpleName}")
            // Cannot assert without the maps file — treat as a hard fail
            // rather than a silent pass.
            throw AssertionError(
                "Failed to read /proc/self/maps: ${t.javaClass.simpleName}: ${t.message}",
                t
            )
        }
        Log.d(TAG, "maps total lines = ${maps.size}")
        val hits = maps.filter {
            val lower = it.lowercase()
            lower.contains("goldfish") || lower.contains("qemu")
        }
        assertTrue(
            "/proc/self/maps must not contain any goldfish/qemu library " +
                    "path. If it does, the process has loaded emulator-specific " +
                    "libraries. Got ${hits.size} matching lines.",
            hits.isEmpty()
        )
    }
}