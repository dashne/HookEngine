package com.ims134.hookengine

import dalvik.system.BaseDexClassLoader

/**
 * Session 20 — guest-first ClassLoader for DualHost integration.
 *
 * Restored from Session 18 after Session 20's native-boot refactor
 * accidentally dropped it. The failure it solves is unchanged:
 *
 * PathClassLoader(parent=hostLoader) is parent-first, so a guest that
 * bundles its own Kotlin stdlib (Viber: kotlin.text.StringsKt.i0)
 * resolves that class from OUR APK, not the guest's. Viber's
 * ViberApplication.attachBaseContext throws NoSuchMethodError because
 * our Kotlin version doesn't have the same symbols.
 *
 * Rule 15 note: this class is JVM classloader delegation policy, NOT
 * engine behavior. The reference's nativeFNLN is the eventual native
 * equivalent (chains dex elements rather than overriding delegation).
 * Until we have nativeFNLN decomp, this Kotlin subclass reproduces its
 * observable effect: guest's own bundled classes win for guest lookups,
 * host engine classes remain reachable via the parent chain.
 */
class GuestClassLoader(
    dexPath: String,
    librarySearchPath: String?,
    parent: ClassLoader
) : BaseDexClassLoader(dexPath, null, librarySearchPath, parent) {

    companion object {
        // Prefixes whose guest-side version must win over any host-side copy.
        // These are libraries apps bundle and compile against a specific
        // version — a host shadow produces NoSuchMethodError / NoClassDefFound.
        //
        // NOT including android.* or java.* — those are bootclasspath and
        // resolve identically from both loaders; no shadow is possible.
        private val GUEST_PRIORITY_PREFIXES = listOf(
            "kotlin.", "kotlinx.",
            "androidx.",
            "com.google.android.material.",
            "com.google.gson.", "com.google.protobuf.", "com.google.firebase.",
            "okhttp3.", "okio.",
            "org.bouncycastle.",
            "com.facebook.",
            "io.reactivex.", "io.grpc.",
            "org.apache.",
        )
    }

    override fun loadClass(name: String, resolve: Boolean): Class<*> {
        val guestFirst = GUEST_PRIORITY_PREFIXES.any { name.startsWith(it) }
        if (!guestFirst) {
            return super.loadClass(name, resolve)
        }
        synchronized(GuestClassLoader::class.java) {
            val loaded = findLoadedClass(name)
            if (loaded != null) {
                if (resolve) resolveClass(loaded)
                return loaded
            }
            return try {
                val c = findClass(name)
                if (resolve) resolveClass(c)
                c
            } catch (_: ClassNotFoundException) {
                super.loadClass(name, resolve)
            }
        }
    }
}