package com.ims134.hookengine

import android.util.Log
import java.lang.reflect.Field

/**
 * Chains the guest's BaseDexClassLoader to include the host's DEX elements.
 *
 * After this runs, `guest.loadClass("com.ims134.hookengine.Foo")` succeeds
 * because the guest's DexPathList now contains host elements before its own.
 * This is what the reference engine's `nativeFNLN(ClassLoader, ClassLoader,
 * boolean)` does — we do it via hidden-api bypass reflection, no native hook.
 *
 * Requires: PmSpoof.init / HiddenApiBridge.reflectGetDeclaredField working.
 */
object ClassLoaderBridge {
    private const val TAG = "DualCore"
    private var installed = false

    /**
     * Idempotent install using the current thread's context classloader as
     * guest and our own loader as host. Called from DualApplication.attachBaseContext.
     */
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

    /**
     * Explicit form for testing and DualHost integration. Chains host dex
     * elements into guest DexPathList. Safe to call multiple times.
     */
    @Synchronized
    fun installFor(guest: ClassLoader, host: ClassLoader): Boolean {
        if (guest === host) {
            Log.d(TAG, "ClassLoaderBridge.installFor: guest == host, nothing to chain")
            installed = true
            return true
        }
        return try {
            chain(guest, host)
            installed = true
            Log.d(TAG, "ClassLoaderBridge: chained ${guest.javaClass.simpleName} → ${host.javaClass.simpleName}")
            true
        } catch (t: Throwable) {
            Log.e(TAG, "ClassLoaderBridge: chain failed", t)
            false
        }
    }

    fun isInstalled(): Boolean = installed

    private fun chain(guest: ClassLoader, host: ClassLoader) {
        val pathListField = findField(guest.javaClass, "pathList")
            ?: throw IllegalStateException("no pathList on ${guest.javaClass.name}")

        val guestPathList = pathListField.get(guest)
            ?: throw IllegalStateException("guest pathList null")
        val hostPathList = pathListField.get(host)
            ?: throw IllegalStateException("host pathList null")

        val dexElementsField = findField(guestPathList.javaClass, "dexElements")
            ?: throw IllegalStateException("no dexElements on ${guestPathList.javaClass.name}")

        val guestElems = dexElementsField.get(guestPathList) as? Array<*> ?: run {
            throw IllegalStateException("guest dexElements null")
        }
        val hostElems = dexElementsField.get(hostPathList) as? Array<*> ?: run {
            throw IllegalStateException("host dexElements null")
        }

        Log.d(TAG, "ClassLoaderBridge: guest elements=${guestElems.size} host elements=${hostElems.size}")

        // Guard: if host elements are already present, skip (idempotent)
        val guestSet = guestElems.filterNotNull().toSet()
        val hostOnly = hostElems.filterNotNull().filter { it !in guestSet }
        if (hostOnly.isEmpty()) {
            Log.d(TAG, "ClassLoaderBridge: all host elements already present, nothing to add")
            return
        }

        // Build merged array: host elements FIRST, then guest
        val elementClass = Class.forName("dalvik.system.DexPathList\$Element")
        val total = hostOnly.size + guestElems.size
        val merged = java.lang.reflect.Array.newInstance(elementClass, total) as Array<Any?>

        var i = 0
        for (e in hostOnly) { java.lang.reflect.Array.set(merged, i++, e) }
        for (e in guestElems) { if (e != null) java.lang.reflect.Array.set(merged, i++, e) }

        dexElementsField.set(guestPathList, merged)
        Log.d(TAG, "ClassLoaderBridge: merged array size=${merged.size} (host ${hostOnly.size} + guest ${guestElems.size})")
    }

    private fun findField(cls: Class<*>, name: String): Field? {
        var c: Class<*>? = cls
        while (c != null) {
            HiddenApiBridge.reflectGetDeclaredField(c, name)?.let { f ->
                HiddenApiBridge.reflectSetAccessible(f)
                return f
            }
            c = c.superclass
        }
        return null
    }
}