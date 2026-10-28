package com.ims134.hookengine

import android.content.Context
import android.provider.Settings
import android.util.Log
import java.lang.reflect.Field

/**
 * Session 59 — Feature 5.10 ADB detect blocking (PARTIAL).
 *
 * ─── Property surface — COMPLETE ────────────────────────────────────
 *
 * The property-based ADB detection surface is fully blocked:
 *   ro.debuggable    → "0"        (Hooks.cpp kPropSpoofs)
 *   init.svc.adbd    → "stopped"  (Hooks.cpp kPropSpoofs)
 *   ro.adb.secure    → "1"        (Hooks.cpp kPropSpoofs, S59 addition)
 *   service.adb.root → ""         (Hooks.cpp kPropSpoofs)
 *
 * libandroid_runtime.so added to ShouldHookProperty so reflection-based
 * SystemProperties.get fires 5.6's spoof. Verified by
 * AdbDetectTest.roDebuggableSpoofed / initSvcAdbdSpoofed.
 *
 * ─── Settings.Global surface — ARCHITECTURALLY WALLED ──────────────
 *
 * Settings.Global.getInt/getString for ADB_ENABLED and
 * DEVELOPMENT_SETTINGS_ENABLED cannot be spoofed by any mechanism a
 * user-space engine can install at attachBaseContext time. Four
 * hypotheses measured, four refuted on-device:
 *
 *   1. NameValueCache.mValues cache poison — write persists forever
 *      (pre-write log always shows "0"), framework read bypasses it
 *      (getString returns "1"). Real defect: writes land, reads don't
 *      consult the map for these keys.
 *
 *   2. mGenerationTrackers sync (AOSP 34+ per-key tracker) — field
 *      exists but stays ArrayMap size=0. Not the invalidation scheme
 *      on this ROM.
 *
 *   3. mReadableFieldsWithRedactedValue hypothesis — these two keys
 *      ARE the only entries in the map, but the redacted "0" is a
 *      fallback for *unprivileged* callers; we're privileged, so the
 *      framework returns the real value "1", not the redacted "0".
 *
 *   4. IContentProvider.call() via IPCThreadState::transact — S59
 *      probe fired zero transacts for the three ADB/DEV reads
 *      (fireDelta=0, codes=[], elapsed=0ms) while a control
 *      Settings.Secure read fired code 21 with full Bundle reply.
 *      Definitive: the reads never leave the process.
 *
 * **What the CP reply reveals (for future Phase F):** MIUI uses
 * shared-memory generation tracking on cache miss (Bundle reply
 * carries `_track_generation` marker + `android.util.MemoryIntArray` +
 * `android.os.ParcelFileDescriptor` + `value`). The shared-mem region
 * for `adb_enabled` was populated at boot, before hook install. No
 * intercept point exists after boot for a user-space engine.
 *
 * **Correct Phase F entry:** disassemble
 * framework.jar!classes3.dex's Settings$NameValueCache.getStringForUser
 * on this ROM and trace the shared-memory read path. IDA/Ghidra work,
 * not an engine-primitive session.
 *
 * ─── Current state ──────────────────────────────────────────────────
 *
 * AdbSpoof.poison() remains on disk as a no-op-cost mValues writer.
 * The mValues writes never affect framework reads for these keys, but
 * the diagnostic (post-poison getString/getInt log) is useful if a
 * future session finds a way to hook the shared-mem read.
 */
object AdbSpoof {
    private const val TAG = "DualCore"

    private val POISON: Map<String, String> = mapOf(
        "adb_enabled" to "0",
        "development_settings_enabled" to "0",
    )

    @Volatile private var sCacheInstance: Any? = null
    @Volatile private var mValuesField: Field? = null
    @Volatile private var appCtx: Context? = null
    @Volatile private var installed = false

    @Synchronized
    fun install(ctx: Context): Boolean {
        if (installed) return poison()

        appCtx = ctx.applicationContext ?: ctx

        try {
            val globalCls = Class.forName("android.provider.Settings\$Global")

            val cacheField = HiddenApiBridge.reflectGetDeclaredField(globalCls, "sNameValueCache")
                ?: run {
                    Log.e(TAG, "AdbSpoof: sNameValueCache field missing")
                    return false
                }
            if (!HiddenApiBridge.reflectSetAccessible(cacheField)) {
                Log.e(TAG, "AdbSpoof: setAccessible(sNameValueCache) failed")
                return false
            }
            val cacheInstance = try { cacheField.get(null) }
            catch (t: Throwable) {
                Log.e(TAG, "AdbSpoof: read sNameValueCache threw: ${t.javaClass.simpleName}: ${t.message}")
                null
            } ?: run {
                Log.e(TAG, "AdbSpoof: sNameValueCache was null")
                return false
            }

            val valuesField = HiddenApiBridge.reflectGetDeclaredField(cacheInstance.javaClass, "mValues")
                ?: run {
                    Log.e(TAG, "AdbSpoof: mValues field missing on ${cacheInstance.javaClass.name}")
                    return false
                }
            if (!HiddenApiBridge.reflectSetAccessible(valuesField)) {
                Log.e(TAG, "AdbSpoof: setAccessible(mValues) failed")
                return false
            }

            dumpFieldSummary(cacheInstance)

            sCacheInstance = cacheInstance
            mValuesField = valuesField
            installed = true
            Log.d(TAG, "AdbSpoof: resolved sNameValueCache=${cacheInstance.javaClass.simpleName}, " +
                    "mValues type=${valuesField.type.simpleName}")
            return poison()
        } catch (t: Throwable) {
            Log.e(TAG, "AdbSpoof: install threw: ${t.javaClass.simpleName}: ${t.message}", t)
            return false
        }
    }

    private fun dumpFieldSummary(cache: Any) {
        try {
            val fields = HiddenApiBridge.reflectGetDeclaredFieldsArray(cache.javaClass)
                ?: return
            for (f in fields) {
                if (f.name == "mValues" || f.name == "mReadableFieldsWithRedactedValue" ||
                    f.name == "mGenerationTrackers") {
                    try {
                        HiddenApiBridge.reflectSetAccessible(f)
                        val v = try { f.get(cache) } catch (_: Throwable) { null }
                        val asMap = v as? Map<*, *>
                        val sz = asMap?.size ?: 0
                        val keys = asMap?.keys?.toList()?.take(8)
                        Log.d(TAG, "AdbSpoof.dump: [${f.name}] size=$sz keys=$keys")
                    } catch (_: Throwable) {}
                }
            }
        } catch (_: Throwable) {}
    }

    @Synchronized
    fun poison(): Boolean {
        val cache = sCacheInstance ?: return false
        val field = mValuesField ?: return false

        try {
            val raw = field.get(cache)
            if (raw == null) {
                val fresh = android.util.ArrayMap<String, String>()
                for ((k, v) in POISON) fresh[k] = v
                field.set(cache, fresh)
                Log.d(TAG, "AdbSpoof: mValues null, allocated ArrayMap ${POISON.size} keys")
                return true
            }
            @Suppress("UNCHECKED_CAST")
            val map = raw as? MutableMap<String, String> ?: return false
            var changed = 0
            for ((k, v) in POISON) {
                if (map[k] != v) {
                    map[k] = v
                    changed++
                }
            }
            Log.d(TAG, "AdbSpoof: poison applied — $changed/${POISON.size} keys changed, " +
                    "map size=${map.size}")

            val ctx = appCtx
            if (ctx != null) {
                for ((k, _) in POISON) {
                    val s = try { Settings.Global.getString(ctx.contentResolver, k) }
                    catch (t: Throwable) { "<threw: ${t.javaClass.simpleName}>" }
                    val n = try { Settings.Global.getInt(ctx.contentResolver, k, -999) }
                    catch (_: Throwable) { -998 }
                    Log.d(TAG, "AdbSpoof: post-poison getString('$k') = '${s ?: "null"}' getInt('$k') = $n")
                }
            }
            return true
        } catch (t: Throwable) {
            Log.e(TAG, "AdbSpoof: poison threw: ${t.javaClass.simpleName}: ${t.message}", t)
            return false
        }
    }

    fun isInstalled(): Boolean = installed
}