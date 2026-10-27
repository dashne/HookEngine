package com.ims134.hookengine

import android.util.Log
import dalvik.system.DexClassLoader

/**
 * Session 55 — Feature 4.3: Dynamic DEX loading.
 *
 * Constructs a DexClassLoader from a runtime-provided dex/apk path. The
 * construction runs in C++ (`DexLoader.cpp`). This Kotlin surface is a
 * thin wrapper — argument marshaling only.
 *
 * The resulting loader can be chained into a target ClassLoader via
 * Feature 4.2's `ClassLoaderBridge.installFor(target, newLoader)` to
 * make the new classes resolvable from the target loader.
 *
 * Use cases:
 *   - Load feature modules at runtime
 *   - Load extracted DEX from a guest APK without installing it
 *   - Hot-swap code for instrumentation
 *
 * Constraints:
 *   - optimizedDir must be a writable directory (app.codeCacheDir or similar)
 *   - The dex file at dexPath must not already be registered with a
 *     different classloader whose element set doesn't overlap with the
 *     new loader's parent chain. Standard pattern: use the same parent
 *     as the target loader so ART's dex cache is shared.
 */
object DexLoader {
    private const val TAG = "DualCore"
    init { System.loadLibrary("hookengine") }

    private external fun nativeCreate(
        dexPath: String, optDir: String, parent: ClassLoader?): ClassLoader?

    /**
     * Construct a DexClassLoader from the given APK or DEX file.
     *
     * @param dexPath       absolute path to a dex/apk file
     * @param optimizedDir  writable directory for dex optimization (may be
     *                      ignored on Android 8+; provided for compatibility)
     * @param parent        parent ClassLoader, or null for boot classloader
     * @return the created DexClassLoader, or null on failure (logged)
     */
    @JvmStatic
    fun create(dexPath: String, optimizedDir: String, parent: ClassLoader? = null): DexClassLoader? {
        return try {
            val loader = nativeCreate(dexPath, optimizedDir, parent)
            when {
                loader == null -> {
                    Log.e(TAG, "DexLoader.create: native returned null for $dexPath")
                    null
                }
                loader is DexClassLoader -> loader
                else -> {
                    Log.e(TAG, "DexLoader.create: unexpected type ${loader.javaClass.name}")
                    null
                }
            }
        } catch (t: Throwable) {
            Log.e(TAG, "DexLoader.create threw: ${t.javaClass.simpleName}", t)
            null
        }
    }
}