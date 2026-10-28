package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.lang.reflect.Modifier

/**
 * Session 60 — Feature 5.11 probe #2.
 *
 * Probe #1 (DebuggerDetectProbeTest) established:
 *   Debug.isDebuggerConnected  → FromReflectedMethod = 0x39  (fast-JNI token)
 *   Debug.waitingForDebugger   → FromReflectedMethod = 0x3b  (fast-JNI token)
 *   VMDebug.isDebuggerConnected exists as `native=true` but was never probed for
 *   its ArtMethod pointer. This file fills that gap.
 *
 * Diagnostic only. No assertions. Read logcat under tag "DbgDetectProbe2".
 *
 * The single measurement that decides 5.11's outcome:
 *
 *   VMDebug.isDebuggerConnected FromReflectedMethod  >= 0x1000  →  hookable
 *   VMDebug.isDebuggerConnected FromReflectedMethod   < 0x1000  →  walled
 *
 * Two full method dumps are emitted first so the exact available surface
 * on this MIUI build is captured in one place — the filtered dump from
 * probe #1 may have hidden relevant sibling methods (lastDebuggerActivity,
 * getDebugFlags, etc.).
 */
@RunWith(AndroidJUnit4::class)
class DebuggerDetectProbe2Test {

    private val TAG = "DbgDetectProbe2"

    private fun dumpFullMethods(clsName: String) {
        try {
            val c = Class.forName(clsName)
            Log.d(TAG, "=== FULL method list: $clsName (${c.declaredMethods.size} methods) ===")
            for (m in c.declaredMethods.sortedBy { it.name }) {
                val n = Modifier.isNative(m.modifiers)
                Log.d(TAG, "  ${m.name}  native=$n  " +
                        "mods='${Modifier.toString(m.modifiers)}'  " +
                        "ret=${m.returnType.simpleName}  " +
                        "params=[${m.parameterTypes.joinToString { it.simpleName }}]")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "Class.forName($clsName) threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    private fun probeCandidate(className: String, methodName: String) {
        Log.d(TAG, "=== probeCandidate $className.$methodName ===")
        val cls = try { Class.forName(className) }
        catch (t: Throwable) {
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
        Log.d(TAG, "  found: mods='${Modifier.toString(method.modifiers)}' isNative=$isNative " +
                "returnType=${method.returnType.simpleName}")
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

    @Test
    fun probeVMDebugHookability() {
        Log.d(TAG, "########## S60 Probe2 START ##########")

        // Full dumps first so we never have to guess at a method name again.
        dumpFullMethods("android.os.Debug")
        dumpFullMethods("dalvik.system.VMDebug")

        // The single decisive measurement.
        probeCandidate("dalvik.system.VMDebug", "isDebuggerConnected")

        // Context probes so the log shows what else exists at that layer.
        probeCandidate("android.os.Debug", "waitForDebugger")
        probeCandidate("android.os.Debug", "changeDebugPort")
        probeCandidate("android.os.Debug", "isDebuggerConnected")
        probeCandidate("android.os.Debug", "waitingForDebugger")

        Log.d(TAG, "########## S60 Probe2 END ##########")
    }

    @Test
    fun probeNativeSymbolResolution() {
        val candidates = listOf(
            "_ZN3art8VMDebug19isDebuggerConnectedEP7_JNIEnvP7_jclass",   // likely mangled form, exact mangling TBD
            "VMDebug_isDebuggerConnected",                                // unmangled fallback some builds export
        )
        for (sym in candidates) {
            val addr = NativeTestBridge.pltResolve("libart.so", sym)  // or a raw dynsym/symtab scan variant
            Log.d(TAG, "resolve('$sym') in libart.so = 0x${addr.toString(16)}")
        }
    }
}