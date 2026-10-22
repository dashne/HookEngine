package com.ims134.hookengine

import android.app.Service
import android.content.Intent
import android.os.Binder
import android.os.IBinder
import android.util.Log
import java.io.File

class RemoteService : Service() {
    init { System.loadLibrary("hookengine") }

    external fun hookBinderMethod(method: java.lang.reflect.Method)
    external fun installNativeHooks()
    external fun addRedirectRule(from: String, to: String)
    external fun setVirtualUid(uid: Int)
    external fun inlineHookGetpid(): Boolean
    external fun nativeGetpid(): Int
    external fun hiddenApiProbe(): Int
    external fun hiddenApiInstall(): Int

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
                if (installed && after == 12345) Log.d("DualCore", "SUCCESS: inline hook fires (getpid hooked)")
                else Log.e("DualCore", "FAILED: inline hook did not fire (after=$after)")
            } catch (e: Throwable) {
                Log.e("DualCore", "inline hook test failed: $e")
            }

            // HiddenApi
            try {
                Log.d("DualCore", "HiddenApi test: starting")
                val exempt = setHiddenApiExemptionsNative()
                Log.d("DualCore", "setHiddenApiExemptionsNative: $exempt")

                if (exempt) {
                    try {
                        val spClass = Class.forName("android.os.SystemProperties")
                        val findMethod = spClass.getDeclaredMethod("find", String::class.java)
                        findMethod.isAccessible = true
                        Log.d("DualCore", "SUCCESS: hidden API bypass — got ${findMethod.name}")
                    } catch (e: Throwable) {
                        Log.e("DualCore", "FAILED: exempt set but reflection still blocked: $e")
                    }
                } else {
                    Log.e("DualCore", "FAILED: setHiddenApiExemptions JNI call failed")
                }
            } catch (e: Throwable) {
                Log.e("DualCore", "HiddenApi test crashed: $e")
            }

            // HiddenApi
            try {
                Log.d("DualCore", "HiddenApi test: starting")
                val probeCount = hiddenApiProbe()
                Log.d("DualCore", "HiddenApi probe found $probeCount symbol(s)")
                val installed = hiddenApiInstall()
                Log.d("DualCore", "HiddenApi install result: $installed")
                if (installed == 1) {
                    try {
                        val spClass = Class.forName("android.os.SystemProperties")
                        val findMethod = spClass.getDeclaredMethod("find", String::class.java)
                        findMethod.isAccessible = true
                        Log.d("DualCore", "SUCCESS: hidden API bypass — got ${findMethod.name}")
                    } catch (e: Throwable) {
                        Log.e("DualCore", "FAILED: bypass installed but reflection still blocked: $e")
                    }
                } else {
                    Log.e("DualCore", "FAILED: no enforcement function found to hook")
                }
            } catch (e: Throwable) {
                Log.e("DualCore", "HiddenApi test crashed: $e")
            }

            // Diagnostic: is `find` really blocked, or does it just not exist?
            try {
                val spClass = Class.forName("android.os.SystemProperties")
                val allMethods = spClass.declaredMethods
                val allNames = allMethods.map { it.name }.sorted()
                Log.d("DualCore", "SystemProperties declaredMethods count: ${allMethods.size}")
                Log.d("DualCore", "SystemProperties method names: ${allNames.joinToString(",")}")

                val findMethod = allMethods.firstOrNull { it.name == "find" && it.parameterTypes.size == 1 }
                Log.d("DualCore", "find method present in declaredMethods: ${findMethod != null}")

                if (findMethod != null) {
                    // Method object exists. Now try invoking it directly — this bypasses
                    // the getDeclaredMethod(name) lookup path entirely.
                    try {
                        findMethod.isAccessible = true
                        val r = findMethod.invoke(null, "ro.debuggable")
                        Log.d("DualCore", "DIRECT INVOKE SUCCESS: find(\"ro.debuggable\") = $r")
                    } catch (e: Throwable) {
                        Log.e("DualCore", "DIRECT INVOKE BLOCKED: $e")
                    }
                }
            } catch (e: Throwable) {
                Log.e("DualCore", "declaredMethods diagnostic failed: $e")
            }


        }.start()

        return START_NOT_STICKY
    }
}