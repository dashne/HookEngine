package com.ims134.hookengine

import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.util.Log
import java.io.File

class RemoteService : Service() {
    init {
        System.loadLibrary("hookengine")
    }

    external fun hookBinderMethod(method: java.lang.reflect.Method)
    external fun installNativeHooks()
    external fun addRedirectRule(from: String, to: String)
    external fun setVirtualUid(uid: Int)
    external fun inlineHookGetpid(): Boolean
    external fun nativeGetpid(): Int
    external fun hiddenApiProbe(): Int
    external fun hiddenApiInstall(): Int

    external fun unhideMethod(method: java.lang.reflect.Method): Boolean
    external fun unhideField(field: java.lang.reflect.Field): Boolean

    override fun onBind(intent: Intent?): IBinder? = null

    external fun setHiddenApiExemptionsNative(): Boolean

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Log.d("DualCore", "Service started. (v4 — with HiddenApi test)")

        Thread {
            // Binder hook
            Log.d("DualCore", "Binder.getCallingUid() BEFORE: ${Binder.getCallingUid()}")
            try {
                val m = Binder::class.java.getDeclaredMethod("getCallingUid")
                setVirtualUid(10000)
                hookBinderMethod(m)
                val after = Binder.getCallingUid()
                Log.d("DualCore", "Binder.getCallingUid() AFTER: $after")
                if (after == 10000) Log.d("DualCore", "SUCCESS: Binder hook works")
            } catch (e: Throwable) {
                Log.e("DualCore", "Binder hook failed: $e")
            }

            // File redirect
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

            // Property spoof
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

            // Native getuid
            try {
                Log.d("DualCore", "Process.myUid() = ${android.os.Process.myUid()} (expect 10000)")
            } catch (e: Throwable) {
                Log.e("DualCore", "uid test failed: $e")
            }

            // Inline hook
            try {
                Log.d("DualCore", "inline hook test: starting")
                val before = nativeGetpid()
                val installed = inlineHookGetpid()
                val after = nativeGetpid()
                Log.d("DualCore", "inline getpid: installed=$installed before=$before after=$after")
                if (installed && after == 12345) Log.d(
                    "DualCore", "SUCCESS: inline hook fires (getpid hooked)"
                )
                else Log.e("DualCore", "FAILED: inline hook did not fire (after=$after)")
            } catch (e: Throwable) {
                Log.e("DualCore", "inline hook test failed: $e")
            }

            // ============================================================
// Item 1 conclusion: hidden API does NOT block greylist @hide methods.
// Reflection works out of the box for SystemProperties.native_get.
// No unhide patch needed. Prove it once and record it.
// ============================================================
            try {
                Log.d("DualCore", "Item1 conclusion test: starting")

                val spClass = Class.forName("android.os.SystemProperties")

                // Greylist @hide — should work without any bypass
                val nativeGet = spClass.getDeclaredMethod("native_get", String::class.java)
                nativeGet.isAccessible = true
                val spoofed = nativeGet.invoke(null, "ro.debuggable") as? String
                Log.d(
                    "DualCore",
                    "greylist @hide invoke: native_get('ro.debuggable') = '$spoofed' (expect '0')"
                )

                // Bulk lookup works
                val bulk = spClass.declaredMethods.firstOrNull { it.name == "native_get" }
                Log.d("DualCore", "bulk lookup: found=${bulk != null}")

                if (spoofed == "0") {
                    Log.d(
                        "DualCore", "SUCCESS: Item1 — greylist @hide accessible, no bypass required"
                    )
                }
            } catch (e: Throwable) {
                Log.e("DualCore", "Item1 conclusion test failed: $e")
            }

            // ============================================================
// Item 1 conclusion — recorded. Greylist @hide works, no bypass needed.
// ============================================================
            try {
                val spClass = Class.forName("android.os.SystemProperties")
                val nativeGet = spClass.getDeclaredMethod("native_get", String::class.java)
                nativeGet.isAccessible = true
                val spoofed = nativeGet.invoke(null, "ro.debuggable") as? String
                Log.d("DualCore", "Item1: native_get('ro.debuggable') = '$spoofed' (expect '0')")
            } catch (e: Throwable) {
                Log.e("DualCore", "Item1 conclusion: $e")
            }

// ============================================================
// Item 2 test — statx path redirect + readdir filtering
// ============================================================
            try {
                Log.d("DualCore", "Item2 test: starting")

                val realDir = File(filesDir, "real").apply { mkdirs() }
                val fakeDir = File(filesDir, "fake").apply { mkdirs() }

                addRedirectRule(realDir.absolutePath + "/", fakeDir.absolutePath + "/")

                // Size probe: 12 bytes in fake. Real version does not exist — if stat
                // redirect works, File(real/x).length() reads fake and returns 12.
                // Without redirect, getLength catches ENOENT and returns 0.
                File(fakeDir, "size_probe.txt").writeText("FAKEFAKEFAKE")

                // Readdir probe: drop these into fake/ and list real/. If opendir redirects,
                // listFiles sees fake/ contents (with hide_me_* filtered out).
                File(fakeDir, "hide_me_dualhost_here.txt").writeText("x")
                File(fakeDir, "visible.txt").writeText("x")

                val len = File(realDir, "size_probe.txt").length()
                Log.d("DualCore", "Item2 statx: length() = $len (expect 12)")
                if (len == 12L) Log.d("DualCore", "SUCCESS: Item2 statx")
                else            Log.e("DualCore", "FAILED: Item2 statx — got $len")

                val entries: List<String> = realDir.listFiles()?.map { it.name }?.sorted() ?: emptyList()
                Log.d("DualCore", "Item2 readdir: entries = $entries (expect size_probe.txt + visible.txt)")
                val leaked  = entries.any { it.contains("dualhost") }
                val vis     = entries.contains("visible.txt")
                val size    = entries.contains("size_probe.txt")
                if (!leaked && vis && size) Log.d("DualCore", "SUCCESS: Item2 readdir")
                else                        Log.e("DualCore", "FAILED: Item2 readdir — leaked=$leaked vis=$vis size=$size")
            } catch (e: Throwable) {
                Log.e("DualCore", "Item2 test crashed: $e")
            }

        }.start()

        return START_NOT_STICKY
    }
}