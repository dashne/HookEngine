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

    // Feature 1.5
    external fun attachRuntime(addr: Long, repl: Long): Int
    external fun detachRuntime(addr: Long): Int
    external fun getFnAttachTargetAddr(): Long
    external fun getFakeAttachAddr(): Long
    external fun getTestDataVarAddr(): Long
    external fun callFnAttachTarget(x: Int): Int
    external fun readU32At(addr: Long): Long

    // Feature 1.7
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

    // Feature 1.8
    external fun chainAttachCond(target: Long, predicate: Long, handler: Long): Int
    external fun getChainPredAcceptAllAddr(): Long
    external fun getChainPredRejectAllAddr(): Long
    external fun getChainPredX0Is10Addr(): Long
    external fun getChainPredX0Is20Addr(): Long
    external fun chainPredCalls(): Int
    external fun chainPredLastX0(): Int

    // Feature 1.9
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

    // Feature 1.6
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

    // Feature 2.7
    external fun getRegInspectGpTargetAddr(): Long
    external fun getRegInspectFpTargetAddr(): Long
    external fun callRegInspectGp(a: Int, b: Int): Int
    external fun callRegInspectFp(a: Float, b: Float): Float
    external fun attachRegInspect(target: Long, mode: Int): Int
    external fun detachRegInspect(target: Long): Int
    external fun regInspectCount(): Int
    external fun regInspectSnapshot(target: Long): LongArray?

    // Feature 2.1 / 2.2 / 2.3
    external fun getMemTestBufAddr(): Long
    external fun getMemTestRoPageAddr(): Long
    external fun memTestResetRoPage()
    external fun memTestPageProtOf(addr: Long): Int
    external fun memRead(addr: Long, len: Int): ByteArray?
    external fun memWrite(addr: Long, data: ByteArray): Int
    external fun memScan(start: Long, len: Long,
                         pattern: ByteArray, mask: ByteArray?,
                         maxOut: Int): LongArray?

    // Feature 2.5
    external fun memEnumMapsAllCount(): Int
    external fun memEnumMapsFiltered(prot_mask: Int, maxEntries: Int): LongArray?
    external fun memEnumMapsPathContains(substr: String): Int
    external fun memEnumMapsDebugDump(): String
    external fun memFindRegionContaining(addr: Long): LongArray?

    // Feature 2.8
    external fun btRunChain(): Int
    external fun btTrigger(): Int
    external fun btFrameCount(): Int
    external fun btFramePc(idx: Int): Long
    external fun btFrameFp(idx: Int): Long
    external fun btFixtureAAddr(): Long
    external fun btFixtureBAddr(): Long
    external fun btFixtureCAddr(): Long
    external fun btFilterOutLib(libSubstr: String): Int

    // Feature 3.4
    external fun smSetNullOverride(name: String): Boolean
    external fun smSetAliasOverride(from: String, to: String): Boolean
    external fun smClearOverrides()
    external fun smIsNullOverridden(name: String): Boolean
    external fun smFindAlias(name: String): String?
    external fun smNullHitCount(): Int
    external fun smAliasHitCount(): Int
    external fun installSvcMgrJniHook(): Int
    external fun uninstallSvcMgrJniHook()
    external fun svcMgrJniCount(): Int

    // Feature 3.4 Path A
    external fun bpSmProbe(): Int
    external fun bpSmInstall(): Int
    external fun bpSmUninstall()
    external fun bpSmCount(): Int

    // Feature 1.4
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

    // Feature 3.6
    external fun sigovrInstall(signo: Int, handlerId: Int, userCtx: Long): Int
    external fun sigovrUninstall(signo: Int): Int
    external fun sigovrCount(): Int
    external fun sigovrFireCount(signo: Int): Int
    external fun sigovrResetAll()
    external fun sigovrLastSiCode(): Int
    external fun sigovrLastPid(): Int
    external fun sigovrLastUid(): Int
    external fun sigovrLastSivalInt(): Int
    external fun sigovrLastPc(): Long
    external fun sigovrRaiseWithInt(signo: Int, sival: Int): Int
    external fun sigovrInstallRawSigaction(signo: Int): Int
    external fun sigovrRestoreRawSigaction(signo: Int)
    external fun sigovrRawSigactionCount(): Int
    external fun sigovrCurrentHandlerKind(signo: Int): Int

    // Feature 7.6
    external fun pltResolve(libName: String, symbolName: String): Long
    external fun pltCallOpen(pltAddr: Long, path: String, flags: Int): Int

    // Feature 3.7 — Native ServiceManager addition.
    // Binder bytes are built in C++ via Parcel.writeStrongBinder + native
    // buffer read. Parcel.marshall() refuses parcels containing binders,
    // so the byte-array API from the first attempt is gone.
    external fun syntheticAdd(name: String, binder: Any): Int
    external fun syntheticRemove(name: String): Int
    external fun syntheticCount(): Int
    external fun syntheticClearAll()

    // ─── Feature 6.8 — Signal-safe deferred uninstall (S65) ──────────
    // Arm() spawns the deferred worker thread and creates the eventfd.
    // MUST be called from normal thread context before any signal-handler
    // enqueue can succeed. Idempotent — second call returns 0.
    external fun signalSafeUninstallArm(): Int
    external fun signalSafeUninstallDisarm(): Int
    // Async-signal-safe enqueue. Called from within a signal handler.
    // Returns 0 on enqueue, -1 on queue full, -2 on null target, -3 if
    // not armed. Never blocks and never allocates.
    external fun signalSafeUninstallFromHandler(target: Long): Int
    // Polls FindHandle(target) until null or timeout (normal context).
    // Returns 0 if hook was uninstalled within timeout, -1 on timeout.
    external fun signalSafeUninstallWaitFor(target: Long, timeoutMs: Int): Int
    // Returns {enqueued, completed, failed, dropped} atomically.
    external fun signalSafeUninstallStats(): LongArray
}