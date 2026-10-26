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

    // ─── Feature 1.8 — conditional hook ─────────────────────────────
    external fun chainAttachCond(target: Long, predicate: Long, handler: Long): Int
    external fun getChainPredAcceptAllAddr(): Long
    external fun getChainPredRejectAllAddr(): Long
    external fun getChainPredX0Is10Addr(): Long
    external fun getChainPredX0Is20Addr(): Long
    external fun chainPredCalls(): Int
    external fun chainPredLastX0(): Int

    // ─── Feature 1.9 — return-value modification ────────────────────
    external fun attachRetMod(target: Long, modifier: Long): Int
    external fun detachRetMod(target: Long): Int
    external fun retModCount(): Int
    external fun retModCalls(): Int
    external fun retModLastOrigRet(): Int
    external fun retModLastArg0(): Int
    external fun retModReset()
    external fun getRetModAdd1000Addr(): Long
    external fun getRetModClamp100Addr(): Long
    external fun getRetModForceDeadAddr(): Long

    // ─── Feature 1.6 — SVC instruction hooking ──────────────────────
    external fun getSvcGetpidTargetAddr(): Long
    external fun getSvcGetpidSvcAddr(): Long
    external fun getSvcGetppidTargetAddr(): Long
    external fun getSvcGetppidSvcAddr(): Long
    external fun getSvcCloseInvalidTargetAddr(): Long
    external fun getSvcCloseInvalidSvcAddr(): Long

    external fun getSvcHandlerForce7aceAddr(): Long
    external fun getSvcHandlerAdd1000Addr(): Long
    external fun getSvcHandlerIdentityAddr(): Long

    external fun callSvcGetpid(): Int
    external fun callSvcGetppid(): Int
    external fun callSvcCloseInvalid(fd: Int): Int

    external fun svcHookAttach(svcAddr: Long, expectedNr: Int, handler: Long): Int
    external fun svcHookDetach(svcAddr: Long): Int
    external fun svcHookCount(): Int
    external fun svcHookScanLib(lib: String): Int

    external fun svcFires(): Long
    external fun svcLastNr(): Long
    external fun svcLastOrig(): Long
    external fun svcLastArg0(): Long
    external fun svcLastArg1(): Long
    external fun svcResetCounters()

    // ─── Feature 2.7 — register state inspection ────────────────────
    //
    // Snapshot array layout (99 longs):
    //   [0..31]   x[0..31]   (x0..x30, sp_orig)
    //   [32]      nzcv
    //   [33]      pc
    //   [34]      slot_id
    //   [35..98]  q[0..63]   (32 Q-regs × low/high u64 pairs)
    external fun getRegInspectGpTargetAddr(): Long
    external fun getRegInspectFpTargetAddr(): Long
    external fun callRegInspectGp(a: Int, b: Int): Int
    external fun callRegInspectFp(a: Float, b: Float): Float
    external fun attachRegInspect(target: Long, mode: Int): Int
    external fun detachRegInspect(target: Long): Int
    external fun regInspectCount(): Int
    external fun regInspectSnapshot(target: Long): LongArray?

    // ─── Feature 1.4 — ArtMethod entry/exit trace ───────────────────
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

    external fun traceTargetS0(warmup: String): Int
    external fun traceTargetS1(warmup: String, x: Int): Int
}