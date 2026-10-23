package com.ims134.hookengine

import android.content.Context
import android.content.pm.PackageInfo
import android.util.Log
import java.lang.reflect.Proxy

object PmSpoof {
    private var installed = false

    @Synchronized
    fun install(ctx: Context) {
        if (installed) return
        try {
            val pm = ctx.packageManager
            val pmCls = pm.javaClass
            Log.d("DualCore", "PmSpoof: PM cls=${pmCls.name}")

            val iPmCls = Class.forName("android.content.pm.IPackageManager")

            val instField = pmCls.getDeclaredField("mPM").apply { isAccessible = true }
            val realPm = instField.get(pm)
            if (realPm == null) { Log.e("DualCore", "PmSpoof: mPM null"); return }

            val atCls = Class.forName("android.app.ActivityThread")
            val staticField = atCls.getDeclaredField("sPackageManager").apply { isAccessible = true }
            val staticPm = staticField.get(null)
            Log.d("DualCore", "PmSpoof: instance.mPM=${realPm.javaClass.simpleName} " +
                    "static.sPackageManager=${staticPm?.javaClass?.simpleName ?: "null"}")

            val pkgName = ctx.packageName

            val handler = java.lang.reflect.InvocationHandler { _, method, args ->
                val result = try {
                    if (args == null) method.invoke(realPm)
                    else method.invoke(realPm, *args)
                } catch (e: java.lang.reflect.InvocationTargetException) {
                    throw e.targetException
                }
                if (result is PackageInfo && args != null && args.isNotEmpty()) {
                    val askedPkg = args[0] as? String
                    if (askedPkg == pkgName) {
                        nullOutSignatures(result)
                    }
                }
                result
            }

            val proxy = Proxy.newProxyInstance(
                iPmCls.classLoader, arrayOf(iPmCls), handler)

            instField.set(pm, proxy)
            Log.d("DualCore", "PmSpoof: instance mPM -> ${instField.get(pm)?.javaClass?.name}")

            try {
                staticField.set(null, proxy)
                Log.d("DualCore", "PmSpoof: static sPackageManager -> ${staticField.get(null)?.javaClass?.name}")
            } catch (t: Throwable) {
                Log.e("DualCore", "PmSpoof: sPackageManager swap failed: $t")
            }

            installed = true
        } catch (e: Throwable) {
            Log.e("DualCore", "PmSpoof install failed: $e")
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
            } catch (_: Throwable) {
            }
        } catch (t: Throwable) {
            Log.e("DualCore", "PmSpoof: null-out failed: $t")
        }
    }
}