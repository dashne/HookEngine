package com.ims134.hookengine

import android.content.ComponentName
import android.content.Context
import android.content.pm.ActivityInfo
import android.content.pm.ApplicationInfo
import android.content.pm.ComponentInfo
import android.content.pm.PackageInfo
import android.content.pm.ProviderInfo
import android.content.pm.ResolveInfo
import android.content.pm.ServiceInfo
import android.util.Log
import java.lang.reflect.Field
import java.lang.reflect.InvocationHandler
import java.lang.reflect.InvocationTargetException
import java.lang.reflect.Modifier
import java.lang.reflect.Proxy

object PmSpoof {
    private const val TAG = "DualCore"

    @Volatile var hiddenPackages: Set<String> = emptySet()

    @Volatile var signatureNullOutPackages: Set<String> = emptySet()

    @Volatile var guestArchives: Map<String, String> = emptyMap()

    @Volatile private var hostPm: android.content.pm.PackageManager? = null

    fun isInstalled(): Boolean = storedProxy != null && listFieldResolved

    @Volatile private var storedProxy: Any? = null
    @Volatile private var cachedListField: Field? = null
    @Volatile private var listFieldResolved = false

    init { System.loadLibrary("hookengine") }

    internal external fun getSliceListNative(slice: Any): List<Any>?
    internal external fun replaceSliceListNative(slice: Any, list: List<Any>): Any?

    @Synchronized
    fun install(ctx: Context) {
        if (hostPm == null) {
            hostPm = ctx.packageManager
        }
        if (storedProxy == null) {
            try {
                createProxy(ctx)
            } catch (t: Throwable) {
                Log.e(TAG, "PmSpoof: createProxy failed: $t", t)
                return
            }
        }
        val proxy = storedProxy ?: return

        if (!listFieldResolved) {
            try {
                resolveListField()
            } catch (t: Throwable) {
                Log.e(TAG, "PmSpoof: resolveListField threw: $t", t)
            }
        }

        try {
            installOnStatic(proxy)
        } catch (t: Throwable) {
            Log.w(TAG, "PmSpoof: installOnStatic will retry: $t")
        }

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
            if (method.name == "getProviderInfo" || method.name == "resolveContentProvider"
                || method.name == "getServiceInfo" || method.name == "getActivityInfo"
                || method.name == "getReceiverInfo") {
                Log.d(TAG, "PmSpoof: proxy ${method.name} intercepted args=" +
                        args?.joinToString { it?.javaClass?.simpleName ?: "null" })
            }

            val result = try {
                if (args == null) method.invoke(realPm) else method.invoke(realPm, *args)
            } catch (e: InvocationTargetException) {
                val target = e.targetException

                if (method.name == "getProviderInfo" &&
                    target is android.content.pm.PackageManager.NameNotFoundException) {
                    val synthetic = synthesizeGuestProviderInfo(args)
                    if (synthetic != null) {
                        Log.d(TAG, "PmSpoof: getProviderInfo exception-path synthesized for ${synthetic.name}")
                        return@InvocationHandler synthetic
                    }
                }
                if (method.name == "getApplicationInfo" &&
                    target is android.content.pm.PackageManager.NameNotFoundException) {
                    val synthetic = synthesizeGuestApplicationInfo(args)
                    if (synthetic != null) {
                        Log.d(TAG, "PmSpoof: getApplicationInfo exception-path synthesized for ${synthetic.packageName}")
                        return@InvocationHandler synthetic
                    }
                }
                if (method.name == "getServiceInfo" &&
                    target is android.content.pm.PackageManager.NameNotFoundException) {
                    val syn = synthesizeGuestComponentInfo(args, "getServiceInfo")
                    if (syn != null) {
                        Log.d(TAG, "PmSpoof: getServiceInfo exception-path synthesized for ${syn.name}")
                        return@InvocationHandler syn
                    }
                }
                if (method.name == "getActivityInfo" &&
                    target is android.content.pm.PackageManager.NameNotFoundException) {
                    val syn = synthesizeGuestComponentInfo(args, "getActivityInfo")
                    if (syn != null) {
                        Log.d(TAG, "PmSpoof: getActivityInfo exception-path synthesized for ${syn.name}")
                        return@InvocationHandler syn
                    }
                }
                if (method.name == "getReceiverInfo" &&
                    target is android.content.pm.PackageManager.NameNotFoundException) {
                    val syn = synthesizeGuestComponentInfo(args, "getReceiverInfo")
                    if (syn != null) {
                        Log.d(TAG, "PmSpoof: getReceiverInfo exception-path synthesized for ${syn.name}")
                        return@InvocationHandler syn
                    }
                }
                throw target
            }

            if (result == null && method.name == "getProviderInfo") {
                val synthetic = synthesizeGuestProviderInfo(args)
                if (synthetic != null) {
                    Log.d(TAG, "PmSpoof: getProviderInfo null-path synthesized for ${synthetic.name}")
                    return@InvocationHandler synthetic
                }
            }
            if (result == null && method.name == "getApplicationInfo") {
                val synthetic = synthesizeGuestApplicationInfo(args)
                if (synthetic != null) {
                    Log.d(TAG, "PmSpoof: getApplicationInfo null-path synthesized for ${synthetic.packageName}")
                    return@InvocationHandler synthetic
                }
            }
            if (result == null && method.name == "getServiceInfo") {
                val syn = synthesizeGuestComponentInfo(args, "getServiceInfo")
                if (syn != null) {
                    Log.d(TAG, "PmSpoof: getServiceInfo null-path synthesized for ${syn.name}")
                    return@InvocationHandler syn
                }
            }
            if (result == null && method.name == "getActivityInfo") {
                val syn = synthesizeGuestComponentInfo(args, "getActivityInfo")
                if (syn != null) {
                    Log.d(TAG, "PmSpoof: getActivityInfo null-path synthesized for ${syn.name}")
                    return@InvocationHandler syn
                }
            }
            if (result == null && method.name == "getReceiverInfo") {
                val syn = synthesizeGuestComponentInfo(args, "getReceiverInfo")
                if (syn != null) {
                    Log.d(TAG, "PmSpoof: getReceiverInfo null-path synthesized for ${syn.name}")
                    return@InvocationHandler syn
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

    private fun synthesizeGuestProviderInfo(args: Array<Any?>?): ProviderInfo? {
        if (args == null || args.isEmpty()) return null
        val comp = args[0] as? ComponentName ?: return null
        val guestApk = guestArchives[comp.packageName] ?: return null
        val pm = hostPm ?: return null
        val archives = try {
            pm.getPackageArchiveInfo(guestApk,
                android.content.pm.PackageManager.GET_PROVIDERS or
                        android.content.pm.PackageManager.GET_META_DATA)
        } catch (_: Throwable) { null } ?: return null
        val providers = archives.providers ?: return null
        val wanted = comp.className
        val matching = providers.firstOrNull { p ->
            val name = p?.name ?: return@firstOrNull false
            name == wanted ||
                    (name.startsWith(".") && comp.packageName + name == wanted) ||
                    (!name.contains(".") && comp.packageName + "." + name == wanted)
        } ?: return null
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
        return matching
    }

    private fun synthesizeGuestApplicationInfo(args: Array<Any?>?): ApplicationInfo? {
        if (args == null || args.isEmpty()) return null
        val pkg = args[0] as? String ?: return null
        val guestApk = guestArchives[pkg] ?: return null
        val pm = hostPm ?: return null
        val archive = try { pm.getPackageArchiveInfo(guestApk, 0) } catch (_: Throwable) { null } ?: return null
        val ai = archive.applicationInfo ?: return null
        ai.sourceDir = guestApk
        ai.publicSourceDir = guestApk
        if (ai.packageName.isNullOrEmpty()) ai.packageName = pkg
        return ai
    }

    private fun synthesizeGuestComponentInfo(
        args: Array<Any?>?,
        which: String
    ): ComponentInfo? {
        if (args == null || args.isEmpty()) return null
        val comp = args[0] as? ComponentName ?: return null
        val guestApk = guestArchives[comp.packageName] ?: return null
        val pm = hostPm ?: return null
        val archives = try {
            pm.getPackageArchiveInfo(guestApk,
                android.content.pm.PackageManager.GET_SERVICES or
                        android.content.pm.PackageManager.GET_ACTIVITIES or
                        android.content.pm.PackageManager.GET_RECEIVERS or
                        android.content.pm.PackageManager.GET_META_DATA)
        } catch (_: Throwable) { null } ?: return null
        val candidates: List<ComponentInfo> = when (which) {
            "getServiceInfo"  -> archives.services?.toList() ?: emptyList()
            "getActivityInfo" -> archives.activities?.toList() ?: emptyList()
            "getReceiverInfo" -> archives.receivers?.toList() ?: emptyList()
            else -> return null
        }.filterNotNull()
        val wanted = comp.className
        val match = candidates.firstOrNull { c ->
            val name = c.name ?: return@firstOrNull false
            name == wanted ||
                    (name.startsWith(".") && comp.packageName + name == wanted) ||
                    (!name.contains(".") && comp.packageName + "." + name == wanted)
        } ?: return null
        val ai = archives.applicationInfo ?: ApplicationInfo().apply {
            packageName = comp.packageName
            sourceDir = guestApk
            publicSourceDir = guestApk
        }
        ai.sourceDir = guestApk
        ai.publicSourceDir = guestApk
        if (ai.packageName.isNullOrEmpty()) ai.packageName = comp.packageName
        match.packageName = comp.packageName
        match.applicationInfo = ai
        return match
    }

    private fun filterList(result: Any?, keep: (Any?) -> Boolean): Any? {
        if (result == null) return null
        if (result is List<*>) {
            val filtered = result.filter(keep)
            return if (filtered.size == result.size) result else filtered
        }
        val field = cachedListField ?: return result
        val list = try {
            @Suppress("UNCHECKED_CAST")
            field.get(result) as? List<*>
        } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof: field.get failed on ${result.javaClass.name}: $t")
            null
        } ?: return result
        val filtered = list.filter(keep)
        if (filtered.size == list.size) return result
        try {
            field.set(result, filtered)
            return result
        } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof: field.set failed: $t")
            return result
        }
    }

    /**
     * Session 62 — signature null-out, probe iteration #3.
     *
     * ─── History ────────────────────────────────────────────────────
     *
     * Iteration 1: direct reflection, both paths. Legacy `signatures`
     * nulled fine (api=unsupported); modern `signingInfo.mSigningDetails
     * .signatures` refused with IllegalAccessException (api=blocked).
     *
     * Iteration 2: routing `mSigningDetails` through HiddenApiBridge got
     * past the field-lookup wall, but `SigningDetails.signatures` threw
     * NoSuchFieldException. The AOSP field name doesn't exist here.
     *
     * Iteration 3 (this): iterating `sdCls.declaredFields` from the
     * caller thread returned 0 fields. That is the app-domain
     * hidden-api filter on `api=blocked` classes: `getDeclaredFields()`
     * returns an empty array without throwing. The field exists, it just
     * isn't visible to an app-domain caller.
     *
     * ─── Fix ────────────────────────────────────────────────────────
     *
     * Route the field enumeration through
     * `HiddenApiBridge.reflectGetDeclaredFieldsArray` — same bypass
     * thread that reveals hidden fields. Then null EVERY non-static
     * `Signature[]` field we find, name-agnostic. Log the full field
     * inventory (name:type) so that on a future MIUI restructure we
     * immediately see what the ROM has.
     *
     * ─── Strategic note ─────────────────────────────────────────────
     *
     * On THIS device, the signature is the real Xiaomi release
     * signature — no test-keys detector triggers on it. The null-out is
     * defense-in-depth for a future port or a device that happens to be
     * test-keys-signed. Feature 5.13 (test-keys / custom-build detect)
     * is satisfied by the property surface + natural Build.TAGS state
     * regardless of this function's outcome. If the bypass route still
     * fails, we document the modern-signature-path wall for Phase F
     * and close 5.13 on the property/natural surface.
     */
    internal fun nullOutSignatures(pi: PackageInfo) {
        try {
            val sigCls = Class.forName("android.content.pm.Signature")
            val empty = java.lang.reflect.Array.newInstance(sigCls, 0)

            // ─── Legacy path — PackageInfo.signatures ────────────────
            try {
                @Suppress("DEPRECATION")
                val sigFld = PackageInfo::class.java.getDeclaredField("signatures")
                sigFld.isAccessible = true
                sigFld.set(pi, empty)
            } catch (t: Throwable) {
                Log.w(TAG, "PmSpoof: legacy signatures null-out: " +
                        "${t.javaClass.simpleName}: ${t.message}")
            }

            // ─── Modern path — PackageInfo.signingInfo.mSigningDetails ─
            try {
                val siFld = HiddenApiBridge.reflectGetDeclaredField(
                    PackageInfo::class.java, "signingInfo")
                if (siFld == null) return
                if (!HiddenApiBridge.reflectSetAccessible(siFld)) return
                val si = siFld.get(pi) ?: return

                val siCls = Class.forName("android.content.pm.SigningInfo")
                val sdFld = HiddenApiBridge.reflectGetDeclaredField(siCls, "mSigningDetails")
                if (sdFld == null) return
                if (!HiddenApiBridge.reflectSetAccessible(sdFld)) return
                val sd = sdFld.get(si) ?: return

                val sdCls = Class.forName("android.content.pm.SigningDetails")

                // Enumerate fields through the bypass thread. Direct
                // `sdCls.declaredFields` from the caller returns an empty
                // array (probe #3: "0 fields scanned").
                val fieldsArr = HiddenApiBridge.reflectGetDeclaredFieldsArray(sdCls)
                if (fieldsArr == null) {
                    Log.w(TAG, "PmSpoof: reflectGetDeclaredFieldsArray(SigningDetails) returned null")
                    return
                }
                val fieldSummary = fieldsArr.joinToString(limit = 20) {
                    "${it.name}:${it.type.simpleName}"
                }
                Log.d(TAG, "PmSpoof: SigningDetails fields via bypass = ${fieldsArr.size}: [$fieldSummary]")

                var nulled = 0
                for (f in fieldsArr) {
                    if (Modifier.isStatic(f.modifiers)) continue
                    if (!f.type.isArray) continue
                    if (f.type.componentType != sigCls) continue
                    if (!HiddenApiBridge.reflectSetAccessible(f)) {
                        Log.w(TAG, "PmSpoof: setAccessible(SigningDetails.${f.name}) failed")
                        continue
                    }
                    try {
                        f.set(sd, empty)
                        nulled++
                        Log.d(TAG, "PmSpoof: nulled SigningDetails.${f.name}")
                    } catch (t: Throwable) {
                        Log.w(TAG, "PmSpoof: failed to null SigningDetails.${f.name}: " +
                                "${t.javaClass.simpleName}: ${t.message}")
                    }
                }
                if (nulled == 0) {
                    Log.w(TAG, "PmSpoof: no Signature[] field to null on SigningDetails " +
                            "(${fieldsArr.size} fields scanned, inventory above)")
                } else {
                    Log.d(TAG, "PmSpoof: nulled $nulled Signature[] field(s) on SigningDetails")
                }
            } catch (t: Throwable) {
                Log.w(TAG, "PmSpoof: signingInfo null-out: " +
                        "${t.javaClass.simpleName}: ${t.message}")
            }
        } catch (t: Throwable) {
            Log.e(TAG, "PmSpoof: null-out failed: ${t.javaClass.simpleName}: ${t.message}")
        }
    }
}