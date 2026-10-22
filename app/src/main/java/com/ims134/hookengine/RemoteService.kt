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

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.d("DualCore", "Service started. (v11 — Item3 empty-sig)")

        Thread {
            // ===== Binder hook =====
            Log.d("DualCore", "Binder.getCallingUid() BEFORE: ${Binder.getCallingUid()}")
            try {
                val m = Binder::class.java.getDeclaredMethod("getCallingUid")
                setVirtualUid(10000)
                hookBinderMethod(m)
                val guestView = Binder.getCallingUid()
                Log.d("DualCore", "Binder.getCallingUid() [guest] = $guestView (expect 10000)")
                setInSystemCall(true)
                val systemView = Binder.getCallingUid()
                setInSystemCall(false)
                Log.d("DualCore", "Binder.getCallingUid() [system] = $systemView (expect 10411)")
                if (guestView == 10000 && systemView != 10000)
                    Log.d("DualCore", "SUCCESS: Binder hook — context-aware")
                else
                    Log.e("DualCore", "FAILED: guest=$guestView system=$systemView")
            } catch (e: Throwable) {
                Log.e("DualCore", "Binder hook failed: $e")
            }

            // ===== File redirect =====
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

            // ===== Property spoof =====
            try {
                val spClass = Class.forName("android.os.SystemProperties")
                val getMethod = spClass.getMethod("get", String::class.java)
                val debugGet = getMethod.invoke(null, "ro.debuggable") as? String
                Log.d("DualCore", "prop[ro.debuggable] (get) = '$debugGet' (expect 0)")
                val tagsGet = getMethod.invoke(null, "ro.build.tags") as? String
                Log.d("DualCore", "prop[ro.build.tags] (get) = '$tagsGet' (expect release-keys)")
            } catch (e: Throwable) {
                Log.e("DualCore", "property test failed: $e")
            }

            // ===== Native getuid =====
            try {
                Log.d("DualCore", "Process.myUid() = ${android.os.Process.myUid()} (expect 10000)")
            } catch (e: Throwable) {
                Log.e("DualCore", "uid test failed: $e")
            }

            // ===== Inline hook =====
            try {
                Log.d("DualCore", "inline hook test: starting")
                val before = nativeGetpid()
                val installed = inlineHookGetpid()
                val after = nativeGetpid()
                Log.d("DualCore", "inline getpid: installed=$installed before=$before after=$after")
                if (installed && after == 12345) Log.d("DualCore", "SUCCESS: inline hook fires")
                else                              Log.e("DualCore", "FAILED: inline hook did not fire")
            } catch (e: Throwable) {
                Log.e("DualCore", "inline hook test failed: $e")
            }

            // ===== Item 1 =====
            try {
                val spClass = Class.forName("android.os.SystemProperties")
                val nativeGet = spClass.getDeclaredMethod("native_get", String::class.java)
                nativeGet.isAccessible = true
                val spoofed = nativeGet.invoke(null, "ro.debuggable") as? String
                Log.d("DualCore", "Item1: native_get('ro.debuggable') = '$spoofed' (expect '0')")
                if (spoofed == "0") Log.d("DualCore", "SUCCESS: Item1 — greylist @hide, no bypass needed")
            } catch (e: Throwable) {
                Log.e("DualCore", "Item1 conclusion: $e")
            }

            // ===== Item 2 =====
            try {
                Log.d("DualCore", "Item2 test: starting")
                val realDir = File(filesDir, "real").apply { mkdirs() }
                val fakeDir = File(filesDir, "fake").apply { mkdirs() }
                addRedirectRule(realDir.absolutePath + "/", fakeDir.absolutePath + "/")
                File(fakeDir, "size_probe.txt").writeText("FAKEFAKEFAKE")
                File(fakeDir, "hide_me_dualhost_here.txt").writeText("x")
                File(fakeDir, "visible.txt").writeText("x")

                val len = File(realDir, "size_probe.txt").length()
                Log.d("DualCore", "Item2 statx: length() = $len (expect 12)")
                if (len == 12L) Log.d("DualCore", "SUCCESS: Item2 statx")
                else            Log.e("DualCore", "FAILED: Item2 statx — got $len")

                val entries: List<String> = realDir.listFiles()?.map { it.name }?.sorted() ?: emptyList()
                Log.d("DualCore", "Item2 readdir: entries = $entries")
                val leaked = entries.any { it.contains("dualhost") }
                val vis    = entries.contains("visible.txt")
                val size   = entries.contains("size_probe.txt")
                if (!leaked && vis && size) Log.d("DualCore", "SUCCESS: Item2 readdir")
                else                        Log.e("DualCore", "FAILED: Item2 readdir — leaked=$leaked vis=$vis size=$size")
            } catch (e: Throwable) {
                Log.e("DualCore", "Item2 test crashed: $e")
            }

            // ===== Item 5 =====
            try {
                Log.d("DualCore", "Item5 test: starting")
                val rootPaths = listOf(
                    "/system/bin/su", "/system/xbin/su", "/sbin/su",
                    "/system/app/Superuser.apk", "/data/adb/magisk"
                )
                var anyVisible = false
                for (p in rootPaths) {
                    val f = File(p)
                    val exists = f.exists()
                    val canExec = try { f.canExecute() } catch (t: Throwable) { false }
                    Log.d("DualCore", "Item5: exists('$p')=$exists canExecute=$canExec (expect false)")
                    if (exists || canExec) anyVisible = true
                }
                val shExists = File("/system/bin/sh").exists()
                Log.d("DualCore", "Item5: exists('/system/bin/sh')=$shExists (expect true)")
                if (!anyVisible && shExists) Log.d("DualCore", "SUCCESS: Item5 — root paths hidden, legit visible")
                else if (anyVisible)         Log.e("DualCore", "FAILED: Item5 — at least one root path visible")
                else                         Log.e("DualCore", "FAILED: Item5 — over-filtered")
            } catch (e: Throwable) {
                Log.e("DualCore", "Item5 test crashed: $e")
            }

            // ============================================================
            // Item 3 — signature spoof
            // ============================================================
            try {
                Log.d("DualCore", "Item3: start")

                // STEP 1: verify getPackageInfo(GET_SIGNATURES) works WITHOUT our proxy
                Log.d("DualCore", "Item3 STEP1: getPackageInfo(GET_SIGNATURES) — no proxy")
                var baselineOk = false
                setInSystemCall(true)
                try {
                    @Suppress("DEPRECATION")
                    val pi1 = packageManager.getPackageInfo(packageName, PackageManager.GET_SIGNATURES)
                    @Suppress("DEPRECATION")
                    val sig1 = pi1.signatures
                    Log.d("DualCore", "Item3 STEP1: OK, sig count=${sig1?.size ?: 0}")
                    baselineOk = true
                } catch (t: Throwable) {
                    Log.e("DualCore", "Item3 STEP1 FAILED (MIUI base getPackageInfo broken?): $t")
                } finally {
                    setInSystemCall(false)
                }

                if (!baselineOk) {
                    Log.e("DualCore", "Item3: cannot test spoof — MIUI base getPackageInfo(GET_SIGNATURES) crashes")
                } else {
                    // STEP 2: install proxy
                    Log.d("DualCore", "Item3 STEP2: installing proxy")
                    installPmSignatureSpoof()

                    // STEP 3: verify proxy changes the result
                    Log.d("DualCore", "Item3 STEP3: getPackageInfo(GET_SIGNATURES) — with proxy")
                    setInSystemCall(true)
                    try {
                        @Suppress("DEPRECATION")
                        val pi2 = packageManager.getPackageInfo(packageName, PackageManager.GET_SIGNATURES)
                        @Suppress("DEPRECATION")
                        val sig2 = pi2.signatures
                        if (sig2 == null || sig2.isEmpty()) {
                            Log.d("DualCore", "SUCCESS: Item3 — signature spoofed (empty array)")
                        } else {
                            Log.e("DualCore", "FAILED: Item3 — sig not nulled, count=${sig2.size}")
                        }
                    } catch (t: Throwable) {
                        Log.e("DualCore", "Item3 STEP3 FAILED (proxy path broken): $t")
                    } finally {
                        setInSystemCall(false)
                    }
                }
            } catch (e: Throwable) {
                Log.e("DualCore", "Item3 outer crash: $e")
            }
        }.start()

        return START_NOT_STICKY
    }

    // =============================================================
    // Signature spoof — empty-array approach
    //
    // Fake byte arrays fail DER deserialization on HyperOS. Instead
    // we set the signatures field to an empty array. This is a valid
    // state for PackageInfo and skips the DER parser entirely.
    //
    // Also: set TLS inside the proxy around the real Binder call.
    // ApplicationPackageManager can call back into the proxy from a
    // context where the Kotlin-side TLS flag has already been cleared.
    // =============================================================
    private fun installPmSignatureSpoof() {
        try {
            val pm = applicationContext.packageManager
            val pmCls = pm.javaClass

            val mPmField = pmCls.getDeclaredField("mPM")
            mPmField.isAccessible = true
            val realPm = mPmField.get(pm)
            if (realPm == null) { Log.e("DualCore", "Item3: mPM null"); return }
            Log.d("DualCore", "Item3: mPM = ${realPm.javaClass.name}")

            val iPmCls = Class.forName("android.content.pm.IPackageManager")
            val proxy = Proxy.newProxyInstance(
                iPmCls.classLoader, arrayOf(iPmCls)
            ) { _, method, args ->
                setInSystemCall(true)
                try {
                    val result = if (args == null) method.invoke(realPm)
                    else method.invoke(realPm, *args)

                    if (method.name == "getPackageInfo"
                        && result is PackageInfo
                        && args != null && args.isNotEmpty()
                        && (args[0] as? String) == packageName
                    ) {
                        try {
                            val sigCls = Class.forName("android.content.pm.Signature")
                            val empty = java.lang.reflect.Array.newInstance(sigCls, 0)
                            @Suppress("DEPRECATION")
                            val sigFld = PackageInfo::class.java.getDeclaredField("signatures")
                            sigFld.isAccessible = true
                            sigFld.set(result, empty)
                        } catch (t: Throwable) {
                            Log.e("DualCore", "Item3: sig null-out failed: $t")
                        }
                    }
                    result
                } catch (e: java.lang.reflect.InvocationTargetException) {
                    throw e.targetException
                } finally {
                    setInSystemCall(false)
                }
            }

            Log.d("DualCore", "Item3: swapping mPM")
            mPmField.set(pm, proxy)
            Log.d("DualCore", "Item3: mPM swapped OK")
        } catch (e: Throwable) {
            Log.e("DualCore", "Item3 install failed: $e")
        }
    }
}