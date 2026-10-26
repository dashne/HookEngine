package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * Session 44 — Feature 1.4: ArtMethod entry/exit trace.
 *
 * Targets are framework native methods (Binder.getCallingUid, Binder.getCallingPid).
 * These are public static native int() methods with real ArtMethod pointers
 * (data_ = compiled fast stub in libart.so), so JavaHookInstall accepts them.
 *
 * App-defined native methods (external fun in a Kotlin object) that were
 * resolved via dlsym — not RegisterNatives — return a fast-JNI dispatch
 * token from FromReflectedMethod (< 0x1000) rather than a real ArtMethod
 * pointer. JavaHookInstall correctly refuses those. See ArtMethodTrace.h.
 *
 * Static methods get (JNIEnv*, jclass) at the ABI level, our wrapper is
 * (JNIEnv*, jobject); jclass and jobject are both void* so this is ABI
 * compatible and the wrapper's pass-through of `thiz` to `_orig` works.
 *
 * HookResult codes from HookRegistry.h:
 *   HOOK_OK              = 0
 *   HOOK_ALREADY_HOOKED  = 8
 *   HOOK_NOT_HOOKED      = 9
 */
@RunWith(AndroidJUnit4::class)
class ArtMethodTraceTest {

    private val HOOK_OK = 0
    private val HOOK_ALREADY_HOOKED = 8
    private val HOOK_NOT_HOOKED = 9

    private fun uidMethod(): java.lang.reflect.Method =
        android.os.Binder::class.java.getDeclaredMethod("getCallingUid")

    private fun pidMethod(): java.lang.reflect.Method =
        android.os.Binder::class.java.getDeclaredMethod("getCallingPid")

    @Before
    fun reset() {
        NativeTestBridge.uninstallAll()
        NativeTestBridge.traceResetCounters()
    }

    @After
    fun cleanup() = NativeTestBridge.uninstallAll()

    @Test
    fun traceBasicFireAndRestore() {
        val m = uidMethod()

        val realUid = android.os.Binder.getCallingUid()
        assertEquals("no entries yet", 0, NativeTestBridge.traceEntryCount())
        assertEquals("no exits yet", 0, NativeTestBridge.traceExitCount())

        val rc = NativeTestBridge.traceAttach0(m)
        assertEquals("attach", HOOK_OK, rc)
        assertEquals("1 active trace", 1, NativeTestBridge.traceCount())

        val ret = android.os.Binder.getCallingUid()
        assertEquals("return preserved through trace", realUid, ret)
        assertEquals("entry fired once", 1, NativeTestBridge.traceEntryCount())
        assertEquals("exit fired once", 1, NativeTestBridge.traceExitCount())
        assertEquals("exit saw real return", realUid, NativeTestBridge.traceLastRet())

        val drc = NativeTestBridge.traceDetach(m)
        assertEquals("detach", HOOK_OK, drc)
        assertEquals("0 active traces", 0, NativeTestBridge.traceCount())

        android.os.Binder.getCallingUid()
        assertEquals("entry count unchanged after detach",
            1, NativeTestBridge.traceEntryCount())
        assertEquals("exit count unchanged after detach",
            1, NativeTestBridge.traceExitCount())
    }

    @Test
    fun traceMultipleCallsEachFires() {
        val m = uidMethod()
        assertEquals("attach", HOOK_OK, NativeTestBridge.traceAttach0(m))

        repeat(5) { android.os.Binder.getCallingUid() }
        assertEquals("5 entries", 5, NativeTestBridge.traceEntryCount())
        assertEquals("5 exits", 5, NativeTestBridge.traceExitCount())
    }

    @Test
    fun traceDuplicateAttachRefused() {
        val m = uidMethod()
        assertEquals("first attach", HOOK_OK, NativeTestBridge.traceAttach0(m))
        val rc = NativeTestBridge.traceAttach0(m)
        assertEquals("duplicate refused", HOOK_ALREADY_HOOKED, rc)
        assertEquals("still 1 trace", 1, NativeTestBridge.traceCount())
    }

    @Test
    fun traceDetachNonTracedReturnsNotHooked() {
        val m = uidMethod()
        val rc = NativeTestBridge.traceDetach(m)
        assertEquals("detach on empty", HOOK_NOT_HOOKED, rc)
    }

    @Test
    fun traceTwoMethodsIndependent() {
        val mu = uidMethod()
        val mp = pidMethod()
        assertEquals("attach uid", HOOK_OK, NativeTestBridge.traceAttach0(mu))
        assertEquals("attach pid", HOOK_OK, NativeTestBridge.traceAttach0(mp))
        assertEquals("2 traces", 2, NativeTestBridge.traceCount())

        android.os.Binder.getCallingUid()
        assertEquals("uid entry fired", 1, NativeTestBridge.traceEntryCount())

        android.os.Binder.getCallingPid()
        assertEquals("pid entry fired", 2, NativeTestBridge.traceEntryCount())

        NativeTestBridge.traceDetach(mu)
        assertEquals("detach uid", 1, NativeTestBridge.traceCount())

        android.os.Binder.getCallingUid()
        assertEquals("uid entry unchanged after detach",
            2, NativeTestBridge.traceEntryCount())

        android.os.Binder.getCallingPid()
        assertEquals("pid still fires", 3, NativeTestBridge.traceEntryCount())
    }
}