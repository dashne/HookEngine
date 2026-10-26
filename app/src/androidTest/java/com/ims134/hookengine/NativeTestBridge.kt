package com.ims134.hookengine

object NativeTestBridge {
    init { System.loadLibrary("hookengine") }
    external fun runSuite(suite: String): Array<String>
    external fun uninstallAll()
    external fun setItem4Paths(realDir: String, fakeDir: String)
    external fun spoofProcessStatics(uid: Int, pid: Int): Int
    external fun restoreProcessStatics()

    external fun installProcessHooks(): Int
    external fun uninstallProcessHooks()
    external fun installFileHooks(): Int
    external fun installAntiDetection(): Int
    external fun writeFileRaw(path: String, content: String): Boolean
    external fun readFileRaw(path: String): String
    external fun deleteFileRaw(path: String): Boolean
    external fun binderProbe(): Int

    external fun probeFromReflectedMethod(method: java.lang.reflect.Method): Long

    external fun parcelRewriterReady(): Boolean

    external fun dumpLastCode36Reply(): ByteArray?

    // ─── Feature 1.5 — dynamic runtime attach ───────────────────────
    external fun attachRuntime(addr: Long, repl: Long): Int
    external fun detachRuntime(addr: Long): Int
    external fun getFnAttachTargetAddr(): Long
    external fun getFakeAttachAddr(): Long
    external fun getTestDataVarAddr(): Long
    external fun callFnAttachTarget(x: Int): Int
    external fun readU32At(addr: Long): Long

    // ─── Feature 1.7 — hook chaining ────────────────────────────────
    external fun chainAttach(target: Long, handler: Long): Int
    external fun chainDetach(target: Long, handler: Long): Int
    external fun chainCount(): Int
    external fun chainHandlerCount(target: Long): Int
    external fun getChainHandler1Addr(): Long
    external fun getChainHandler2Addr(): Long
    external fun chainCalls1(): Int
    external fun chainCalls2(): Int
    external fun chainLastX0_1(): Int
    external fun chainLastX0_2(): Int
    external fun chainOrderFirst(): Int
    external fun chainReset()

    // ─── Feature 1.4 — ArtMethod entry/exit trace ───────────────────
    //
    // arity parameter selects the wrapper JNI signature:
    //   0 = int foo()          → traceAttach0
    //   1 = int foo(int)       → traceAttach1
    //   2 = int foo(String)    → traceAttach2
    //   3 = int foo(String,int)→ traceAttach3
    //
    // The fixture methods deliberately take a String to avoid ART's
    // fast-JNI optimization (which rejects all-primitive signatures).
    external fun traceAttach0(method: java.lang.reflect.Method): Int
    external fun traceAttach1(method: java.lang.reflect.Method): Int
    external fun traceAttach2(method: java.lang.reflect.Method): Int
    external fun traceAttach3(method: java.lang.reflect.Method): Int
    external fun traceDetach(method: java.lang.reflect.Method): Int
    external fun traceCount(): Int
    external fun traceEntryCount(): Int
    external fun traceExitCount(): Int
    external fun traceLastRet(): Int
    external fun traceResetCounters()

    // Trace fixture targets (String arg breaks fast-JNI eligibility).
    external fun traceTargetS0(warmup: String): Int
    external fun traceTargetS1(warmup: String, x: Int): Int
}