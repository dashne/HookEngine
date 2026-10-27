package com.ims134.hookengine

import android.util.Log

/**
 * Session 54 — Feature 4.2: Native ClassLoader chaining.
 *
 * Rule 15 debt closure: the merge logic that was previously Kotlin
 * reflection (findField + read array + dedupe + merge + write back) now
 * runs in C++ (`NativeFnln.cpp`). This Kotlin object is a thin API
 * surface that delegates to the native function.
 *
 * Behavior preserved from the prior Kotlin implementation:
 *   - guest === host      → no-op, returns true
 *   - guest null / host null → false
 *   - host elements already in guest (identity) → skipped
 *   - merged guest.dexElements = [host-only..., guest-elements...]
 *
 * Hidden-api-bypass reflection needed to reach pathList and dexElements
 * (both private, hidden-api-blocked) goes through HiddenApiBridge from
 * C++ — same pattern ParcelRewriter.cpp uses.
 *
 * Existing ClassLoaderBridgeTest (3 tests) continues to pass:
 *   - installForOnSameLoaderReturnsTrue      → same-loader no-op
 *   - installForOnFreshLoaderReturnsTrue     → native returns 0
 *   - chainMergesDexElementsStructurally     → afterCount >= beforeCount
 */
object ClassLoaderBridge {
    private const val TAG = "DualCore"
    private var installed = false

    init { System.loadLibrary("hookengine") }

    private external fun nativeFnln(guest: ClassLoader, host: ClassLoader): Int

    @Synchronized
    fun install(): Boolean {
        if (installed) return true
        val guest = try { Thread.currentThread().contextClassLoader } catch (_: Throwable) { null }
        val host  = ClassLoaderBridge::class.java.classLoader
        if (guest == null || host == null) {
            Log.e(TAG, "ClassLoaderBridge.install: null loader (guest=$guest host=$host)")
            return false
        }
        return installFor(guest, host)
    }

    @Synchronized
    fun installFor(guest: ClassLoader, host: ClassLoader): Boolean {
        if (guest === host) {
            Log.d(TAG, "ClassLoaderBridge.installFor: guest == host, nothing to chain")
            installed = true
            return true
        }
        return try {
            val rc = nativeFnln(guest, host)
            if (rc >= 0) {
                installed = true
                Log.d(TAG, "ClassLoaderBridge: chained natively, added $rc elements")
                true
            } else {
                Log.e(TAG, "ClassLoaderBridge: native chain failed rc=$rc")
                false
            }
        } catch (t: Throwable) {
            Log.e(TAG, "ClassLoaderBridge: native chain threw", t)
            false
        }
    }

    fun isInstalled(): Boolean = installed
}