package com.ims134.hookengine

import dalvik.system.BaseDexClassLoader

/**
 * Session 18 — guest-first ClassLoader for DualHost integration.
 *
 * Observed failure: Viber's ManifestViberApplication.attachBaseContext calls
 * a Kotlin extension inline that resolves to kotlin.text.StringsKt.i0(String,
 * String)Z. PathClassLoader(parent=hostLoader) is parent-first, so the class
 * `kotlin.text.StringsKt` was resolved from the HOST APK (our engine's
 * bundled Kotlin stdlib), not from Viber's dex. Our stdlib version does not
 * have `i0` → NoSuchMethodError. Viber's own dex has it; the loader never
 * reached that dex because parent-first won.
 *
 * The reference engine's nativeFNLN exists to solve exactly this problem
 * (guest's own bundled deps must win for guest-originated lookups). We do
 * not have nativeFNLN decomp (Rule 7). This subclass reproduces the
 * observable behavior — behavior match, not signature match (Rule 5) — by
 * inverting delegation for the class-name prefixes that apps bundle and
 * compile against a specific version. All other names stay parent-first so
 * host engine classes (GuestHost, ParcelRewriter, PmSpoof, AntiDetection,
 * ProxyBridge, …) remain reachable from guest code via the parent chain.
 */
class GuestClassLoader(
    dexPath: String,
    librarySearchPath: String?,
    parent: ClassLoader
) : BaseDexClassLoader(dexPath, null, librarySearchPath, parent) {

    companion object {
        // Prefixes whose guest-side version must win over any host-side copy.
        // These are libraries apps routinely bundle and compile against a
        // specific version, so a host shadow produces NoSuchMethodError /
        // NoClassDefFoundError / NoSuchFieldError at runtime.
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
                // Guest's own dex does not bundle it — fall back to parent
                // (host, then bootclasspath).
                super.loadClass(name, resolve)
            }
        }
    }
}