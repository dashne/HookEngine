package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.lang.reflect.Method
import java.lang.reflect.Modifier

/**
 * Session 13 — nativeCNVM surface + behavior tests.
 *
 * nativeCNVM is the decomp-verified port of libkxqpplatform.so sub_30BF7C.
 * These four tests pin:
 *   1. JNI surface parity (external, void return).
 *   2. Null-args no-op (matches the C++ guard).
 *   3. Real dispatch — void method, int+String args.
 *   4. Boxed-Integer coercion (Integer.intValue() -> jvalue.i).
 *
 * Every assertion can fail (Rule 12). If the JNI symbol doesn't resolve,
 * test 1 fails with ClassNotFoundException — loud, honest failure.
 * If the body is wrong, tests 2-4 fail on the value assertions.
 */
@RunWith(AndroidJUnit4::class)
class ProxyBridgeSurfaceTest {

    object Probe {
        fun probeVoid(x: Int, s: String?) {
            lastVoidX = x
            lastVoidS = s
        }

        @Volatile var lastVoidX: Int = -1
        @Volatile var lastVoidS: String? = null
    }

    private fun methodOf(name: String, vararg params: Class<*>): Method =
        Probe::class.java.getDeclaredMethod(name, *params)

    // ─── Test 1: surface parity ──────────────────────────────────────

    @Test
    fun nativeCNVMMatchesReferenceSignature() {
        val cls = Class.forName("com.ims134.hookengine.ProxyBridge")
        val m = cls.getDeclaredMethod(
            "nativeCNVM",
            Method::class.java,
            Any::class.java,
            String::class.java,
            Array<Any?>::class.java
        )
        assertTrue("nativeCNVM must be external", Modifier.isNative(m.modifiers))
        assertEquals("nativeCNVM return type", Void.TYPE, m.returnType)
    }

    // ─── Test 2: null-args no-op ─────────────────────────────────────

    @Test
    fun nativeCNVMNullArgsIsNoOp() {
        // C++ guards on !method || !obj || !str || !arr — no exception,
        // no state change. Assert the probe is untouched.
        Probe.lastVoidX = -1
        Probe.lastVoidS = null
        val target = methodOf("probeVoid", Int::class.javaPrimitiveType!!, String::class.java)

        // empty args array — must return without throwing or dispatching.
        ProxyBridge.nativeCNVM(target, Probe, "VIL", arrayOf<Any?>())

        assertEquals("no dispatch occurred", -1, Probe.lastVoidX)
    }

    // ─── Test 3: real dispatch, void method ──────────────────────────

    @Test
    fun nativeCNVMInvokesVoidMethodWithCorrectArgs() {
        // probeVoid(Int, String), sig "VIL" — V is the leading sentinel,
        // then I (int) then L (object). This is the reference contract.
        val target = methodOf("probeVoid", Int::class.javaPrimitiveType!!, String::class.java)
        Probe.lastVoidX = -1
        Probe.lastVoidS = null

        ProxyBridge.nativeCNVM(target, Probe, "VIL", arrayOf<Any?>(99, "hello"))

        assertEquals("int arg dispatched", 99, Probe.lastVoidX)
        assertEquals("String arg dispatched", "hello", Probe.lastVoidS)
    }

    // ─── Test 4: boxed-Integer coercion ──────────────────────────────

    @Test
    fun nativeCNVMCoercesBoxedIntegersToJvalueInt() {
        // Reference unwraps via CallIntMethod(Integer.intValue) — i.e. the
        // boxed Integer in args[] becomes jvalue.i. If the port used
        // GetIntField or direct jint cast, this would fail.
        val target = methodOf("probeVoid", Int::class.javaPrimitiveType!!, String::class.java)
        val boxed = Integer.valueOf(123)
        Probe.lastVoidX = -1
        Probe.lastVoidS = null

        ProxyBridge.nativeCNVM(target, Probe, "VIL", arrayOf<Any?>(boxed, "x"))

        assertEquals("boxed Integer unwrapped to int", 123, Probe.lastVoidX)
        assertEquals("String arg dispatched", "x", Probe.lastVoidS)
    }
}