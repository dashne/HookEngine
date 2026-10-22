package com.ims134.hookengine

import android.app.Service
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Binder
import android.os.IBinder
import android.util.Log
import java.io.File
import java.lang.reflect.Proxy

class RemoteService : Service() {
    init { System.loadLibrary("hookengine") }

    external fun hookBinderMethod(method: java.lang.reflect.Method)
    external fun installNativeHooks()
    external fun addRedirectRule(from: String, to: String)
    external fun setVirtualUid(uid: Int)
    external fun setInSystemCall(on: Boolean)
    external fun inlineHookGetpid(): Boolean
    external fun nativeGetpid(): Int
    external fun hiddenApiProbe(): Int
    external fun hiddenApiInstall(): Int

    external fun restoreBinderMethod()
    external fun diagRawZip(path: String): String

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.d("DualCore", "Service started. (v27 — both fields)")

        Thread {
            // ============================================================
            // STEP 0 — Diagnostic BEFORE any hooks
            // If these pass, our hooks are NOT the cause of Item 3 crashes.
            // If they crash, the ROM genuinely cannot read the APK.
            // ============================================================
            Log.d("DualCore", "=== STEP 0: pre-hook diagnostics ===")
            val srcDir = applicationInfo.sourceDir
            Log.d("DualCore", "STEP0 srcDir=$srcDir")

            // STEP 0a: raw ZIP read (pure JNI, no framework, no hooks installed yet)
            try {
                val diag = diagRawZip(srcDir)
                Log.d("DualCore", "STEP0a rawZip: $diag")
            } catch (t: Throwable) {
                Log.e("DualCore", "STEP0a rawZip threw: $t")
            }

            // STEP 0b: getPackageArchiveInfo with NO hooks installed yet
            try {
                @Suppress("DEPRECATION")
                val archInfo = packageManager.getPackageArchiveInfo(srcDir, PackageManager.GET_SIGNATURES)
                @Suppress("DEPRECATION")
                val realSigs = archInfo?.signatures
                Log.d("DualCore", "STEP0b archive sig count (pre-hook) = ${realSigs?.size ?: 0}")
            } catch (t: Throwable) {
                Log.e("DualCore", "STEP0b archive threw: ${t.javaClass.simpleName}: ${t.message}")
            }

            // STEP 0c: getPackageInfo(self) with NO hooks installed yet
            try {
                val pi = packageManager.getPackageInfo(packageName, 0)
                Log.d("DualCore", "STEP0c getPackageInfo(self) (pre-hook) OK v=${pi.versionCode}")
            } catch (t: Throwable) {
                Log.e("DualCore", "STEP0c getPackageInfo threw: ${t.javaClass.simpleName}: ${t.message}")
            }

            Log.d("DualCore", "=== STEP 0 done, installing hooks ===")

            // ============================================================
            // STEP 1 — Binder hook (test, then immediately unhook)
            // ============================================================
            Log.d("DualCore", "Binder.getCallingUid() BEFORE: ${Binder.getCallingUid()}")
            try {
                val m = Binder::class.java.getDeclaredMethod("getCallingUid")
                setVirtualUid(10000)
                hookBinderMethod(m)
                val guestView = Binder.getCallingUid()
                Log.d("DualCore", "Binder.getCallingUid() [hooked] = $guestView (expect 10000)")
                if (guestView == 10000) Log.d("DualCore", "SUCCESS: Binder hook works")

                // ---- UNHOOK to test whether the hook is the crash cause ----
                restoreBinderMethod()
                val restored = Binder.getCallingUid()
                Log.d("DualCore", "Binder.getCallingUid() [after restore] = $restored (expect 10421)")
                if (restored != 10000) Log.d("DualCore", "SUCCESS: Binder hook cleanly restored")
            } catch (e: Throwable) {
                Log.e("DualCore", "Binder hook test failed: $e")
            }

            // ============================================================
            // STEP 2 — Install native hooks
            // ============================================================
            try {
                val real = File(filesDir, "real").apply { mkdirs() }
                val fake = File(filesDir, "fake").apply { mkdirs() }
                File(real, "probe.txt").writeText("REAL")
                File(fake, "probe.txt").writeText("FAKE")
                addRedirectRule(real.absolutePath + "/", fake.absolutePath + "/")
                installNativeHooks()
                val got = File(real, "probe.txt").readText()
                Log.d("DualCore", "Redirect test: '$got' (expect FAKE)")
            } catch (e: Throwable) {
                Log.e("DualCore", "Redirect test failed: $e")
            }

            // ... keep existing property spoof, Process.myUid, inline hook,
            // Item1, Item2, Item5 test blocks unchanged ...

            // ============================================================
            // STEP 3 — Item 3 with the Binder hook STILL unhooked
            // If this works, we've proven the hook was the crash cause.
            // ============================================================
            try {
                Log.d("DualCore", "Item3: start")
                val srcDir = applicationInfo.sourceDir

                // Read via archive
                @Suppress("DEPRECATION")
                val archInfo = packageManager.getPackageArchiveInfo(srcDir, PackageManager.GET_SIGNATURES)
                @Suppress("DEPRECATION")
                val realSigs = archInfo?.signatures
                Log.d("DualCore", "Item3: archive sig count = ${realSigs?.size ?: 0}")

                if (realSigs != null && realSigs.isNotEmpty()) {
                    Log.d("DualCore", "SUCCESS: Item3 — signature readable")

                    // Confirm package manager works
                    val pi = packageManager.getPackageInfo(packageName, 0)
                    Log.d("DualCore", "Item3: getPackageInfo OK v=${pi.versionCode}")

                    // Install proxy
                    installPmSignatureSpoof()

                    // Verify
                    Log.d("DualCore", "Item3: test using PM hashCode=${packageManager.hashCode()}")
                    @Suppress("DEPRECATION")
                    val piSig = packageManager.getPackageInfo(packageName, PackageManager.GET_SIGNATURES)
                    @Suppress("DEPRECATION")
                    val sigAfter = piSig.signatures
                    if (sigAfter == null || sigAfter.isEmpty())
                        Log.d("DualCore", "SUCCESS: Item3 — signature spoofed (empty array)")
                    else
                        Log.e("DualCore", "FAILED: Item3 — sig count=${sigAfter.size}")
                } else {
                    Log.e("DualCore", "FAILED: Item3 — no signatures in archive")
                }
            } catch (t: Throwable) {
                Log.e("DualCore", "Item3 threw: ${t.javaClass.simpleName}: ${t.message}")
            }
        }.start()

        return START_NOT_STICKY
    }

    private fun installPmSignatureSpoof() {
        try {
            val pm = packageManager
            val pmCls = pm.javaClass
            Log.d("DualCore", "Item3: PM cls=${pmCls.name}")

            val iPmCls = Class.forName("android.content.pm.IPackageManager")

            // --- read original from instance field ---
            val instField = pmCls.getDeclaredField("mPM").apply { isAccessible = true }
            val realPm = instField.get(pm)
            if (realPm == null) { Log.e("DualCore", "Item3: mPM null"); return }

            // --- read original from static sPackageManager ---
            val atCls = Class.forName("android.app.ActivityThread")
            val staticField = atCls.getDeclaredField("sPackageManager").apply { isAccessible = true }
            val staticPm = staticField.get(null)
            Log.d("DualCore", "Item3: instance.mPM=${realPm.javaClass.simpleName} " +
                    "static.sPackageManager=${staticPm?.javaClass?.simpleName ?: "null"}")

            // --- ONE handler used for both proxies ---
            val handler = java.lang.reflect.InvocationHandler { _, method, args ->
                if (args != null && args.isNotEmpty() && args[0] is String) {
                    Log.d("DualCore", "PROXY: ${method.name}(${args[0]})")
                }
                val result = try {
                    if (args == null) method.invoke(realPm)
                    else method.invoke(realPm, *args)
                } catch (e: java.lang.reflect.InvocationTargetException) {
                    throw e.targetException
                }
                if (result is PackageInfo && args != null && args.isNotEmpty()) {
                    val askedPkg = args[0] as? String
                    if (askedPkg == packageName) {
                        Log.d("DualCore", "PROXY: nulling sigs for ${method.name}")
                        nullOutSignatures(result)
                    }
                }
                result
            }

            val proxy = Proxy.newProxyInstance(
                iPmCls.classLoader, arrayOf(iPmCls), handler)

            // Patch instance field
            instField.set(pm, proxy)
            Log.d("DualCore", "Item3: instance mPM -> ${instField.get(pm)?.javaClass?.name}")

            // Patch static field — this is the one MIUI actually uses
            try {
                staticField.set(null, proxy)
                Log.d("DualCore", "Item3: static sPackageManager -> ${staticField.get(null)?.javaClass?.name}")
            } catch (t: Throwable) {
                Log.e("DualCore", "Item3: sPackageManager swap failed: $t")
            }
        } catch (e: Throwable) {
            Log.e("DualCore", "Item3 install failed: $e")
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
            Log.d("DualCore", "PROXY: signatures -> empty")

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
                Log.d("DualCore", "PROXY: SigningDetails.signatures -> empty")
            } catch (t: Throwable) {
                Log.e("DualCore", "PROXY: signingInfo path failed: $t")
            }
        } catch (t: Throwable) {
            Log.e("DualCore", "PROXY: null-out failed: $t")
        }
    }
}