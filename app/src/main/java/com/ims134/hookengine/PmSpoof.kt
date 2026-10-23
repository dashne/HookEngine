package com.ims134.hookengine

import android.content.Context
import android.content.pm.ApplicationInfo
import android.content.pm.PackageInfo
import android.content.pm.ResolveInfo
import android.util.Log
import java.lang.reflect.Field
import java.lang.reflect.InvocationHandler
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Proxy

object PmSpoof {
    private var installed = false

    @Volatile var hiddenPackages: Set<String> = emptySet()

    // Cached once — the field on BaseParceledListSlice that holds the list.
    @Volatile private var cachedListField: Field? = null

    @Synchronized
    fun install(ctx: Context) {
        if (installed) return
        try {
            resolveListField()
            val pm = ctx.packageManager
            val iPmCls = Class.forName("android.content.pm.IPackageManager")
            val instField = pm.javaClass.getDeclaredField("mPM").apply { isAccessible = true }
            val realPm = instField.get(pm) ?: run {
                Log.e("DualCore", "PmSpoof: mPM null"); return
            }
            val staticField = Class.forName("android.app.ActivityThread")
                .getDeclaredField("sPackageManager").apply { isAccessible = true }

            val pkgName = ctx.packageName
            val handler = InvocationHandler { _, method, args ->
                val result = try {
                    if (args == null) method.invoke(realPm) else method.invoke(realPm, *args)
                } catch (e: InvocationTargetException) { throw e.targetException }
                val n = method.name
                if (n.startsWith("getInstalled") || n.startsWith("queryIntent")) {
                    Log.d("DualCore", "PmSpoof: proxy $n result=${result?.javaClass?.name}")
                }
                rewrite(n, args, result, pkgName)
            }
            val proxy = Proxy.newProxyInstance(iPmCls.classLoader, arrayOf(iPmCls), handler)
            instField.set(pm, proxy)
            try { staticField.set(null, proxy) } catch (_: Throwable) {}
            installed = true
            Log.d("DualCore", "PmSpoof: installed")
        } catch (e: Throwable) {
            Log.e("DualCore", "PmSpoof install failed: $e")
        }
    }

    /** Locate mList on BaseParceledListSlice via the hidden-api bypass. */
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
                val f = HiddenApiBridge.reflectGetDeclaredField(c, n)
                if (f != null) {
                    if (!HiddenApiBridge.reflectSetAccessible(f)) {
                        Log.w("DualCore", "PmSpoof: setAccessible(${c.simpleName}.$n) failed")
                    }
                    // Probe: does get() return a List without throwing?
                    try {
                        f.get(null)  // will throw on non-static — catches illegal config
                    } catch (t: Throwable) {
                        // non-static, expected. Try on a real instance later.
                    }
                    Log.d("DualCore", "PmSpoof: resolved list field on ${c.name} = $n (via bypass)")
                    cachedListField = f
                    return
                }
            }
        }
        Log.e("DualCore", "PmSpoof: could not resolve list field via bypass — filtering disabled")
    }

    private fun rewrite(name: String, args: Array<Any?>?, result: Any?, ownPkg: String): Any? {
        when {
            name == "getPackageInfo" || name == "getPackageInfoAsUser" -> {
                if (result is PackageInfo && args != null && args.isNotEmpty()) {
                    if ((args[0] as? String) == ownPkg) nullOutSignatures(result)
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

    private fun filterList(result: Any?, keep: (Any?) -> Boolean): Any? {
        if (result == null) return null
        if (result is List<*>) {
            val filtered = result.filter(keep)
            return if (filtered.size == result.size) result else filtered
        }
        val field = cachedListField ?: run {
            Log.w("DualCore", "PmSpoof: no cached list field — returning unchanged")
            return result
        }
        val list = try {
            @Suppress("UNCHECKED_CAST")
            field.get(result) as? List<*>
        } catch (t: Throwable) {
            Log.e("DualCore", "PmSpoof: field.get failed on ${result.javaClass.name}: $t")
            null
        } ?: return result
        val filtered = list.filter(keep)
        Log.d("DualCore", "PmSpoof: filter ${list.size} -> ${filtered.size} (${result.javaClass.simpleName})")
        if (filtered.size == list.size) return result
        try {
            field.set(result, filtered)
            Log.d("DualCore", "PmSpoof: field.set OK size=${filtered.size}")
            return result
        } catch (t: Throwable) {
            Log.e("DualCore", "PmSpoof: field.set failed: $t")
            return result
        }
    }

    private fun nullOutSignatures(pi: PackageInfo) {
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
            Log.e("DualCore", "PmSpoof: null-out failed: $t")
        }
    }
}