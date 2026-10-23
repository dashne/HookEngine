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

    external fun hookBinderMethod(method: java.lang.reflect.Method):Int
    external fun installNativeHooks(): Int
    external fun addRedirectRule(from: String, to: String)
    external fun setVirtualUid(uid: Int)
    external fun setInSystemCall(on: Boolean)
    external fun inlineHookGetpid(): Boolean
    external fun nativeGetpid(): Int
    external fun hiddenApiProbe(): Int
    external fun hiddenApiInstall(): Int
    external fun installProcessHooks(): Int
    external fun uninstallProcessHooks(): Int

    external fun runItem1Tests(): String
    external fun runItem2Tests(): String
    external fun runItem3Tests(): String
    external fun runItem4Tests(realDir: String, fakeDir: String): String
    external fun runItem5Tests(getCallingUidMethod: java.lang.reflect.Method): String

    external fun capabilityCheck(): Int
    external fun hookCount(): Int

    external fun restoreBinderMethod()
    external fun diagRawZip(path: String): String

    external fun writeSelftestFile(path: String): Int

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.d("DualCore", "Service started. (v29)")
        Log.d("DualCore","==============================================------------Start---------=============================================");
        try {
            Log.d("DualCore", runItem1Tests())
        } catch (t: Throwable) {
            Log.e("DualCore", "runItem1Tests threw: $t")
        }
        try {
            Log.d("DualCore", runItem2Tests())
        } catch (t: Throwable) {
            Log.e("DualCore", "runItem2Tests threw: $t")
        }
        try {
            Log.d("DualCore", runItem3Tests())
        } catch (t: Throwable) {
            Log.e("DualCore", "runItem3Tests threw: $t")
        }
        try {
            val r4Real = File(filesDir, "real").apply { mkdirs() }
            val r4Fake = File(filesDir, "fake").apply { mkdirs() }
            Log.d("DualCore", runItem4Tests(r4Real.absolutePath, r4Fake.absolutePath))
        } catch (t: Throwable) {
            Log.e("DualCore", "runItem4Tests threw: $t")
        }

        try {
            val m = Binder::class.java.getDeclaredMethod("getCallingUid")
            Log.d("DualCore", runItem5Tests(m))
        } catch (t: Throwable) {
            Log.e("DualCore", "runItem5Tests threw: $t")
        }

        Log.d("DualCore","==============================================------------End----------=============================================");

        try {
            val selftest = File(filesDir, "selftest.txt")
            val pass = writeSelftestFile(selftest.absolutePath)
            Log.d("DualCore", "selftest.txt written: $pass pass")
        } catch (t: Throwable) {
            Log.e("DualCore", "writeSelftestFile threw: $t")
        }



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
                val cap = capabilityCheck()
                if (cap != 0) {
                    Log.e("DualCore", "HOSTSIG: capability check failed, code=$cap")
                } else {
                    val m = Binder::class.java.getDeclaredMethod("getCallingUid")
                    setVirtualUid(10000)
                    val r = hookBinderMethod(m)
                    if (r != 0) {
                        Log.e("DualCore", "HOSTSIG: hookBinderMethod failed, code=$r")
                    } else {
                        val guestView = Binder.getCallingUid()
                        Log.d("DualCore", "Binder.getCallingUid() [hooked] = $guestView (expect 10000)")
                        if (guestView == 10000) Log.d("DualCore", "SUCCESS: Binder hook works")
                    }
                    restoreBinderMethod()
                    val restored = Binder.getCallingUid()
                    Log.d("DualCore", "Binder.getCallingUid() [after restore] = $restored (expect 10421)")
                    if (restored != 10000) Log.d("DualCore", "SUCCESS: Binder hook cleanly restored")
                }
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
                val rc = installNativeHooks()
                if (rc != 0) {
                    Log.e("DualCore", "InstallHooks failed, code=$rc (rolled back)")
                } else {
                    Log.d("DualCore", "HookRegistry: ${hookCount()} hooks installed")
                    val got = File(real, "probe.txt").readText()
                    Log.d("DualCore", "Redirect test: '$got' (expect FAKE)")
                }
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
        PmSpoof.install(this)
    }
}