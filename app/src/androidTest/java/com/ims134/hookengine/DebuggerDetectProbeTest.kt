package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.lang.reflect.Modifier
import java.net.InetSocketAddress
import java.net.Socket

/**
 * Session 60 — Feature 5.11 debugger detect probe.
 *
 * Diagnostic only. No assertions. Rule 12 does not apply to probes — the
 * purpose is to measure, not to gate. All output goes to logcat under
 * tag "DbgDetectProbe".
 *
 * The five questions this probe answers, in order:
 *
 *   Q1. Is android.os.Debug.isDebuggerConnected() a native JNI method,
 *       or a managed Java wrapper that calls into VMDebug?
 *       If NATIVE  → JavaHookInstall can hook it directly (Feature 1.4 class).
 *       If MANAGED → JavaHookInstall would hit the S58 JIT-ABI wall; do not
 *                    hook the wrapper, hook the VMDebug target instead.
 *
 *   Q2. Same for android.os.Debug.waitingForDebugger().
 *
 *   Q3. Is dalvik.system.VMDebug.getIsDebuggerConnected() native?
 *       This is the actual implementation the wrapper delegates to. If
 *       native, it is the hook target when the wrapper is managed.
 *
 *   Q4. Same for dalvik.system.VMDebug.waitingForDebugger() (actual name
 *       may differ — the full method enumeration at the top of the probe
 *       will list the real name).
 *
 *   Q5. Baseline current state:
 *       - Debug.isDebuggerConnected() / waitingForDebugger() live values
 *       - VMDebug equivalents live values
 *       - ro.debuggable / ro.adb.secure / init.svc.adbd / service.adb.root
 *         (S59 spoof surface — verify it still fires)
 *       - JDWP port 8700..8705 connect attempt (expect ECONNREFUSED)
 *       - Debug.getNativeHeapSize() sanity (unrelated debug API passthrough)
 *
 * All five questions in one test invocation. Read logcat, then decide
 * the hook mechanism.
 */
@RunWith(AndroidJUnit4::class)
class DebuggerDetectProbeTest {

    private val TAG = "DbgDetectProbe"

    /**
     * Enumerate every "debug" / "wait" / "JDWP"-shaped method on a class so
     * the probe output shows the exact names available on this ROM, not the
     * AOSP names we assume. MIUI has a habit of renaming.
     */
    private fun dumpClass(clsName: String) {
        try {
            val c = Class.forName(clsName)
            Log.d(TAG, "=== all methods on $clsName (filtered to ebug/wait/JDWP) ===")
            for (m in c.declaredMethods) {
                val hitsEbug = m.name.contains("ebug", ignoreCase = true)
                val hitsWait = m.name.contains("ait", ignoreCase = true)
                val hitsJdwp = m.name.contains("JDWP", ignoreCase = true)
                if (!(hitsEbug || hitsWait || hitsJdwp)) continue
                val isNative = Modifier.isNative(m.modifiers)
                Log.d(TAG, "  ${m.name}  native=$isNative  " +
                        "mods='${Modifier.toString(m.modifiers)}'  " +
                        "ret=${m.returnType.simpleName}  " +
                        "params=${m.parameterTypes.joinToString { it.simpleName }}")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  Class.forName($clsName) threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    private fun probeCandidate(className: String, methodName: String) {
        Log.d(TAG, "=== probeCandidate $className.$methodName ===")
        val cls = try {
            Class.forName(className)
        } catch (t: Throwable) {
            Log.d(TAG, "  Class.forName threw: ${t.javaClass.simpleName}: ${t.message}")
            return
        }
        val method = try {
            val m = cls.getDeclaredMethod(methodName)
            m.isAccessible = true
            m
        } catch (t: Throwable) {
            Log.d(TAG, "  getDeclaredMethod threw: ${t.javaClass.simpleName}: ${t.message}")
            return
        }
        val isNative = Modifier.isNative(method.modifiers)
        Log.d(TAG, "  found: mods='${Modifier.toString(method.modifiers)}' " +
                "isNative=$isNative returnType=${method.returnType.simpleName}")
        val mid = try {
            NativeTestBridge.probeFromReflectedMethod(method)
        } catch (t: Throwable) {
            Log.d(TAG, "  probeFromReflectedMethod threw: ${t.javaClass.simpleName}: ${t.message}")
            0L
        }
        Log.d(TAG, "  FromReflectedMethod = 0x${mid.toString(16)} (decimal $mid)")
        when {
            mid >= 0x1000L -> Log.d(TAG, "  ✅ HOOKABLE via JavaHookInstall (real ArtMethod*)")
            mid > 0L       -> Log.d(TAG, "  ❌ fast-JNI token, JavaHookInstall will refuse")
            else           -> Log.d(TAG, "  ❌ null jmethodID / reflection failed")
        }
    }

    private fun probeCurrentState() {
        Log.d(TAG, "=== current debug state (live values) ===")
        try {
            val debugCls = Class.forName("android.os.Debug")
            val m1 = debugCls.getDeclaredMethod("isDebuggerConnected").apply { isAccessible = true }
            Log.d(TAG, "  Debug.isDebuggerConnected() = ${m1.invoke(null) as Boolean}")
            val m2 = debugCls.getDeclaredMethod("waitingForDebugger").apply { isAccessible = true }
            Log.d(TAG, "  Debug.waitingForDebugger() = ${m2.invoke(null) as Boolean}")
        } catch (t: Throwable) {
            Log.d(TAG, "  Debug state probe threw: ${t.javaClass.simpleName}: ${t.message}")
        }
        try {
            val vmCls = Class.forName("dalvik.system.VMDebug")
            val m1 = vmCls.getDeclaredMethod("getIsDebuggerConnected").apply { isAccessible = true }
            Log.d(TAG, "  VMDebug.getIsDebuggerConnected() = ${m1.invoke(null) as Boolean}")
            val m2 = vmCls.getDeclaredMethod("waitingForDebugger").apply { isAccessible = true }
            Log.d(TAG, "  VMDebug.waitingForDebugger() = ${m2.invoke(null) as Boolean}")
        } catch (t: Throwable) {
            Log.d(TAG, "  VMDebug state probe threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    private fun probePropertySurface() {
        Log.d(TAG, "=== property surface (S59 spoof — should already be live) ===")
        for (key in listOf(
            "ro.debuggable",
            "ro.adb.secure",
            "init.svc.adbd",
            "service.adb.root",
            "ro.build.version.sdk"    // non-spoofed control
        )) {
            try {
                val spCls = Class.forName("android.os.SystemProperties")
                val getM = spCls.getMethod("get", String::class.java)
                val v = getM.invoke(null, key) as? String ?: "<null>"
                Log.d(TAG, "  SystemProperties[$key] = '$v'")
            } catch (t: Throwable) {
                Log.d(TAG, "  SystemProperties[$key] threw: ${t.javaClass.simpleName}: ${t.message}")
            }
        }
    }

    private fun probeJdwpPort() {
        Log.d(TAG, "=== JDWP port probe (127.0.0.1:8700..8705) ===")
        for (port in 8700..8705) {
            try {
                Socket().use { s ->
                    s.connect(InetSocketAddress("127.0.0.1", port), 300)
                    Log.d(TAG, "  port $port CONNECTED (unexpected — no debugger expected)")
                }
            } catch (t: Throwable) {
                Log.d(TAG, "  port $port refused: ${t.javaClass.simpleName}: ${t.message}")
            }
        }
    }

    private fun probeSanity() {
        Log.d(TAG, "=== unrelated debug API sanity (must still work) ===")
        try {
            val heap = android.os.Debug.getNativeHeapSize()
            Log.d(TAG, "  Debug.getNativeHeapSize() = $heap")
            Log.d(TAG, if (heap > 0L) "  ✅ unrelated debug API works" else "  ❌ returned 0")
        } catch (t: Throwable) {
            Log.d(TAG, "  Debug.getNativeHeapSize() threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    @Test
    fun probeDebugSurface() {
        Log.d(TAG, "########## S60 DebuggerDetectProbeTest START ##########")

        dumpClass("android.os.Debug")
        dumpClass("dalvik.system.VMDebug")

        probeCandidate("android.os.Debug", "isDebuggerConnected")
        probeCandidate("android.os.Debug", "waitingForDebugger")
        probeCandidate("dalvik.system.VMDebug", "getIsDebuggerConnected")
        probeCandidate("dalvik.system.VMDebug", "waitingForDebugger")

        probeCurrentState()
        probePropertySurface()
        probeJdwpPort()
        probeSanity()

        Log.d(TAG, "########## S60 DebuggerDetectProbeTest END ##########")
    }
}