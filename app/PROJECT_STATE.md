Last verified build: v29, verified on device 2026-10-23, items 1–5 all green (23 tests), Item 7 §7.1–§7.5 green on device.

Session 13 closed 2026-10-25: baseline confirmed at 48 tests green. handleJHMethod DEFERRED — no sub_29E168 decomp, body unchanged, surface wired but behavior unverified. nativeCNVM decomp-verified against sub_30BF7C + sub_30C238. Coverage 13/90 (unchanged — handleJHMethod deferred, nativeCNVM surface present but not counted as reference-behavior-match until ProxyBridgeSurfaceTest is confirmed running in the suite).

Session 12 closed 2026-10-24: Q1 flags-check measured (code 36 only matters for signature-flag callers), code-36 byte-rewrite deferred, exception-path test added, AidlProbe narrowed. 48 tests green. Coverage 13/90 (unchanged — pivot to handleJHMethod next).

Session 11 closed 2026-10-24: tnFIN rewriter exception safety, drainCodes stub fix, code-36 reply captured + hex-dumped. 47 tests green. Coverage 13/90 (unchanged — code 36 not delivered).

Completed:
- Registry finding DISPROVED — RegistryCount() counts inline hooks, regDuring=1 observed. Assertion tightened in both directions.
- §7.1 — 23 sites migrated to testreport::Report, logcat byte-identical.
- §7.2 — test_suite.cpp dispatch, per-item harness runners extracted. Logcat byte-identical.
- §7.3 — JNI bridge (NativeTestBridge.runSuite / uninstallAll / setItem4Paths).
- §7.4 — NativeTestBridge.kt Kotlin object.
- §7.5 — NativeItemTest.kt parameterized runner. 5/5 green on device via am instrument.
- §7.6 — regression suites bug1–bug5. 10 tests green.
- §7.7 — selftest.txt file channel + IntegrationTest.kt. 11 tests green.
- §7.9 — build-only CI + README limitations. Item 7 CLOSED.
- Phase 2 — 40 per-function reloc cases. 12 tests green.
- Mission #1 Phase A: tnFIN mechanism (Session 8), rewriter algorithm (Session 9), production delivery for code 3 (Session 10), code-36 dispatch confirmed + reply format captured (Session 11).

Pending:
- Mission #1 Phase A: tnFIN code 36 byte-rewrite (Session 12+).
- Mission #1 Phase A: handleJHMethod + nativeCNVM dispatcher.
- Mission #1 Phase A: nativeInit full config.
- Mission #1 Phase A: nativeOnLoad full DEX extract.

---

## Registry finding — DISPROVED, assertion tightened

The v29 draft suspected regAfter == regBefore in test_item3.cpp::run_test_installed_state was vacuous because the log showed regBefore=0 regAfter=0 while a hook was installed, and inferred that RegistryCount() excludes inline hooks.

That inference was wrong. The samples were taken outside the installed window. regBefore is captured before install; regAfter after uninstall. That pair tests exactly one property — "uninstall leaks no slot" — and would have failed on a real leak. It was incomplete, not vacuous.

Device evidence (2026-10-23, diagnostic build):

UninstallAllHooks: 26 hooks examined, 0 failures
IIHE(installed): ... RegistryAlloc ... DONE
installed_state diag: regBefore=0 regDuring=1
[PASS] installed_state: orig=0 fake=25000 bad=0 post=11 regBefore=0 regDuring=1 regAfter=0

regDuring=1 — RegistryCount() does count inline hooks. InstallInlineHookEx sets HS_INSTALLED; RegistryCount() counts HS_INSTALLED; therefore the registry covers both GOT and inline hooks.

Assertion change (test_item3.cpp::run_test_installed_state): kept regAfter == regBefore, added regDuring == regBefore + 1. Fails on either direction of a leak:

| Failure mode | Which condition catches it |
|---|---|
| Install doesn't register a slot | regDuring == regBefore + 1 fails |
| Uninstall leaks a slot | regAfter == regBefore fails |

No C++ counters added. The proposed g_inline_install_count / g_inline_uninstall_count / g_relay_alloc_count / g_relay_free_count were unnecessary — RegistryCount() already provides what's needed.

---

## Device test signal is triple-unreliable

Discovered 2026-10-23. Three layers, each independently capable of returning green on failure:

1. :app:connectedDebugAndroidTest — fails to install the test APK on this HyperOS device with SecurityException: INSTALL_GRANT_RUNTIME_PERMISSIONS (UTP-injected flag, refused by MIUI PackageInstallerService), logs AndroidTestRunner failed, and then reports BUILD SUCCESSFUL. Zero tests ran; the task returned green.

2. am instrument -w — returns INSTRUMENTATION_CODE: -1 (Activity.RESULT_OK) on both success and failure. Tests run: 6, Failures: 1 was observed with INSTRUMENTATION_CODE: -1. Exit code is 0 either way.

3. Consequence: no automated signal from the device is trustworthy as a CI gate. Reliable gate is parsing output:
- OK (N tests) = pass
- presence of INSTRUMENTATION_STATUS_CODE: -2 = fail

Verified device workflow (only method that works on this device):

adb install -r -t app/build/outputs/apk/debug/app-debug.apk
adb install -r -t app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb logcat -c
adb shell am instrument -w -r com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner

adb install succeeds where UTP does not — the security exception is specific to the UTP install path, not to pm install.

CI (§7.9) stays build-only. README must document this under the test-suite section, not as a footnote.

---

## Known issue — RESOLVED: duplicate log blocks, not duplicate runs

The suspected "two PROCESS STARTED lines and the Item 1 block appearing three times" is expected behavior:

- Two PROCESS STARTED: main process + :remote process (RemoteService is declared android:process=":remote"). One line per process.
- Three emissions of each suite: three distinct emission points, fired once each per run —
    1. per-test testreport::Report() inside the loop in test_itemN.cpp;
    2. LOGD("%s", out.str().c_str()) at the end of the same function;
    3. Kotlin Log.d("DualCore", runItemNTests()) in RemoteService.onStartCommand.

Same single run. The suites execute once. Closed.

Item 7 implication: the file-based channel (§7.7) is mandatory, not optional. Logcat scraping would count 3×. RemoteService must write each line exactly once to filesDir/selftest.txt.

---

## v29 handoff deltas (applied)

| Part | Change | Status |
|---|---|---|
| Header | v28 → v29 verified on device 2026-10-23; items 1–5 all green, 23 tests | applied |
| Part 2 Item 3 | ⚠️ → ✅. Registry finding disproved — assertion tightened, not replaced. | applied |
| Part 2 Item 5 | ⚠️ → ✅. gate_refuses_unverified: api33=1 api34=1 api35=1 api36=1 api38=1 | applied |
| Part 5 | Item 3 expected: installed_state: orig=0 fake=25000 bad=0 post=11 regBefore=0 regDuring=1 regAfter=0 | applied |
| Part 5 | Item 5 expected: hook_refused_when_gate_blocks: before=10421 rc_refused=1 ... after=10421 | applied |
| Part 7 | "27 tests" → 23 tests (6+5+3+4+5) | applied |
| Part 8 README | Item 3 proof: fake=25000 / fake=68934 | applied |
| Part 13 | Summary: item 6 verified, item 7 in progress | applied |
| RemoteService.kt | Service started. (v27 — both fields) → (v29) | applied |
| Known issue | Duplicate log blocks — RESOLVED | applied |

---

## Item 7 — deliverable status

Design decisions:

1. File-based result channel, not logcat scraping (§7.7). RemoteService writes filesDir/selftest.txt; the integration test polls the file. Logcat stays as the human workflow. Required because logcat triple-counts.
2. C++ stays in C++. Primitives (ReadPageProt, AllocateMemoryNear, ShouldHookDlopen) live natively. Regression checks for Bugs 1, 2, 4, 5 are C++. Bug 3 is Kotlin-side (§7.6).
3. Aggregate runner with per-test reporting, not one JUnit method per C++ function. 23 individually-reported cases is the deliverable.

### §7.1 — CLOSED 2026-10-23

app/src/main/cpp/TestReport.h added. All 23 [PASS]/[FAIL] sites in test_item1.cpp through test_item5.cpp migrated from LOGD to testreport::Report(name, pass, detail). Report function prints to logcat via ANDROID_LOG_DEBUG (same level as LOGD, filters unchanged) and appends to an in-memory std::vector<Line>.

Verified on device: logcat output byte-identical to pre-refactor run — same 23 lines, same field order, same 3-emission pattern per suite.

Drain() renamed to Snapshot() — the function does not clear, so the name reflects behavior.

### §7.2 — CLOSED 2026-10-23

test_suite.h / test_suite.cpp added. RunTestSuite(env, name) dispatches to RunItem1ForHarness() … RunItem5ForHarness(env).

Each test_itemN.cpp refactored: the JNI wrapper runItemNTests now calls a new static int RunItemNInternal(std::string& out) shared function. The harness path calls RunItemNInternal with an ignored out-param. JNI path unchanged at runtime.

CMakeLists.txt extended with test_suite.cpp.

RunTestSuite is dead code in this commit — verified by logcat remaining byte-identical.

RunItem4ForHarness and RunItem5ForHarness needed plumbing decided in §7.5: item4 needs writable paths, item5 needs a reflected Method object.

### §7.3 — CLOSED 2026-10-23

native-lib.cpp gained three JNI exports:
- Java_com_ims134_hookengine_NativeTestBridge_runSuite(jstring) → array of "PASS|name|detail" or "FAIL|name|detail"
- Java_com_ims134_hookengine_NativeTestBridge_uninstallAll() → UninstallAllHooks()
- Java_com_ims134_hookengine_NativeTestBridge_setItem4Paths(jstring, jstring) → forwarded to SetItem4Paths in test_item4.cpp

Plus extern "C" int UninstallAllHooks(); forward declaration.

### §7.4 — CLOSED 2026-10-23

app/src/androidTest/java/com/ims134/hookengine/NativeTestBridge.kt — Kotlin object with System.loadLibrary("hookengine") and three external funs matching the §7.3 exports.

### §7.5 — CLOSED 2026-10-23

app/src/androidTest/java/com/ims134/hookengine/NativeItemTest.kt — parameterized runner, 5 cases (item1..item5, expectedMin = 6/5/3/4/5). Per-case assertions:
1. lines.isNotEmpty()
2. lines.size >= expectedMin
3. no line starts with FAIL|

@Before calls uninstallAll and setItem4Paths with targetContext.cacheDir/item4_real and .../item4_fake. @After calls uninstallAll.

Device verification: adb shell am instrument -w -r com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner → numtests=5, all suites INSTRUMENTATION_STATUS_CODE: 0, final OK (5 tests).

Findings during §7.5:

1. mprotect_failure over-specified the refusal code. On the instrumentation process, the target mmap landed at 0x4034b50 (dense address space), and AllocateMemoryNear failed before mprotect was reached: HOOK_ALLOC_FAILED (6) instead of HOOK_MPROTECT_FAILED (5). Test now accepts both. Invariant tested is "install refused, target bytes unchanged, registry unchanged", not the specific refusal code.

2. ExampleInstrumentedTest template was in the test APK, inflating numtests=6. Deleted.

3. INSTRUMENTATION_CODE: -1 returns on both success and failure. See "Device test signal is triple-unreliable".

Not verified by §7.5: logcat format vs. app path ([PASS] lines identical), absence of === Item N tests === headers in harness output.

### §7.8 — CLOSED 2026-10-23

app/build.gradle.kts:
- testInstrumentationRunner "androidx.test.runner.AndroidJUnitRunner"
- testBuildType "debug"
- ndk { abiFilters += "arm64-v8a" } — required: test files contain ARM64 inline assembly and do not compile for x86/x86_64/armv7.
- androidTestImplementation for androidx.test.ext:junit:1.2.1, androidx.test:runner:1.6.2, androidx.test:rules:1.6.1

---

## Assumed symbols — verified against actual headers

| Assumed | Verified |
|---|---|
| InstallInlineHookEx(void*, void*, void**, const char*) returns HookResult with HOOK_OK=0 | matches HookRegistry.h |
| UninstallInlineHook(void*) returns HookResult | yes |
| SetUninstallGraceMs(long) exists | yes, InlineHooker.cpp |
| RunItemNTests() callable | refactored to shared RunItemNInternal in §7.2 |
| Native lib name is hookengine | yes |
| installPmSignatureSpoof callable from test context | NO — private to RemoteService. Bug 3 regression (§7.6) needed extraction. |
| RegistryCount() excludes inline hooks | DISPROVED — counts them |

---

## Item 7 — CLOSED 2026-10-23

Final device state: `am instrument -w -r` → `numtests=11`, `OK (11 tests)`.
33 underlying assertions (23 item + 8 regression + 1 integration + 1 Kotlin bug3), 0 failures.

Files added across Item 7:
- `app/src/main/cpp/TestReport.h`
- `app/src/main/cpp/test_suite.h` / `test_suite.cpp`
- `app/src/main/cpp/test_regression.cpp`
- `app/src/androidTest/java/com/ims134/hookengine/NativeTestBridge.kt`
- `app/src/androidTest/java/com/ims134/hookengine/NativeItemTest.kt`
- `app/src/androidTest/java/com/ims134/hookengine/RegressionTest.kt`
- `app/src/androidTest/java/com/ims134/hookengine/IntegrationTest.kt`
- `app/src/main/java/com/ims134/hookengine/PmSpoof.kt`
- `.github/workflows/test.yml`

---

## Phase 2 — per-function reloc tests — CLOSED 2026-10-23

40 cases across `ClassifyInsn` (20), `EncodeBranch` (8),
`EmitRelocated` (12). Device: `am instrument -w -r` → `numtests=12`,
`OK (12 tests)`, all reloc cases pass.

Coverage beyond Item 1:
- `ClassifyInsn` — 20 canonical/boundary words, raw roundtrip
- `EncodeBranch` — forward/backward/self, B vs BL opcode,
  unaligned rejection, ±128MB edge (in and out), decode roundtrip
- `EmitRelocated` — NORMAL cap=0/1, B negative offset, BL 3-instruction
  seq (STP/BL/LDP) + cap=2 rejection, ADRP negative page, ADR short
  (n=1), ADR long fallback to ADRP+ADD (n=2), all four
  unsupported-kind rejections (B.cond, CBZ, TBZ, LDR-literal)

Two test bugs found and fixed while writing:
1. `EncodeBranch` "edge in" test used `+(1LL<<27)` which is out of
   range — max valid forward is `2^27 - 4`. Fixed.
2. `make_adr(1, 0x200000)` overflowed the 21-bit signed field —
   encoded `ADR x1, +0`, so EmitRelocated produced `n=1` not `n=2`.
   Fixed with `make_adr(1, 0x80000)` and `new_pc=0x400000`.

Both are test-only; the underlying `arm64_reloc.cpp` is unchanged and
still passes all original Item 1 cases.

---

## API 37 finding — Process.MY_UID / MY_PID do not exist — 2026-10-23

Verified on device: `GetStaticFieldID(Process, "MY_UID", "I")` and the same
for `MY_PID` both return null + `NoSuchFieldError` on API 37.
`Process.myUid()` / `Process.myPid()` are fast-JNI natives; they do not read
any static field. The Batch 2a plan ("spoof MY_UID/MY_PID reflectively and
verify Process.myUid() returns 10000") is therefore a no-op on this API.

What works on API 37:
- `Binder.getCallingUid()` ArtMethod hook          (Item 5 + ProcessHooksTest)
- `libc::getpid` inline hook                        (test_process_hooks Test 4)
- `getuid/geteuid/getgid/getegid` GOT hooks in libjavacore / libopenjdk
- `execve/execv/execvp/system/ptrace/kill` inline hooks

What does NOT work:
- Field spoofing of Process.MY_UID / MY_PID (fields don't exist)
- ArtMethod hook of Process.myUid / myPid / myTid / myTgid (fast-JNI tokens)

Consequences:
- The `Process.myPpid` Java hook is attempted but often not installed because
  `myPpid` is also fast-JNI (installed count is 8, not 9, on API 37 — the
  test accepts this).
- Batch 2a as described in the original handoff should be retired. To spoof
  UID as seen by the target app, choose the entry point it actually uses:
  (a) `Binder.getCallingUid()`  → already covered by Item 5
  (b) `Process.myUid()`         → needs native `getuid` hook (already in
  Hooks.cpp as Fake_getuid, gated on
  g_systemCallDepth)
  (c) direct `getuid` syscall   → out of scope (needs seccomp / kernel)

---

## Test suite status — 16/16 green (pre-Session 8)

Classes:
- IntegrationTest              (1)   selftest file channel, 23 PASS lines
- NativeItemTest               (5)   items 1–5
- ProcessHooksTest             (4)   allProcessHooksPass,
  spoofProcessStaticsApi37State,
  processMyUidIsNative,
  doesFrameworkSeeNativeHook
- RegressionTest               (5)   bug1–bug5
- RelocTest                    (1)   40 per-function reloc cases

Underlying assertions: 33+ (items) + 9 (process_hooks) + 3×bug4 + 3×bug5 +
1×bug1 + 1×bug2 + 40 reloc = 90+ PASS lines, 0 FAIL.

---

## PmSpoof filter delivered; hidden API bypass via thread-attached reflection

Two separate fixes:

1. JavaHook gate: CheckHookCapability() now called inside JavaHookInstall.
   Prior code only protected HookJavaMethod; ProcessHooks.cpp called
   JavaHookInstall directly and would patch ArtMethod offset 24 blind on
   any ART build outside [37,37].

2. Hidden API bypass: Class.getDeclaredField / getDeclaredMethod calls
   issued from an AttachCurrentThread'ed thread bypass the hidden-API
   caller-domain check. Modeled on org.chickenhook.restrictionbypass.
   Resolves BaseParceledListSlice.mList, enabling PmSpoof list filtering
   to read, filter, and write back the item list without parcel
   marshalling, Unsafe offsets, or CREATOR reflection.

Test suite: 21/21 green. PmSpoof 4/4. Process hooks 9/9.

---

## Batch 2c — DEFERRED pending design decision — 2026-10-23

Probe results on API 37 / HyperOS (ClassLoaderProbeTest):

    DexFile.loadClassBinaryName(String, ClassLoader)   NOT FOUND
    DexFile.loadClass(String, ClassLoader)             native=false public
    ClassLoader.loadClass(String)                      native=false public
    ClassLoader.loadClass(String, boolean)             native=false protected
    ClassLoader.findClass(String)                      native=false protected
    BaseDexClassLoader.findClass(String)               native=false protected

Conclusion: **every class-loading entry point is a Java method.**
`JavaHookInstall` (which only replaces `ArtMethod.data_` = JNI entry,
offset 24) cannot hook any of them. `ClassLoaderHooks.cpp` as
originally planned is impossible.

The reference NativeEngine does NOT hook loadClass either. It caches
`DexFile.openDexFileNative` (a real native method) — that's DEX-file-
swapping, where merged/prepended DEX files make `loadClass` succeed
naturally. That's a different architecture, not a hook.

### Decision required before any 2c code

Three viable paths, all with different cost/benefit:

1. **Java proxy on guest ClassLoader instance** — wrap the guest's
   BaseDexClassLoader with a subclass that falls through to the host
   classloader on ClassNotFoundException. Requires subclassing a
   concrete class (impossible via Proxy), or bytecode manipulation
   of the guest's existing classloader. **Cost: high. Fragile.**

2. **DEX-file-swapping via openDexFileNative hook** — intercept DEX
   open, return a merged DEX that includes both guest and host DEX.
   Matches the reference engine. **Cost: very high** (build merged
   DEX at runtime, multidex handling, pathList swap via hidden API).
   Delivers the correct semantics.

3. **Rely on guest/host classloader compatibility** — if DualHost
   already loads host and guest DEX in the same classloader chain,
   Viber's `loadClass` calls may naturally find host classes.
   **Cost: zero.** Verify at integration time whether this is needed
   at all.

**Recommendation:** path 3 first. Attempt DualHost integration, watch
whether Viber actually fails with `ClassNotFoundException` on host
classes. If yes, path 1. Path 2 only if path 1 proves insufficient
in practice.

Do not write ClassLoaderHooks.cpp as a JavaHookInstall target. It
will not work.

---

## MISSION #1 — Reproduce ApplicationProxy — declared 2026-10-24

### Scope (permanent, supersedes all prior plans)

Replicate the reference implementation
`com.excelliance.kxqp.platform.ApplicationProxy` (decompiled) **completely**
before any DualHost integration. The reference is ~90 native methods across
11 categories. Every method must be present in our engine — either as a
direct port, an equivalent under a different name, or (with justification)
explicitly marked out-of-scope.

**DualHost integration is BLOCKED until the reference surface is matched.**

### Current coverage: 13 / 90 (~14%)

**Delivered (matches reference):**
| Method | Ours | Status |
|---|---|---|
| `attachBaseContext` | `DualApplication.attachBaseContext` | ✅ |
| `onCreate` | `DualApplication.onCreate` | ✅ |
| `nativeOnLoad` | `DualApplication.cpp::nativeOnLoad` | ⚠️ Partial — records paths, doesn't extract DEX |
| `nativeFNLN` | `ClassLoaderBridge.installFor` | ✅ |
| `getCallingUid` | `FakeBinderGetCallingUid` | ✅ |
| `is64Bit` | Implicit (arm64-only build) | ✅ |
| `nativeGetCpuABI` | Implicit (arm64-only build) | ✅ |
| | `tnFIN` | `BinderTransactHook` + `ParcelRewriter` + `CppParcelRewriter` | ✅ Production delivery for code 3 (getPackageInfo). Code 36 (getInstalledPackages) dispatched but byte-rewrite deferred — see Session 12 decision note. Code 36 only matters for callers passing signature flags; not observed as a target requirement. |
| `nativeDNPO` | TracerPid filter | ⚠️ Partial — misses full ptrace detect |
| `nativeAMRHook` | Path hide list | ⚠️ Partial — no mount spoof |

**Not started (77 methods):**
- **`nativeCNVM`** — JNI dispatcher. Surface wired (`ProxyBridge.nativeCNVM` + `ProxyBridge.cpp`); body decomp-verified against sub_30BF7C + sub_30C238 (Session 13). Not yet counted as reference-behavior-match until `ProxyBridgeSurfaceTest` is confirmed present in the suite.
- **`handleJHMethod`** — **DEFERRED**. No decomp of sub_29E168. Body in `ProxyBridge.cpp` present but not claimed as behavior-verified per Rule 5. Known open issues that will be fixed together with sub_29E168: (1) `BoxResult` FindClass-per-call local ref leak; (2) static dispatch uses `GetObjectClass(obj)` — declaring class unverified; (3) `IsStaticMethod` FindClass-per-call leak.
- **`nativeInit`** (9 params) — full context config
- **`nativeOnLoad`** (full DEX extract) — currently partial
- **tnFIN code 36 byte-rewrite** — dispatched, not rewritten (Session 11 confirmed code 36 constant; layout hypothesis captured, decomp still required)
- **Per-signature hook family (13 methods)** — `nativeBINH`, `nativeBPNH`, `nativeCLFH`, `nativeFRNH`, `nativeFSC`, `nativeLILH`, `nativeMDRMH`, `nativeMMIORH`, `nativeNGSTHook`, `nativeNINH`, `nativeSSMH`, `nativeTSTNH`, `nativeVMRTH`
- **Field/method getters (~15)** — `onGIF`, `onSIF`, `getFCO`, `getFNIR`, `getFIAAR`, `getFSTAF`, `getFBI`, `getFBABAI`, `getFWM`, `nativeGDFK`, `nativeGETDFNH`, `nativeGETDFS`, `nativeGETDMUH`, `getNIDS`, `nativeGIOMP`
- **Anti-detection remaining (~20)** — `nativeADB`, `nativeADOI`, `nativeAURR`, `nativeASK`, `nativeALBNM`, `nativeDNPO` (full), `nativeEROT`, `nativeIMME`, `nativeSHH`, `nativeSDMD`, `nativeSDTWI`, `nativeSIOR`, `nativeUFPL`, `nativeSFU`, `nativeSGSP`, `nativeSNF`, `nativeSQHCT`, `nativeSTZ`, `nativeTTLHW`, `nativeSATSV`, `nativeDBTHL`, `nativeLNLIH`, `nativeRTIMs`, `nativeLFMH`, `nativeLFMUH`, `nativeMMPH`, `nativeDJCV`, `nativeNPX`, `nativeSPPL`
- **File/IO (~5)** — `nativeSFSP`, `nativeSDO`, `nativeFCSCSG`, `nativeFIOR`, `nativeFGR`
- **NLDS (~2)** — `nativeStartNLDS`, `nativeUpdateNLDS`
- **ANR/crash/debug (~13)** — `nativeSignalAnrDetectiveInit`, `onANRDumped`, `onTraceFileDumpFinished`, `onPKTECB`, `reportAppExit`, `reportAppHaveDetected`, `reportHanhuaCorrupted`, `reportHanhuaUpdate`, `reportNativeCrashInfo`, `reportNewGameStarted`, `nativeDbgCaptureLog`, `nativeDbgClientInit`, `nativeDbgServerStart`
- **Misc (~7)** — `nativeJH*` family (`nativeJHHJM`, `nativeJHIOJM`, `nativeJHJHC`, `nativeJHYABAH`, `nativeJHYAInit`, `nativeJHI`, `nativeJHJI`, `nativeJHANRM`)
- **Public instance methods (~2)** — `call(int, Object[])`, `callAssistant(Context, int, Object[])`
- **Static init** — DEX extraction from APK-JAR with checksum + file lock

### Order of attack

***Phase A — Core engine (blocking everything else):**
1. ✅ `tnFIN` — mechanism (Session 8), algorithm (Session 9), production code 3 (Session 10), code-36 dispatch confirmed (Session 11)
2. ⏸️ `tnFIN` code 36 byte-rewrite — DEFERRED (Session 12). Code 36 measured irrelevant for flags=0 callers. Reopen if a target is shown to require signature-flag `getInstalledPackages`, or if decomp of `PackageInfo.readFromParcel` + `BaseParceledListSlice.readFromParcel` lands.
3. ⚠️ `handleJHMethod` + `nativeCNVM` (Java dispatcher)
    - `nativeCNVM` — surface delivered; decomp-verified vs sub_30BF7C + sub_30C238 (Session 13). Coverage credit pending ProxyBridgeSurfaceTest in suite.
    - `handleJHMethod` — **DEFERRED** (no sub_29E168 decomp; body unchanged; three open issues listed in the "Not started" section).
4. 🔲 `nativeInit` (full config)
5. 🔲 `nativeOnLoad` (full DEX extract, if needed)

**Phase B — Java hook surface:**
6. Per-signature hook family (13 methods)
7. Field/method getters (~15)

**Phase C — Anti-detection full suite (~20)**

**Phase D — ANR / crash / debug (~13)**

**Phase E — Misc + public instance methods**

### Rules of engagement

1. **Every session opens with the mission header.** No exceptions.
2. **DualHost integration is out of scope until Phase A complete.** When Phase A is done, re-evaluate; DualHost may still wait for Phase B.
3. **Coverage number in every message.** 13/90 now, must be higher every session.
4. **Every method shipped must have a test.** Decision-function tests + E2E where the hook fires.
5. **Do not declare a method "done" unless it matches the reference's behavior**, not just its signature.
6. **Session-by-session commit discipline.** One method (or one logical group) per commit, tests green at every commit.
7. **When wire format, symbol contract, or field layout is uncertain, request IDA decomp of the reference implementation before writing parse code.** Do not iterate on offset guesses. Session 9 burned three rebuild cycles on this. Session 10 confirmed it — dynsym scan for symbols, IDA-verified field offsets for Parcel struct, no guessing. Session 11 reinforced it — guessed slice format caused OOM; guessed offset caused SIGABRT. Only the hex dump produced facts.
8. **On this ROM, any JNI call taking a `jobject` on a caller thread with a JIT-compiled `BinderProxy.transact` frame above it crashes under CheckJNI.** Not just `NewGlobalRef`. Not just ref operations. `GetLongField`, `GetObjectField`, `CallIntMethod` on a `jobject` — every one. Hook at the C++ layer (`IPCThreadState::transact`) or offload JNI to a fresh attached thread. Session 8 discovered for `NewGlobalRef`; Session 10 confirmed for `GetLongField`. Do not re-discover.
9. **dlsym(RTLD_DEFAULT, ...) filters `android::Parcel` primitives and `IPCThreadState::transact` on this ROM.** Use dynsym scan (the `BinderProbe.cpp` / `CppParcelRewriter.cpp` pattern) for any `libbinder.so` symbol. dlsym returns null; do not treat null as "symbol does not exist."
10. **The reply parcel's `mData` buffer is read-only post-transact.** Kernel fills it via `copy_to_user`. Never write into it. Copy to a fresh `malloc`'d buffer, rewrite there, swap the pointer back into the struct (struct fields +8/+16/+24/+32 are writable — `setDataPosition` writes +32 successfully).
11. **Kotlin `object` instance fields are not static.** `GetStaticFieldID` on `ourPackageName` returns null. Add a `@JvmStatic` accessor. Session 10 bug.
12. **Test assertions must be able to fail.** If a test can only pass, it is not a test. Session 9's `rawAidlGetPackageInfoReturnsNullSignatures` was premise-broken (server returned null anyway); Session 10's `hookFiresOnRealPackageManagerQuery` was cached; Session 11's `confirmGetPackageInfoCode` returns empty codes on this ROM (diagnostic only, not assertable). Fix the test, not the assertion.

### Current test count: 47 green

---

## Session 7 — BinderProbe results + strategy correction — 2026-10-24

### Mission #1 status

**Reference surface coverage: 10 / 90 (~11%).**
DualHost integration blocked until all 90 methods are present. No exceptions.

### What was probed

`BinderProbe.cpp` scans `libbinder.so` `.dynsym` for the symbols we'd need
to implement `tnFIN` (Binder transact interceptor).

### Key findings

1. **`BinderProxy::transact` is NOT a C++ symbol** in `libbinder.so`.
   Neither mangling variant found. Reason: `android.os.BinderProxy.transact`
   is a **Java `native` method**, implemented as JNI function
   `android_os_BinderProxy_transact` in `libandroid_runtime.so`, registered
   via `RegisterNatives`. It was never a C++ class method.

2. **The reference's `tnFIN` signature confirms this:**
   `tnFIN(Object obj, int i, Parcel parcel, Parcel parcel2, int i2)`
   = JNI signature of a hooked `BinderProxy.transact`. The reference is
   hooking the **Java** native method's JNI entry, not a C++ symbol.

3. **All Parcel I/O methods are exported** in `libbinder.so`:
    - `Parcel::readInt32`           `_ZNK7android6Parcel9readInt32EPi`
    - `Parcel::writeInt32`          `_ZN7android6Parcel10writeInt32Ei`
    - `Parcel::readString16`        `_ZNK7android6Parcel12readString16EPNS_8String16E`
    - `Parcel::writeString16`       `_ZN7android6Parcel13writeString16EPKDsm`
    - `Parcel::readStrongBinder`    `_ZNK7android6Parcel16readStrongBinderEPNS_2spINS_7IBinderEEE`
    - `Parcel::writeStrongBinder`   `_ZN7android6Parcel17writeStrongBinderERKNS_2spINS_7IBinderEEE`
    - `Parcel::data`                `_ZNK7android6Parcel4dataEv`
    - `Parcel::dataSize`            `_ZNK7android6Parcel8dataSizeEv`
    - `Parcel::dataPosition`        `_ZNK7android6Parcel12dataPositionEv`
    - `Parcel::setDataPosition`     `_ZNK7android6Parcel15setDataPositionEm`

   Fallback path for reading parcels if Java-side `Parcel.readInt()` ever
   gets blocked.

4. **`IPCThreadState::transact` IS exported**:
   `_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j`
   Signature: `(int handle, uint32_t code, const Parcel& data, Parcel* reply, uint32_t flags)`
   Alternative hook target if Java-level `BinderProxy.transact` proves
   insufficient (e.g., Viber uses a native path that bypasses the Java
   transact wrapper).

### Strategy correction (superseded by Session 10)

Old plan: hook `BinderProxy::transact` at C++ level — impossible, symbol
doesn't exist.

Session 7 plan: hook Java `BinderProxy.transact` via `JavaHookInstall`.
This was implemented in Session 8, worked at the mechanism level, but
was superseded in Session 10 because of the CheckJNI crash — see
Session 10 pivot.

### Session 7 deliverables

- `BinderProbe.cpp` — symbol scanner for libbinder.so (dynsym only)
- `BinderProbeTest.kt` — diagnostic test (deleted in Session 10)
- 32 JUnit tests green

---

## Session 8 — tnFIN mechanism delivered — 2026-10-24### Mission #1 status: 11 / 90 (~12%)

`tnFIN` (Binder transact interceptor) mechanism is live. Reference
coverage moved 10 → 11. DualHost remains blocked until Phase A completes.

### What "mechanism" means vs "full tnFIN"

Delivered this session:
- `BinderProxy.transactNative` hooked via `JavaHookInstall`
- `FakeBinderTransact` fires on real PackageManager calls (count > 0)
- Pass-through preserves behavior (no observable change to callers)
- Install / uninstall round-trip clean

Not yet delivered (Session 9): Parcel rewriting, code dispatch, recursion guard.

### Key technical findings

**1. `BinderProxy.transact` is NOT native on API 37.** It's a Java wrapper.
Probe confirmed: `public` (not `native`), calls `transactNative`.

**2. `BinderProxy.transactNative` IS native.** Method ID real
(`FromReflectedMethod = 0x71ac6130`, not fast-JNI token).

**3. `transactNative`'s `data_` (offset 24) is a JIT stub, not a libart pointer.**
- `data_ = 0x48166ef0`
- `dladdr` fails (anon exec memory)
- Page is `r-xp` (executable)
- Reason: API 34+ ART JIT-compiles JNI stubs via `art::jit::JniCompiler`.
  Hot native methods get per-method JIT stubs instead of direct libart
  dispatch entries.

**4. `FindEntrySlot` was over-strict.** Required `dladdr` to succeed and
name a known lib. Fix: accept any `data_ >= 0x1000` that is executable.
Keep the fast-JNI token guard (`< 0x1000`) as the true safety check.

**5. Regression clean.** `Binder.getCallingUid` (`data_ = 0x6d70c63310`, in
`libart.so`) still hooks via the old `dladdr` path. Both paths coexist.

---

## Session 9 — tnFIN rewriter algorithm delivered — 2026-10-24

### Mission #1 status: 12 / 90 (~13%)

`tnFIN` rewriter algorithm is verified end-to-end on **synthetic parcels**.
Reference coverage moved 11 → 12.

### What was delivered

- **Rewriter algorithm** — `ParcelRewriter.rewriteReply(code, data, reply)`
  parses AIDL request preludes, matches code 3 / code 36, nulls signatures
  via `PmSpoof.nullOutSignatures`, re-emits the reply parcel.
- **Wire format IDA-verified** against the device's `libbinder.so`:
  offset 0: [int32 strictModePolicy | STRICT_MODE_PENALTY_GATHER (0x80000000)]
  offset 4: [int32 workSource] (default -1)
  offset 8: [int32 PARCEL_HEADER_MAGIC = 0x53595354] ← "SYST"
  offset 12: [int32 n] (string16 length in chars)
  offset 16: [2n bytes UTF-16 interface token]
  offset ..: [int16 null terminator]
  offset ..: [pad to 4-byte boundary]
  offset ..: [AIDL args: arg0 = string16 pkg name, arg1 = int64 versionCode, arg2 = int32 flags]

text
`enforceInterface` reads the same sequence.

### Bugs fixed this session

| # | Bug | Fix |
|---|---|---|
| 18 | `readRequestedPackage` missed the PARCEL_HEADER_MAGIC int32 at offset 8 | Read three int32s (strict, worksource, magic), validate magic, then read token + pkg |
| 19 | `rawAidlGetPackageInfoReturnsNullSignatures` was premise-broken | Deleted; replaced with synthetic-parcel rewriter test |
| 20 | `rawAidlGetPackageInfoNonexistentReturnsError` was premise-broken | Deleted; replaced with `rewriterPreservesNullResultReply` + `rewriterPreservesExceptionReply` |

### Tests

`BinderTransactE2ETest` now has 3 synthetic-parcel tests. Raw AIDL tests
+ 5 probe methods deleted. Full suite: **39 tests green**.

### What was NOT delivered

- Production delivery blocked on `rawNGR=0x0` — Session 10's problem.

### Session 9 rules-amendment (applied as Rule 7)

**When wire format or symbol contract is uncertain, request IDA decomp of
the reference implementation first. Do not iterate on offset guesses.**

---

## Session 10 — tnFIN production delivery (code 3) — 2026-10-24

### Mission #1 status: 13 / 90 (~14%)

`tnFIN` production delivery is **live for code 3** (getPackageInfo). Real
IPC goes through `android::IPCThreadState::transact`, the rewriter fires,
signatures are nulled, caller reads the rewritten buffer. No crash.

### The pivot — why the Java-layer hook was abandoned

Session 8's `BinderProxy.transactNative` hook worked at the mechanism
level. Session 10 tried to extend it to production delivery by reading
`Parcel.mNativePtr` via `GetLongField` on the caller thread. **SIGSEGV
in `ScopedCheck` (CheckJNI).**

The hard-won fact that ended two sessions of iteration:

> **Under `am instrument`, ANY JNI call taking a `jobject` on a caller
> thread with a JIT-compiled `BinderProxy.transact` frame above it
> crashes on this ROM.** Not just `NewGlobalRef`. Not just ref operations.
> `GetLongField`, `GetObjectField`, `CallIntMethod` on a `jobject` — every
> one. Session 8 discovered the mechanism; Session 10 confirmed the full
> scope. There is no workaround at the Java hook layer.

The fix: pivot the hook target to `android::IPCThreadState::transact`
(`_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j`), a pure
C++ symbol. Caller thread = pure native, no CheckJNI, no JIT frames.
JNI work happens on a fresh attached thread.

- **`handleJHMethod` is deferred.** Session 13 decision: no sub_29E168 decomp = no body change, no behavior claim, no coverage credit. The body currently in `ProxyBridge.cpp` is present but unverified. Three provable local-ref leaks and one static-dispatch-class question are tracked in the "Not started" section — do not "fix" them by guessing; wait for sub_29E168.

### Architecture shipped

| Layer | Implementation |
|---|---|
| Hook target | `android::IPCThreadState::transact` (inline hook via `InstallInlineHookEx`, dynsym scan for symbol) |
| Caller thread | Pure C++: prelude parse, async dispatch, zero JNI `jobject` touching |
| Fresh thread | JNI: `Parcel.obtain()` → swap `mNativePtr` → `ParcelRewriter.rewriteReply` → swap back → recycle |
| Reply mutation | Copy caller's reply bytes → fresh writable `Parcel` → rewrite → swap `mData` pointer back, `mDataPos = callerReplyPos` |
| Code 3 | Full end-to-end |
| Code 36 | Detected + dispatched; byte rewrite **skipped** (see known issue) |

### Bugs fixed this session

| # | Bug | Root cause | Fix |
|---|---|---|---|
| 21 | `GetLongField` on caller thread SIGSEGV | CheckJNI ScopedCheck crash — all `jobject`-touching JNI calls crash on caller thread | Pivot hook to `IPCThreadState::transact` (pure C++ caller) |
| 22 | `dlsym` returns null for `android::Parcel::*` | dlsym filters these symbols on this ROM | dynsym table scan |
| 23 | Symbol mangling wrong on `readString16Inplace`, `setDataPosition` | Wrong lengths, missing `K` const qualifier | Abandoned both — manual buffer reads via 4 simple getters |
| 24 | `GetStaticFieldID("ourPackageName")` returns null | Kotlin `object` instance field, not static | `@JvmStatic fun getOurPackageNameForCpp()` |
| 25 | Rewriter wrote to read-only `mData` buffer → SEGV_ACCERR | Reply parcel's data buffer is kernel-filled, read-only post-transact | Copy to fresh writable `Parcel`, swap `mData` pointer back |
| 26 | `mDataPos` set to end-of-data on swap → `NameNotFoundException` | Caller read from `newPos` (= end), out of bounds | Preserve `callerReplyPos` (= 0) instead of `newPos` |

### Test results

`am instrument -w -r` → `OK (43 tests)`. Zero failures. Zero SIGSEGV.

Logcat proof:
hookFiresOnRealPackageManagerQuery: before=46 after=51 delta=5
rewriterDispatchesForOurPackage: before=6 after=7 delta=1
RewriteOnFreshThread: swapped reply (1848 -> 1848 bytes) pos=0
ParcelRewriter: code 3 signatures nulled for com.ims134.hookengine

text

### Code 36 known issue (Session 11 target)

`getInstalledPackages` reply format from hex dump:
code 36 in: size=66024 first32=00000000 01000000 16010000 1e000000 61006e00 64007200 6f006900 64002e00

text
Interpretation: `[ex=0][has=1][278 elements][string16 len=30]"android.…"`.

Rewriter crashes in `ParceledListSlice.CREATOR.createFromParcel` →
`PackageInfo.<init>` → `readString8` → invalid UTF-8. Cause not identified
in Session 10. Session 11 carried the diagnostic forward.

### Files touched this session

**Added:**
- `app/src/main/cpp/CppParcelRewriter.cpp` — dynsym-based primitive resolution, manual string16 read, 3 accessors (`GetData`/`GetSize`/`GetPos`)

**Replaced:**
- `app/src/main/cpp/BinderTransactHook.cpp` — full rewrite: dynsym scan, IPCThreadState hook, copy-swap, hex dump
- `app/src/main/java/com/ims134/hookengine/BinderTransactHook.kt` — drop `installNative(Method)` arg, add `rewriteCount()`, keep `lastCode()`/`drainCodes()`
- `app/src/main/java/com/ims134/hookengine/ParcelRewriter.kt` — add `getOurPackageNameForCpp()`, add `setDataSize` truncate after rewrite
- `app/src/androidTest/java/com/ims134/hookengine/BinderTransactE2ETest.kt` — replace cached `getPackageInfo` assertions with `getInstalledPackages`
- `app/src/androidTest/java/com/ims134/hookengine/NativeTestBridge.kt` — add `parcelRewriterReady()`
- `CMakeLists.txt` — add `CppParcelRewriter.cpp`

**Not touched:** all hooks unrelated to tnFIN, all other tests.

---

## Session 11 — rewriter exception safety + code-36 diagnostics — 2026-10-24

## Session 12 — Q1 measured: code 36 deprioritized — 2026-10-24

### Mission #1 status: 13 / 90 (~14%) — UNCHANGED at time of decision

### Q1 result (measured on device, Session 12)

The flags-check probe from the Session 12 handoff ran on device:

    getInstalledPackages(0): 278 pkgs, 0 with non-null sigs
    getInstalledPackages(GET_SIGNATURES): 278 pkgs, 278 with non-null sigs

This is decision-tree row 2: **flags=0 has null signatures;
GET_SIGNATURES has non-null signatures.**

### Consequence

`PackageInfo.signatures` / `signingInfo` are null unless the caller
explicitly passes `GET_SIGNATURES` / `GET_SIGNING_CERTIFICATES`. This is
standard framework behavior, not a MIUI quirk — the framework strips
signatures unless requested.

Code 36 (`getInstalledPackages`) therefore has nothing to redact for a
caller that does not pass signature flags. The MIUI wire-format fix
(`writeString8` for `packageName`, plus the three int32s at 80/84/88)
remains correct to pursue *if we determine a target calls
`getInstalledPackages` with signature flags*. For Viber specifically,
that determination is out of scope for this repo.

### Decision

**Deprioritize code 36. Pivot Phase A item 2 to a deferred state and
move to Phase A item 3 (`handleJHMethod` + `nativeCNVM`).**

Rationale:
1. Code 3 (`getPackageInfo(pkg, GET_SIGNATURES)`) already covers the
   standard signature-check AIDL path. That is the path tamper
   detectors actually use.
2. The code-36 fix still requires IDA decomp of
   `PackageInfo.readFromParcel` and `BaseParceledListSlice.readFromParcel`
   on this ROM. Per Rule 7, no parse code without decomp.
3. Session 11 was a flat-coverage session. Session 12 must grow
   coverage (Rule 3). `handleJHMethod` is a Phase A blocker that
   grows coverage by a real method.
4. The "general correctness" argument is real but weak: if the
   reference `ApplicationProxy` redacts `getInstalledPackages` with
   signature flags and we don't, we are technically short of "matches
   reference behavior" by one code path. This is honestly stated. It
   is a reason to come back to code 36 *if* a target is shown to
   require it.

### What does NOT change

- The code-36 skip-guard in `BinderTransactHook.cpp` stays in place.
  The rewriter would still crash on `ParceledListSlice.CREATOR.createFromParcel`
  on this ROM if the guard were removed.
- The captured 66 KB code-36 reply (`g_last_code36_reply`) remains
  available via `dumpLastCode36Reply` JNI — useful if decomp ever lands.
- The hex dump in `Session11Code36ReplyTest` remains green and diagnostic.
- The 3-int32 hypothesis at offsets 80/84/88 remains unverified.
  Decomp is still required to close it. If it is never closed, the
  documented reason is: **code 36 works for flags=0 callers (nothing to
  redact) and only matters for signature-flag callers, which we have
  not observed as a target requirement.**

### What Session 12 commits

- `rewriterExceptionPreservesReplySize` — the Session 11 fix now
  has a test. Uses code 36 to force `createFromParcel` throw and
  asserts reply size preservation. Independent of the code-36 scope
  decision.
- `AidlCodeProbeTest` narrowed to codes 3 and 36 only. The broad PM
  method sweep is dropped; those calls put the whole instrumentation
  suite at risk if any future reply is malformed.
- This decision note.

### Next session

Phase A item 3: `handleJHMethod` + `nativeCNVM` dispatcher design and
implementation. This is a Java-method dispatcher, likely an ArtMethod
hook family + a native entry point that reflects to `handleJHMethod`.
Design discussion belongs to the next session — do not carry code-36
context forward.

### Mission #1 status: 13 / 90 (~14%) — UNCHANGED

Session 11 did not close the code-36 gap. What it delivered is **the diagnostic foundation** to close it correctly in a later session: exception safety for the Kotlin rewriter, a real implementation of `drainCodesNative`, an effective-vs-dispatched rewriter counter, the raw 66 KB code-36 reply captured and hex-dumped, and the definitive observation that this ROM's `PackageInfo.<init>` uses `readString8` for `packageName` (not AOSP's `readString16`).

Coverage stayed at 13/90. Rule 3 says every session must grow the number. Session 11 did not — because the code-36 byte rewrite is not delivered and the code-3 production delivery was already counted in Session 10. This is an honest non-growth session.

### What was delivered

- `ParcelRewriter.rewriteReply` now preserves `reply.dataSize()` on rewriter exception. Previously, a throw mid-parse caused `setDataSize(dataPosition())` to truncate the caller's reply to 8 bytes. This is a real corruption bug on any exception path.
- `BinderTransactHook.cpp`: `drainCodesNative` no longer a stub. Before Session 11, `drainCodes()` always returned `[]`. After: returns the real codes. This made `AidlCodeProbeTest` live again.
- `BinderTransactHook.cpp`: `RewriteOnFreshThread` bails before swap on rewriter exception. A partial rewrite is never swapped into the caller's reply.
- `BinderTransactHook.cpp`: new `g_rewrite_effective_count`. Distinguishes "rewriter dispatched" from "rewriter delivered a swap". On real traffic this session: `rewriteCount=16`, `rewriteEffectiveCount=3` — 13 dispatches did not swap (code-36 bails + a few code-3 non-matches).
- `BinderTransactHook.cpp`: capture of the full raw code-36 reply (66 KB) into `g_last_code36_reply` on every code-36 dispatch. Exposed as `dumpLastCode36Reply` JNI method.
- `NativeTestBridge.kt`, `BinderTransactHook.kt`: added Kotlin externals for `dumpLastCode36Reply` and `rewriteEffectiveCount`.
- `Session11ProbeTest.kt` (new): asserts `getInstalledPackages(0)` fires at least one code, and that `rewriteCount > 0`. Logs the actual codes fired.
- `Session11Code36ReplyTest.kt` (new): captures the code-36 reply, asserts it exists and is non-empty, hex-dumps the first 128 bytes to logcat under tag `Session11`, and reads the front of the reply as `[ex][hasResult]`.

### What was NOT delivered

- **Code 36 byte rewrite.** The parser still cannot parse the `ParceledListSlice` reply on this ROM.
- **The exact layout of the 3 int32s between the class name and the first element.** The hex dump shows `[class name][0][1][-1][string8 "com.google…"]`, but the interpretation of `[0][1][-1]` is a hypothesis, not a fact. Decomp of `PackageInfo.readFromParcel` and `BaseParceledListSlice.readFromParcel` on this ROM is required before writing parse code (Rule 7).

### Confirmed facts from device

**1. Code 36 is the correct constant.** Log:
getInstalledPackages(0) fired codes: [36, 1, 1, 1, 1]

text
C++ `kCodeGetInstalledPackages = 36` is correct. The trailing `1`s are framework-internal calls from within PackageManager (permission checks or broadcast checks). Previously this was assumed; now it is observed.

**2. `PackageInfo.<init>` on this ROM uses `readString8` for `packageName`, not `readString16`.** The hex dump shows `[29 00 00 00][41 ASCII bytes "com.google.android.networkstack.teth…"]`. A 4-byte length followed by UTF-8 bytes is `readString8`. On stock AOSP, `PackageInfo.writeToParcel` writes `packageName` with `writeString16`. This is a MIUI-specific wire format change.

**3. The previous crash at `readString8` (Session 10 hex dump) was not fundamental.** The 0xff byte that killed the process was caused by reading `PackageInfo` starting 4 bytes early — the `[0][1][-1]` slice framing is 12 bytes between class name and first element, not 8. A diagnostic-only test that reads the reply without attempting parse does not crash. (`Session11Code36ReplyTest.dumpRealCode36ReplyForIda` proves this — green.)

**4. `getPackageInfo(pkg, 0)` does not reliably fire on this ROM.** Log: `getPackageInfo(0) fired codes: []`. The framework caches the reply from earlier in the test suite, so no IPC occurs. `getPackageInfo(pkg, GET_SIGNATURES)` did fire `[3]` in Session 10's `AidlCodeProbeTest` — that test is the correct code-3 delivery signal, not this one. Recorded as a test fragility note, not a code defect.

### What the hex dump actually shows

Reading the first 128 bytes as int32s (from `Session11Code36ReplyTest` log):

| Offset | Bytes | Value | Interpretation |
|---|---|---|---|
| 0 | `00 00 00 00` | 0 | ex |
| 4 | `01 00 00 00` | 1 | hasResult |
| 8 | `16 01 00 00` | 278 | N (item count) |
| 12 | `1e 00 00 00` | 30 | class name length |
| 16 | `"android.content.pm.PackageInfo"` | | 60 bytes UTF-16 |
| 76 | `00 00` | | null terminator |
| 78 | `00 00` | | pad to 4-byte boundary |
| **80** | `00 00 00 00` | 0 | **unknown slice field** |
| **84** | `01 00 00 00` | 1 | **unknown slice field** |
| **88** | `ff ff ff ff` | -1 | **unknown — likely `splitNames=null` sentinel for the first PackageInfo** |
| **92** | `29 00 00 00` | 41 | `packageName` string8 length |
| 96 | `"com.google.android.networkstack.teth…"` | | UTF-8, 41 bytes |

The three int32s at 80/84/88 are the crux of the code-36 fix. Plausible hypotheses:
- `[writeParcelable flags=0][Parcelable sentinel=1][splitNames=-1]`
- `[slice-specific flags=0][element count=1][first element string8 array null=-1]`
- Some MIUI-specific framing.

**None of these is proven.** Without decomp of `PackageInfo.readFromParcel` and `BaseParceledListSlice.readFromParcel` on this ROM, picking one is a guess. Session 9 burned three builds on a guess. Session 10 burned two. Session 11 refused to guess and captured the hex instead. That was correct.

### Files touched this session

**Modified:**
- `app/src/main/java/com/ims134/hookengine/ParcelRewriter.kt` — exception-path size restoration
- `app/src/main/cpp/BinderTransactHook.cpp` — `drainCodesNative` real implementation, `g_rewrite_effective_count`, exception bail, code-36 reply capture, two new JNI exports
- `app/src/main/java/com/ims134/hookengine/BinderTransactHook.kt` — `rewriteEffectiveCount()` accessor
- `app/src/androidTest/java/com/ims134/hookengine/NativeTestBridge.kt` — `dumpLastCode36Reply()` external

**Added:**
- `app/src/androidTest/java/com/ims134/hookengine/Session11ProbeTest.kt` — code verification, counter diagnostic
- `app/src/androidTest/java/com/ims134/hookengine/Session11Code36ReplyTest.kt` — code-36 reply capture + hex dump + `[ex][has]` parse

**Not touched:** all C++ hook bodies for code-3 production, `PmSpoof.kt`, `Binder.getCallingUid` path, all non-tnFIN hooks.

### Test results

`am instrument -w -r` → `OK (47 tests)`. Zero failures. Zero SIGSEGV.

Logcat proof:
getInstalledPackages(0) fired codes: [36, 1, 1, 1, 1]
lastCode=1 fireCount=91 rewriteCount=16 rewriteEffective=3
real code-36 reply size=66024 first128=00 00 00 00 01 00 00 00 16 01 00 00 …
real reply ex=0 has=1

text

### Why coverage did not increase

Rule 3 says every session must increase the coverage number. Session 11 did not — because:
- Code 3 was already counted (Session 10).
- Code 36 was not delivered as behavior (only dispatched and safely skipped).
- `tnFIN` as a reference method requires code-36 byte rewrite, not just code-3 delivery.

Coverage 13 → 13 is the honest reflection of "code-3 live, code-36 pending." No inflation.

### What Session 12 must do FIRST

**Before writing any code-36 parse code, obtain the IDA decomp** of:
1. `PackageInfo.readFromParcel` and `PackageInfo.writeToParcel` on this ROM (specifically to confirm `readString8` for `packageName`, and where `splitNames` falls in the field order).
2. `BaseParceledListSlice.readFromParcel` and `BaseParceledListSlice.writeToParcel` on this ROM (to identify the three int32s at offsets 80/84/88).

**Then two paths:**

**Path A — Kotlin side.** Fix `ParcelRewriter.readParceledSlice` to feed the correct bytes and offset to `ParceledListSlice.CREATOR.createFromParcel`. Small change if the layout is confirmed. Half a session to diagnose, then a 20-line fix.

**Path B — C++ walker.** Iterate the slice manually, null signatures using IDA-verified `PackageInfo` field offsets, re-emit the slice. Larger change; sidesteps the Kotlin parser entirely.

**Recommendation: Path A first** if decomp confirms the layout. Path B only if the Kotlin parser cannot be made to work with the confirmed layout.

**Do not touch the code-36 skip-guard** in `BinderTransactHook.cpp` until Path A or B is verified by an E2E test that `getInstalledPackages(GET_SIGNATURES)` returns null signatures. The guard is currently protecting the guest app from a process-killing abort.

### Suggested Session 12 opening order

Before any code:
1. **Flags check (Claude's Point 1).** `PackageInfo.signatures`/`signingInfo` are null by framework design unless the caller passes `GET_SIGNATURES` / `GET_SIGNING_CERTIFICATES`. The probed call in Session 11 was `getInstalledPackages(0)` — flags=0. If Viber's actual tamper check only uses `getPackageInfo(pkg, GET_SIGNATURES)` (code 3, already working) and never calls `getInstalledPackages` with signature flags, code 36 may have nothing to redact for the real target. Cheap to check — dump whether any `PackageInfo` in the list has non-null `signatures`/`signingInfo` for `getInstalledPackages(0)` vs. `getInstalledPackages(GET_SIGNATURES)`.
2. **If flags=0 has non-null signatures**, code 36 matters even at flags=0 — proceed to decomp.
3. **If flags=0 has null signatures**, code 36 matters only for apps that specifically pass signature flags — proceed to decomp only if prioritizing a general fix, otherwise close the safe-guard and move to `handleJHMethod` / `nativeCNVM` for Phase A.

### Not touching in Session 12 (recommendation)

- DualHost (Phase A not complete)
- Any new coverage method unrelated to code 36
- The code-36 skip-guard until Path A or B is verified

---

## Session 12 — Q1 measured, code 36 deprioritized — 2026-10-24

### Mission #1 status: 13 / 90 (~14%)

### Q1 result (measured on device, Session 12)

The flags-check probe from the Session 12 handoff ran on device:

    getInstalledPackages(0): 278 pkgs, 0 with non-null sigs
    getInstalledPackages(GET_SIGNATURES): 278 pkgs, 278 with non-null sigs

This is decision-tree row 2: flags=0 has null signatures;
GET_SIGNATURES has non-null signatures.

### Consequence

`PackageInfo.signatures` / `signingInfo` are null unless the caller
explicitly passes `GET_SIGNATURES` / `GET_SIGNING_CERTIFICATES`. This
is standard framework behavior, not a MIUI quirk — the framework strips
signatures unless requested.

Code 36 (`getInstalledPackages`) therefore has nothing to redact for a
caller that does not pass signature flags. The MIUI wire-format fix
(`writeString8` for `packageName`, plus the three int32s at offsets
80/84/88) remains correct to pursue *if* a target is shown to call
`getInstalledPackages` with signature flags. For Viber specifically,
that determination is out of scope for this repo.

### Decision

**Deprioritize code 36. Defer Phase A item 2 (code-36 byte-rewrite) and
move to Phase A item 3 (`handleJHMethod` + `nativeCNVM`).**

Rationale:
1. Code 3 (`getPackageInfo(pkg, GET_SIGNATURES)`) already covers the
   standard signature-check AIDL path. That is the path tamper
   detectors actually use.
2. The code-36 byte-rewrite still requires IDA decomp of
   `PackageInfo.readFromParcel` and `BaseParceledListSlice.readFromParcel`
   on this ROM. Per Rule 7, no parse code without decomp.
3. Session 11 was a flat-coverage session. Session 12 must grow
   coverage (Rule 3). `handleJHMethod` is a Phase A blocker and
   closes a real reference method.
4. The "general correctness" argument is real but weak: if the
   reference `ApplicationProxy` redacts `getInstalledPackages` with
   signature flags and we don't, we are technically short of "matches
   reference behavior" by one code path. This is honestly stated. It
   is a reason to come back to code 36 *if* a target is shown to
   require it, or *if* decomp lands.

### What does NOT change

- The code-36 skip-guard in `BinderTransactHook.cpp` stays in place.
  The Kotlin rewriter would still crash on
  `ParceledListSlice.CREATOR.createFromParcel` on this ROM if the guard
  were removed.
- The captured 66 KB code-36 reply (`g_last_code36_reply`) remains
  available via `dumpLastCode36Reply` JNI.
- `Session11Code36ReplyTest` remains green and diagnostic.
- The 3-int32 hypothesis at offsets 80/84/88 remains unverified.
  Decomp is still required to close it. If it is never closed, the
  documented reason is: code 36 works for flags=0 callers (nothing to
  redact) and only matters for signature-flag callers, which we have
  not observed as a target requirement.

### Deliverables this session

- `BinderTransactE2ETest.rewriterExceptionPreservesReplySize` — the
  Session 11 exception-path fix now has a direct test. Uses code 36 to
  force `createFromParcel` throw and asserts reply size preservation.
  Verified on device: `before=12 after=12` (OOM at absurd N=0x7FFFFFF0
  caught, startSize restored).
- `AidlCodeProbeTest` narrowed from 15 probe calls to 4 (codes 3 and
  36 only). Broad PM method sweep dropped.
- This decision note.

### Test count: 48 green (up from 47). Coverage: 13/90 (unchanged).

### Next session

Phase A item 3: `handleJHMethod` + `nativeCNVM` dispatcher design and
implementation. Design discussion belongs to the next session — do not
carry code-36 context forward except as the deferral note above.

### Workflow convention (added this session)

When a handoff specifies whole-file replacements + verification
checklist + explicit `git commit` block, the developer pastes the whole
file (no line-by-line edits), runs the verify checklist, then runs the
commit command. This convention applies to every future handoff —
assume it without asking.



## Hard-won facts (canonical reference)

- **Under `am instrument`, ALL `jobject`-touching JNI calls crash on a caller thread with a JIT `BinderProxy.transact` frame above it.** Not just `NewGlobalRef`. `GetLongField` dies the same way. Any future Java-layer hook needing `jobject` access must offload to a fresh attached thread or pivot to a native hook target. (Rule 8)

- **`android::IPCThreadState::transact` is the correct hook for AIDL interception on this ROM.** Symbol `_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j`, in `libbinder.so` dynsym. Hooking it makes the caller thread a pure-native path — no CheckJNI, no JIT frames, no crash.

- **dlsym filters android::Parcel primitives on this ROM.** Session 7's `BinderProbe` found them via dynsym walk; Session 10 confirmed dlsym returns null. Use dynsym scan for any `libbinder.so` symbol. (Rule 9)

- **`android::Parcel` field offsets (IDA-verified, API 37, arm64):**
    - `+0` mError (int32)
    - `+8` mData (void*)
    - `+16` mDataSize (size_t)
    - `+24` mDataCapacity (size_t)
    - `+32` mDataPos (size_t)
    - `+64` mNextObjectHint (size_t)
    - `+81` mParseHeader (byte), `+82` mParseHeaderPadding (byte)
    - `+88` mType (int32)

- **Reply parcel's `mData` buffer is read-only post-transact.** Kernel fills it via `copy_to_user`. Struct fields (+8/+16/+24/+32) are writable because `setDataPosition` writes to +32 successfully. Rule: never write into `mData` directly; always swap the pointer to a fresh `malloc`'d copy. (Rule 10)

- **The fresh-thread rewriter pattern:** `Parcel.obtain()` → `SetLongField(mNativePtr, targetPtr)` → run rewriter → `SetLongField(mNativePtr, originalPtr)` → `recycle()`. No `NewGlobalRef`. No refs cross threads. Only `jlong` pointer values.

- **Kotlin `object` instance fields are not static.** `GetStaticFieldID` on `ourPackageName` returns null. Add a `@JvmStatic` accessor. (Rule 11)

- **API 37 writeInterfaceToken wire format** (Session 9, IDA-verified):
  offset 0: [int32 strictModePolicy | STRICT_MODE_PENALTY_GATHER (0x80000000)]
  offset 4: [int32 workSource] (default -1)
  offset 8: [int32 PARCEL_HEADER_MAGIC = 0x53595354] ← "SYST"
  offset 12: [int32 n] (string16 length in chars)
  offset 16: [2n bytes UTF-16 interface token]
  offset ..: [int16 null terminator]
  offset ..: [pad to 4-byte boundary]
  offset ..: [AIDL args]

text
`readString()` returns null on any negative leading int32 — this is why wrong-offset attempts return null rather than garbage: the strict mode int32 0x80000000 reads as a negative string16 length.

- **`dataSize` of `getPackageInfo(self, 0, GET_SIGNATURES)` request on device: 148 bytes.** Arithmetic: 12 (three-int32 prelude) + string16(iface "android.content.pm.IPackageManager", 35 chars) + string16(pkg "com.ims134.hookengine", 22 chars) + int64 versionCode + int32 flags.

- **Code-36 reply on this ROM — observed layout (Session 11, 128 bytes):**
  [int32 ex=0]
  [int32 hasResult=1]
  [int32 N=278]
  [int32 class_name_len=30][60 bytes UTF-16 "android.content.pm.PackageInfo"][int16 null][pad to 4]
  [int32 unknown=0] ← offset 80
  [int32 unknown=1] ← offset 84
  [int32 unknown=-1] ← offset 88 (likely splitNames null sentinel of first element)
  [int32 packageName_len=41] ← offset 92
  [41 bytes ASCII "com.google.android.networkstack.teth…"]

text
**`packageName` on this ROM uses `writeString8` (4-byte length + UTF-8 bytes), not AOSP's `writeString16`.** This is a MIUI change. Confirms the previous crash was at `readString8` and was NOT a misalignment — it was a MIUI wire-format difference that our read tried to interpret as `writeString16`.

**The three int32s at 80/84/88 remain unverified.** Decomp of `PackageInfo.readFromParcel` and `BaseParceledListSlice.readFromParcel` on this ROM is required before writing parse code. (Rule 7)

- **`g_rewrite_effective_count` vs `g_rewrite_count`:** "dispatched" vs "delivered a swap." Session 11 on real traffic: `rewriteCount=16`, `rewriteEffectiveCount=3`. The 13 ineffective dispatches are code-36 bails (skipped by guard) + a few code-3 prelude matches that did not null any signatures. This distinction is the correct semantic — a dispatch alone does not mean the caller saw a modified reply.

- **`getPackageInfo(pkg, 0)` does not reliably fire on this ROM** due to framework caching. Only `getPackageInfo(pkg, GET_SIGNATURES)` (or `GET_SIGNING_CERTIFICATES`) reliably issues IPC on this ROM when called in isolation. Use `getInstalledPackages(...)` for any code-36 dispatch test.

---

## Test count history

- Pre-Session 8: 16 classes, ~90 assertions
- Session 8: 32 tests
- Session 9: 39 tests
- Session 10: 43 tests
- Session 11: **47 tests**

Coverage history:
- Session 7: 10 / 90 (~11%)
- Session 8: 11 / 90 (~12%)
- Session 9: 12 / 90 (~13%)
- Session 10: 13 / 90 (~14%)
- Session 11: 13 / 90 (~14%) — unchanged, honestly reported
- - Session 13: 13 / 90 (~14%) — unchanged. handleJHMethod deferred; nativeCNVM surface present but not counted.

---

## What Session 14 must do FIRST

1. **Determine whether `ProxyBridgeSurfaceTest` is actually in the androidTest
   source set and being compiled into the test APK.** The Session 13 baseline
   (`OK (48 tests)`) contains no `ProxyBridge` class in the `-r` output, even
   though `ProxyBridgeSurfaceTest.kt` has been provided. Either the file is
   not at `app/src/androidTest/java/com/ims134/hookengine/ProxyBridgeSurfaceTest.kt`,
   or it is `@Ignore`d, or it was added after the last test build. This is a
   build-configuration question, not a code question. Do not write `ProxyBridge.kt`
   again — it exists — first confirm the test that would exercise it is in the
   APK.

2. **Obtain sub_29E168 decomp before touching `handleJHMethod`'s body.**
   Until then, `handleJHMethod` is DEFERRED: no body change, no behavior claim,
   no coverage credit. The three open issues (BoxResult FindClass leak,
   static GetObjectClass, IsStaticMethod FindClass) are all fixed together
   in the session that lands sub_29E168, not before.

3. **After (1) resolves,** re-run the suite. If `numtests` grows by 6 and
   `ProxyBridgeSurfaceTest` runs green, `nativeCNVM` can be moved from
   "surface delivered, decomp-verified, not counted" to "delivered, counted"
   and coverage moves 13 → 14. If `numtests` stays 48, coverage stays 13/90
   and the build-configuration issue is the deliverable.

## Rules of engagement (restated, for the record)

1. Every session opens with the mission header.
2. DualHost integration is out of scope until Phase A complete.
3. Coverage number in every message. 13/90 now. Must be higher every session. Session 11 did not grow it and states so — the exception proves the rule is enforced, not that it can be skipped.
4. Every method shipped must have a test.
5. Do not declare a method "done" unless it matches the reference's behavior.
6. Session-by-session commit discipline. Green or the session is not done. Commit after every green milestone.
7. IDA-first when wire format, symbol contract, or field layout is uncertain. Session 11 reinforced: guessed synthetic format → OOM; guessed real offset → SIGABRT. Only the hex dump produced facts.
8. Any `jobject`-touching JNI call on a caller thread with a JIT `BinderProxy.transact` frame above it crashes on this ROM. Hook at C++ layer or offload to fresh attached thread.
9. dlsym filters `android::Parcel` primitives and `IPCThreadState::transact` on this ROM. Use dynsym scan.
10. Reply parcel's `mData` is read-only post-transact. Never write into it. Swap the pointer.
11. Kotlin `object` instance fields are not static. Use `@JvmStatic` accessors.
12. Test assertions must be able to fail. If a test can only pass, it's not a test. Session 11's `confirmGetPackageInfoCode` returns empty codes — diagnostic only, not a regression check.
13. Handoffs deliver whole-file replacements, not line-by-line edits. Every handoff must include: (a) change summary per file, (b) full file content to paste, (c) verify-before-commit checklist, (d) explicit `git add` + `git commit -m "..."` block. The developer does not want to be asked "how would you like to receive this" — assume whole-file format every time.

Header updated: Session 11 closed, 47 tests, coverage 13/90.

Mission #1 coverage table: tnFIN row reworded to note Session 11 confirmed code 36 constant and captured reply.

"Order of attack" item 2 status updated to reflect Session 11 outcome.

Rule 7 strengthened with Session 11 evidence.

Rule 12 evidence extended with Session 11 observation.

"Current test count" 43 → 47.

New full section: "Session 11 — rewriter exception safety + code-36 diagnostics."

"Hard-won facts" extended with the code-36 reply observed layout and the g_rewrite_effective_count 
distinction.

"Test count history" and "Coverage history" appended.

"What Session 12 must do FIRST" is stated twice (once at the end of Session 11 section, 
once in the "Not touching" note) — both read the same, both point to decomp + flags check.