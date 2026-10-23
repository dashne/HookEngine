package com.ims134.hookengine

import android.os.IBinder
import android.os.Parcel
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.lang.reflect.Method
import java.lang.reflect.Modifier

@RunWith(AndroidJUnit4::class)
class BinderTransactProbeTest {

    private val transactParamTypes: Array<Class<*>> = arrayOf<Class<*>>(
        Integer.TYPE,
        Parcel::class.java,
        Parcel::class.java,
        Integer.TYPE,
    )

    @Test
    fun probeBinderProxyTransact() {
        Log.d("BTProbe", "=== BinderProxy probe (extended) ===")

        val cls = try {
            Class.forName("android.os.BinderProxy")
        } catch (t: Throwable) {
            Log.d("BTProbe", "BinderProxy class load failed: $t")
            return
        }

        // ── Step 1: enumerate all native methods on BinderProxy ──
        Log.d("BTProbe", "--- enumerating BinderProxy methods ---")
        val allMethods: Array<Method> = try {
            cls.declaredMethods
        } catch (t: Throwable) {
            Log.d("BTProbe", "declaredMethods failed: $t")
            emptyArray()
        }
        if (allMethods.isEmpty()) {
            Log.d("BTProbe", "declaredMethods returned empty — trying HiddenApiBridge? " +
                    "(BinderProxy is public so this shouldn't happen)")
        }
        var nativeCount = 0
        for (m in allMethods) {
            if (Modifier.isNative(m.modifiers)) {
                nativeCount++
                val params = m.parameterTypes.joinToString { it.simpleName }
                Log.d("BTProbe", "  NATIVE: ${m.returnType.simpleName} ${m.name}($params)")
            }
        }
        Log.d("BTProbe", "--- $nativeCount native methods found ---")

        // ── Step 2: probe specific candidates ──
        val candidates = listOf("transactNative", "transact")
        for (name in candidates) {
            Log.d("BTProbe", "--- candidate: $name ---")
            val method = try {
                cls.getDeclaredMethod(name, *transactParamTypes)
            } catch (_: NoSuchMethodException) {
                try {
                    HiddenApiBridge.reflectGetDeclaredMethod(cls, name, transactParamTypes)
                } catch (t: Throwable) {
                    Log.d("BTProbe", "  not found (${t.javaClass.simpleName})")
                    null
                }
            }
            if (method == null) { Log.d("BTProbe", "  null"); continue }

            val isNative = Modifier.isNative(method.modifiers)
            Log.d("BTProbe", "  found: ${Modifier.toString(method.modifiers)}")
            Log.d("BTProbe", "  isNative: $isNative")
            if (!isNative) {
                Log.d("BTProbe", "  → cannot JavaHook this candidate")
                continue
            }
            val mid = try {
                NativeTestBridge.probeFromReflectedMethod(method)
            } catch (t: Throwable) {
                Log.d("BTProbe", "  probeFromReflectedMethod threw: $t")
                0L
            }
            Log.d("BTProbe", "  FromReflectedMethod: 0x${mid.toString(16)}")
            if (mid >= 0x1000) {
                Log.d("BTProbe", "  ✅ HOOKABLE via JavaHookInstall (real ArtMethod*)")
            } else if (mid > 0) {
                Log.d("BTProbe", "  ❌ fast-JNI token, JavaHookInstall will refuse")
            } else {
                Log.d("BTProbe", "  ❌ null jmethodID")
            }
        }

        // ── Step 3: fallback decision summary ──
        Log.d("BTProbe", "=== end probe ===")
        Log.d("BTProbe", "If any candidate shows HOOKABLE → next message writes FakeBinderTransact (JavaHook)")
        Log.d("BTProbe", "If none HOOKABLE → next message writes IPCThreadState::transact (C++ hook)")
    }
}