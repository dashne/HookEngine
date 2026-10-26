package com.ims134.hookengine

import android.content.ComponentName
import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.content.pm.ProviderInfo
import android.content.pm.ResolveInfo
import android.util.Log
import java.lang.reflect.Field
import java.lang.reflect.InvocationHandler
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Proxy

object PmSpoof {
    private const val TAG = "DualCore"

    @Volatile var hiddenPackages: Set<String> = emptySet()
    /**
     * Session 18d — packages whose signatures should be nulled out on
     * getPackageInfo. Superset of `ownPkg` because DualHost may load a
     * guest whose own signature check must pass under our process.
     * Populated by GuestHost.load on success; read by rewrite() at call
     * time via volatile, so post-install registrations take effect
     * without a re-install.
     */
    @Volatile var signatureNullOutPackages: Set<String> = emptySet()
    /**
     * Session 21 — guest archive paths for provider synthesis.
     * Package name -> staged APK absolute path. When guest code calls
     * pm.getProviderInfo() for one of its own providers, AMS rejects
     * with NameNotFoundException (guest isn't installed). Synthesize
     * the ProviderInfo from the APK archive on the throw path.
     */
    @Volatile var guestArchives: Map<String, String> = emptyMap()

    /** Host PackageManager captured at install — used for archive reads. */
    @Volatile private var hostPm: android.content.pm.PackageManager? = null

    /** True only if proxy + list field are both in place. */
    fun isInstalled(): Boolean = storedProxy != null && listFieldResolved

    // Guards a single proxy instance across every install() call.
    @Volatile private var storedProxy: Any? = null
    @Volatile private var cachedListField: Field? = null
    @Volatile private var listFieldResolved = false

    init { System.loadLibrary("hookengine") }

    internal external fun getSliceListNative(slice: Any): List<Any>?
    internal external fun replaceSliceListNative(slice: Any, list: List<Any>): Any?

    /**
     * Idempotent install. Every part retries on every call.
     *
     * Safe to call from attachBaseContext, onCreate, and tests. If any part
     * failed on an earlier call (e.g. sPackageManager not yet initialized),
     * the next call retries it.
     */
    @Synchronized
    fun install(ctx: Context) {
        if (hostPm == null) {
            hostPm = ctx.packageManager
        }
        // 1. Create the proxy once — but re-attempt if a prior call failed
        if (storedProxy == null) {
            try {
                createProxy(ctx)
            } catch (t: Throwable) {
                Log.e(TAG, "PmSpoof: createProxy failed: $t", t)
                return   // can't install anything without the proxy
            }
        }
        val proxy = storedProxy ?: return

        // 2. Resolve BaseParceledListSlice.mList — retry until it succeeds
        if (!listFieldResolved) {
            try {
                resolveListField()
            } catch (t: Throwable) {
                Log.e(TAG, "PmSpoof: resolveListField threw: $t", t)
            }
        }

        // 3. Static field: ActivityThread.sPackageManager = proxy (always retry)
        try {
            installOnStatic(proxy)
        } catch (t: Throwable) {
            Log.w(TAG, "PmSpoof: installOnStatic will retry: $t")
        }

        // 4. Instance field: ctx.packageManager.mPM = proxy (always retry)
        try {
            installOnInstance(ctx, proxy)
        } catch (t: Throwable) {
            Log.w(TAG, "PmSpoof: installOnInstance will retry: $t")
        }

        Log.d(TAG, "PmSpoof: state list=${cachedListField != null} " +
                "static=${isStaticInstalled(proxy)} instance=${isInstanceInstalled(ctx, proxy)}")
    }

    private fun createProxy(ctx: Context) {
        val pm = ctx.packageManager ?: throw IllegalStateException("no packageManager")
        val instField = pm.javaClass.getDeclaredField("mPM").apply { isAccessible = true }
        val realPm = instField.get(pm) ?: throw IllegalStateException("mPM null")
        val iPmCls = Class.forName("android.content.pm.IPackageManager")
        val pkgName = ctx.packageName

        val handler = InvocationHandler { _, method, args ->
            // Session 21b — first-line diagnostic: confirm proxy is consulted.
            if (method.name == "getProviderInfo" || method.name == "resolveContentProvider") {
                Log.d(TAG, "PmSpoof: proxy ${method.name} intercepted args=${args?.joinToString { it?.javaClass?.simpleName ?: "null" }}")
            }

            val result = try {
                if (args == null) method.invoke(realPm) else method.invoke(realPm, *args)
            } catch (e: InvocationTargetException) {
                val target = e.targetException
                // Exception-path synthesis (kept from 21a).
                if (method.name == "getProviderInfo" &&
                    target is android.content.pm.PackageManager.NameNotFoundException) {
                    val synthetic = synthesizeGuestProviderInfo(args)
                    if (synthetic != null) {
                        Log.d(TAG, "PmSpoof: getProviderInfo exception-path synthesized for ${synthetic.name}")
                        return@InvocationHandler synthetic
                    }
                }
                throw target
            }

            // Session 21b — NULL-RETURN path. On API 34+ ApplicationPackageManager
            // calls mPM.getProviderInfo(), receives null, and throws NameNotFoundException
            // client-side. The proxy sees null, no exception. Synthesize here too.
            if (result == null && method.name == "getProviderInfo") {
                val synthetic = synthesizeGuestProviderInfo(args)
                if (synthetic != null) {
                    Log.d(TAG, "PmSpoof: getProviderInfo null-path synthesized for ${synthetic.name}")
                    return@InvocationHandler synthetic
                }
            }

            val n = method.name
            if (n.startsWith("getInstalled") || n.startsWith("queryIntent")) {
                Log.d(TAG, "PmSpoof: proxy $n result=${result?.javaClass?.name}")
            }
            rewrite(n, args, result, pkgName)
        }

        storedProxy = Proxy.newProxyInstance(iPmCls.classLoader, arrayOf(iPmCls), handler)
        Log.d(TAG, "PmSpoof: proxy created")
    }

    private fun installOnStatic(proxy: Any) {
        val atCls = Class.forName("android.app.ActivityThread")
        val staticField = atCls.getDeclaredField("sPackageManager").apply { isAccessible = true }
        val current = staticField.get(null)
        if (current === proxy) return
        staticField.set(null, proxy)
        Log.d(TAG, "PmSpoof: sPackageManager set")
    }

    private fun installOnInstance(ctx: Context, proxy: Any) {
        val pm = ctx.packageManager ?: return
        val instField = pm.javaClass.getDeclaredField("mPM").apply { isAccessible = true }
        val current = instField.get(pm)
        if (current === proxy) return
        instField.set(pm, proxy)
        Log.d(TAG, "PmSpoof: mPM set")
    }

    private fun isStaticInstalled(proxy: Any): Boolean = try {
        val atCls = Class.forName("android.app.ActivityThread")
        val f = atCls.getDeclaredField("sPackageManager").apply { isAccessible = true }
        f.get(null) === proxy
    } catch (_: Throwable) { false }

    private fun isInstanceInstalled(ctx: Context, proxy: Any): Boolean = try {
        val pm = ctx.packageManager ?: return false
        val f = pm.javaClass.getDeclaredField("mPM").apply { isAccessible = true }
        f.get(pm) === proxy
    } catch (_: Throwable) { false }

    private fun resolveListField() {
        if (cachedListField != null) return
        val base = try { Class.forName("android.content.pm.BaseParceledListSlice") }
        catch (_: Throwable) { null }
        val slice = try { Class.forName("android.content.pm.ParceledListSlice") }
        catch (_: Throwable) { null }
        val candidates = listOfNotNull(base, slice).distinct()
        val names = listOf("mList", "list", "mItems", "items", "mElements", "elements")
        for (c in candidates) {
            for (n in names) {
                val f = HiddenApiBridge.reflectGetDeclaredField(c, n) ?: continue
                if (!HiddenApiBridge.reflectSetAccessible(f)) {
                    Log.w(TAG, "PmSpoof: setAccessible(${c.simpleName}.$n) failed")
                }
                Log.d(TAG, "PmSpoof: resolved list field on ${c.name} = $n")
                cachedListField = f
                listFieldResolved = true
                return
            }
        }
        Log.e(TAG, "PmSpoof: could not resolve list field via bypass — will retry next install")
    }

    private fun rewrite(name: String, args: Array<Any?>?, result: Any?, ownPkg: String): Any? {
        when {
            name == "getPackageInfo" || name == "getPackageInfoAsUser" -> {
                if (result is PackageInfo && args != null && args.isNotEmpty()) {
                    val requested = args[0] as? String
                    if (requested != null &&
                        (requested == ownPkg || requested in signatureNullOutPackages)) {
                        nullOutSignatures(result)
                    }
                }
                return result
            }
            name.startsWith("getInstalledPackages") ->
                return filterList(result) { o -> o !is PackageInfo || o.packageName !in hiddenPackages }
            name.startsWith("getInstalledApplications") ->
                return filterList(result) { o -> o !is ApplicationInfo || o.packageName !in hiddenPackages }
            name.startsWith("queryIntentActivities") ->
                return filterList(result) { o -> o !is ResolveInfo || (o.activityInfo?.packageName) !in hiddenPackages }
            name.startsWith("queryIntentServices") ->
                return filterList(result) { o -> o !is ResolveInfo || (o.serviceInfo?.packageName) !in hiddenPackages }
            name.startsWith("queryIntentReceivers") ->
                return filterList(result) { o -> o !is ResolveInfo || (o.activityInfo?.packageName) !in hiddenPackages }
            name.startsWith("queryIntentContentProviders") ->
                return filterList(result) { o -> o !is ResolveInfo || (o.providerInfo?.packageName) !in hiddenPackages }
        }
        return result
    }

    /**
     * Session 21 — synthesize a ProviderInfo for a guest provider.
     * Called from the proxy's exception path — AMS already threw
     * NameNotFoundException. Read the provider declaration from
     * getPackageArchiveInfo(GET_PROVIDERS | GET_META_DATA) and return
     * it so the caller reads metaData and completes initialization.
     */
    private fun synthesizeGuestProviderInfo(args: Array<Any?>?): ProviderInfo? {
        if (args == null || args.isEmpty()) {
            Log.w(TAG, "synth: no args")
            return null
        }
        val comp = args[0] as? ComponentName
        if (comp == null) {
            Log.w(TAG, "synth: args[0] is not ComponentName — actual ${args[0]?.javaClass?.name}")
            return null
        }
        val guestApk = guestArchives[comp.packageName]
        if (guestApk == null) {
            Log.d(TAG, "synth: package ${comp.packageName} not in guestArchives ${guestArchives.keys}")
            return null
        }
        val pm = hostPm
        if (pm == null) {
            Log.w(TAG, "synth: hostPm null")
            return null
        }

        val archives = try {
            pm.getPackageArchiveInfo(
                guestApk,
                android.content.pm.PackageManager.GET_PROVIDERS or
                        android.content.pm.PackageManager.GET_META_DATA)
        } catch (t: Throwable) {
            Log.w(TAG, "synthesizeGuestProviderInfo: archive read failed: $t")
            return null
        } ?: return null

        val providers = archives.providers ?: return null
        val wanted = comp.className
        val matching = providers.firstOrNull { p ->
            val name = p?.name ?: return@firstOrNull false
            name == wanted ||
                    (name.startsWith(".") && comp.packageName + name == wanted) ||
                    (!name.contains(".") && comp.packageName + "." + name == wanted)
        } ?: run {
            Log.d(TAG, "synthesizeGuestProviderInfo: no match for $wanted " +
                    "in ${providers.size} providers")
            return null
        }

        val ai = archives.applicationInfo ?: ApplicationInfo().apply {
            packageName = comp.packageName
            sourceDir = guestApk
            publicSourceDir = guestApk
        }
        ai.sourceDir = guestApk
        ai.publicSourceDir = guestApk
        if (ai.packageName.isNullOrEmpty()) ai.packageName = comp.packageName

        matching.packageName = comp.packageName
        matching.applicationInfo = ai
        Log.d(TAG, "synthesizeGuestProviderInfo: OK for ${comp.flattenToString()} " +
                "metaData=${matching.metaData?.keySet()?.joinToString(",") ?: "null"}")
        return matching
    }

    private fun filterList(result: Any?, keep: (Any?) -> Boolean): Any? {
        if (result == null) return null
        if (result is List<*>) {
            val filtered = result.filter(keep)
            return if (filtered.size == result.size) result else filtered
        }
        val field = cachedListField ?: run {
            Log.w(TAG, "PmSpoof: no cached list field — returning unchanged")
            return result
        }
        val list = try {
            @Suppress("UNCHECKED_CAST")
            field.get(result) as? List<*>
        } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof: field.get failed on ${result.javaClass.name}: $t")
            null
        } ?: return result
        val filtered = list.filter(keep)
        Log.d(TAG, "PmSpoof: filter ${list.size} -> ${filtered.size} (${result.javaClass.simpleName})")
        if (filtered.size == list.size) return result
        try {
            field.set(result, filtered)
            Log.d(TAG, "PmSpoof: field.set OK size=${filtered.size}")
            return result
        } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof: field.set failed: $t")
            return result
        }
    }

    internal fun nullOutSignatures(pi: PackageInfo) {
        try {
            val sigCls = Class.forName("android.content.pm.Signature")
            val empty = java.lang.reflect.Array.newInstance(sigCls, 0)
            @Suppress("DEPRECATION")
            val sigFld = PackageInfo::class.java.getDeclaredField("signatures")
            sigFld.isAccessible = true
            sigFld.set(pi, empty)
            try {
                val siFld = PackageInfo::class.java.getDeclaredField("signingInfo")
                siFld.isAccessible = true
                val si = siFld.get(pi) ?: return
                val siCls = Class.forName("android.content.pm.SigningInfo")
                val sdFld = siCls.getDeclaredField("mSigningDetails")
                sdFld.isAccessible = true
                val sd = sdFld.get(si) ?: return
                val sdCls = Class.forName("android.content.pm.SigningDetails")
                val sigsFld = sdCls.getDeclaredField("signatures")
                sigsFld.isAccessible = true
                sigsFld.set(sd, empty)
            } catch (_: Throwable) {}
        } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof: null-out failed: $t")
        }
    }
}