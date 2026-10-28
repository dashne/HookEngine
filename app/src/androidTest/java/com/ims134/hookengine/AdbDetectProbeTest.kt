package com.ims134.hookengine

import android.provider.Settings
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class AdbDetectProbeTest {

    private val TAG = "AdbDetectProbe"
    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val cr get() = ctx.contentResolver

    @Before
    fun setup() {
        NativeTestBridge.uninstallAll()
        val ok = BinderTransactHook.install()
        Log.d(TAG, "BinderTransactHook.install = $ok (${BinderTransactHook.lastError})")
        assertTrue("hook install", ok)
        Thread.sleep(100)
        // Drain any residual codes so the per-call deltas are clean.
        BinderTransactHook.drainCodes()
        Log.d(TAG, "baseline fireCount=${BinderTransactHook.fireCount()}")
    }

    @After
    fun cleanup() {
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
        NativeTestBridge.uninstallAll()
    }

    private fun measureCall(label: String, block: () -> Any?) {
        val fireBefore = BinderTransactHook.fireCount()
        BinderTransactHook.drainCodes()   // clean slate
        val start = System.currentTimeMillis()
        val result = try { block() } catch (t: Throwable) { "<threw: ${t.javaClass.simpleName}: ${t.message}>" }
        val elapsed = System.currentTimeMillis() - start
        val fireAfter = BinderTransactHook.fireCount()
        val codes = BinderTransactHook.drainCodes()
        Log.d(TAG, "$label: result=$result elapsed=${elapsed}ms " +
                "fireDelta=${fireAfter - fireBefore} codes=${codes.toList()}")
    }

    @Test
    fun probeSettingsTransactsWithCodes() {
        Log.d(TAG, "=== start ===")

        measureCall("getInt(ADB_ENABLED)") {
            Settings.Global.getInt(cr, Settings.Global.ADB_ENABLED, -1)
        }
        Thread.sleep(50)

        measureCall("getString(ADB_ENABLED)") {
            Settings.Global.getString(cr, Settings.Global.ADB_ENABLED)
        }
        Thread.sleep(50)

        measureCall("getInt(DEVELOPMENT_SETTINGS_ENABLED)") {
            Settings.Global.getInt(cr, Settings.Global.DEVELOPMENT_SETTINGS_ENABLED, -1)
        }
        Thread.sleep(50)

        // Control: a read we know DOES go through binder on this ROM.
        // Settings.Secure reads are not in the redacted-value special path.
        measureCall("getString(android_id) [control]") {
            Settings.Secure.getString(cr, Settings.Secure.ANDROID_ID)
        }
        Thread.sleep(50)

        Log.d(TAG, "=== end ===")
    }

    @Test
    fun probeMValuesAtReadTime() {
        val globalCls = Class.forName("android.provider.Settings\$Global")
        val cacheField = HiddenApiBridge.reflectGetDeclaredField(globalCls, "sNameValueCache")!!
        HiddenApiBridge.reflectSetAccessible(cacheField)
        val cache = cacheField.get(null)!!

        val valuesField = HiddenApiBridge.reflectGetDeclaredField(cache.javaClass, "mValues")!!
        HiddenApiBridge.reflectSetAccessible(valuesField)
        val trackersField = HiddenApiBridge.reflectGetDeclaredField(cache.javaClass, "mGenerationTrackers")!!
        HiddenApiBridge.reflectSetAccessible(trackersField)
        val redactedField = HiddenApiBridge.reflectGetDeclaredField(cache.javaClass, "mReadableFieldsWithRedactedValue")!!
        HiddenApiBridge.reflectSetAccessible(redactedField)

        for (k in listOf("adb_enabled", "development_settings_enabled")) {
            @Suppress("UNCHECKED_CAST")
            val values = valuesField.get(cache) as? Map<String, String>
            @Suppress("UNCHECKED_CAST")
            val trackers = trackersField.get(cache) as? Map<String, Any>
            @Suppress("UNCHECKED_CAST")
            val redacted = redactedField.get(cache) as? Map<String, Any>

            Log.d(TAG, "pre-read['$k']: mValues=${values?.get(k)} " +
                    "hasTracker=${trackers?.containsKey(k)} " +
                    "redactedEntry=${redacted?.get(k)}")

            val live = Settings.Global.getInt(cr, k, -1)
            Log.d(TAG, "  getInt('$k') = $live")
        }
    }

    @Test
    fun probeRedactedTableIsLive() {
        val globalCls = Class.forName("android.provider.Settings\$Global")
        val cacheField = HiddenApiBridge.reflectGetDeclaredField(globalCls, "sNameValueCache")!!
        HiddenApiBridge.reflectSetAccessible(cacheField)
        val cache = cacheField.get(null)!!

        val redactedField = HiddenApiBridge.reflectGetDeclaredField(cache.javaClass, "mReadableFieldsWithRedactedValue")!!
        HiddenApiBridge.reflectSetAccessible(redactedField)

        @Suppress("UNCHECKED_CAST")
        val redacted = redactedField.get(cache) as MutableMap<String, Any>

        val key = "adb_enabled"
        val before = redacted[key]
        Log.d(TAG, "redacted['$key'] before = $before (${before?.javaClass?.simpleName})")

        // Sentinel distinguishable from both the real value (1) and our
        // target spoof (0) — if this doesn't move the read, this map is dead.
        val sentinel: Any = when (before) {
            is Int -> 777
            is Boolean -> before  // can't make a 3rd bool value; skip Int path if this hits
            is String -> "777"
            else -> 777
        }
        redacted[key] = sentinel
        Log.d(TAG, "redacted['$key'] set to $sentinel, reading back raw map: ${redacted[key]}")

        val live = Settings.Global.getInt(cr, key, -1)
        Log.d(TAG, "getInt('$key') after sentinel write = $live  (sentinel=$sentinel, expect match if this map is live)")
    }
}