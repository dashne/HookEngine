```
## Status

Last verified build: v29, verified on device 2026-10-23, items 1–5 all green (23 tests), Item 7 §7.1–§7.5 green on device.

Session 10 closed 2026-10-24: tnFIN production delivery live for code 3 (getPackageInfo). 43 tests green. Coverage 13/90.

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
- Mission #1 Phase A: tnFIN mechanism (Session 8), rewriter algorithm (Session 9), production delivery for code 3 (Session 10).

Pending:
- Mission #1 Phase A: tnFIN code 36 byte-rewrite (Session 11 target).
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
| `tnFIN` | `BinderTransactHook` + `ParcelRewriter` + `CppParcelRewriter` | ✅ Production delivery for code 3 (getPackageInfo). Code 36 (getInstalledPackages) detected/dispatched but byte-rewrite skipped — see Session 10 §Code 36 known issue. |
| `nativeAIOR` | Frida port block | ⚠️ Partial — 1 category of N |
| `nativeDNPO` | TracerPid filter | ⚠️ Partial — misses full ptrace detect |
| `nativeAMRHook` | Path hide list | ⚠️ Partial — no mount spoof |

**Not started (77 methods):**
- **`handleJHMethod`** + `nativeCNVM` — Java method dispatcher
- **`nativeInit`** (9 params) — full context config
- **`nativeOnLoad`** (full DEX extract) — currently partial
- **tnFIN code 36 byte-rewrite** — code 36 dispatches but does not rewrite (Session 11 target)
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

**Phase A — Core engine (blocking everything else):**
1. ✅ `tnFIN` — mechanism (Session 8), algorithm (Session 9), production code 3 (Session 10)
2. 🔲 `tnFIN` code 36 byte-rewrite (Session 11 target)
3. 🔲 `handleJHMethod` + `nativeCNVM` (Java dispatcher)
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
7. **When wire format, symbol contract, or field layout is uncertain, request IDA decomp of the reference implementation before writing parse code.** Do not iterate on offset guesses. Session 9 burned three rebuild cycles on this. Session 10 confirmed it — dynsym scan for symbols, IDA-verified field offsets for Parcel struct, no guessing.
8. **On this ROM, any JNI call taking a `jobject` on a caller thread with a JIT-compiled `BinderProxy.transact` frame above it crashes under CheckJNI.** Not just `NewGlobalRef`. Not just ref operations. `GetLongField`, `GetObjectField`, `CallIntMethod` on a `jobject` — every one. Hook at the C++ layer (`IPCThreadState::transact`) or offload JNI to a fresh attached thread. Session 8 discovered for `NewGlobalRef`; Session 10 confirmed for `GetLongField`. Do not re-discover.
9. **dlsym(RTLD_DEFAULT, ...) filters `android::Parcel` primitives and `IPCThreadState::transact` on this ROM.** Use dynsym scan (the `BinderProbe.cpp` / `CppParcelRewriter.cpp` pattern) for any `libbinder.so` symbol. dlsym returns null; do not treat null as "symbol does not exist."
10. **The reply parcel's `mData` buffer is read-only post-transact.** Kernel fills it via `copy_to_user`. Never write into it. Copy to a fresh `malloc`'d buffer, rewrite there, swap the pointer back into the struct (struct fields +8/+16/+24/+32 are writable — `setDataPosition` writes +32 successfully).
11. **Kotlin `object` instance fields are not static.** `GetStaticFieldID` on `ourPackageName` returns null. Add a `@JvmStatic` accessor. Session 10 bug.
12. **Test assertions must be able to fail.** If a test can only pass, it is not a test. Session 9's `rawAidlGetPackageInfoReturnsNullSignatures` was premise-broken (server returned null anyway); Session 10's `hookFiresOnRealPackageManagerQuery` was cached. Fix the test, not the assertion.

### Current test count: 43 green

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

## Session 8 — tnFIN mechanism delivered — 2026-10-24

### Mission #1 status: 11 / 90 (~12%)

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
  ```
offset 0:  [int32 strictModePolicy | STRICT_MODE_PENALTY_GATHER (0x80000000)]
offset 4:  [int32 workSource]                    (default -1)
offset 8:  [int32 PARCEL_HEADER_MAGIC = 0x53595354]  ← "SYST"
offset 12: [int32 n]                             (string16 length in chars)
offset 16: [2n bytes UTF-16 interface token]
offset ..: [int16 null terminator]
offset ..: [pad to 4-byte boundary]
offset ..: [AIDL args: arg0 = string16 pkg name, arg1 = int64 versionCode, arg2 = int32 flags]
  ```
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
```
hookFiresOnRealPackageManagerQuery: before=46 after=51 delta=5
rewriterDispatchesForOurPackage: before=6 after=7 delta=1
RewriteOnFreshThread: swapped reply (1848 -> 1848 bytes) pos=0
ParcelRewriter: code 3 signatures nulled for com.ims134.hookengine
```

### Code 36 known issue (Session 11 target)

`getInstalledPackages` reply format from hex dump:
```
code 36 in: size=66024 first32=00000000 01000000 16010000 1e000000 61006e00 64007200 6f006900 64002e00
```
Interpretation: `[ex=0][has=1][278 elements][string16 len=30]"android.…"`.

Rewriter crashes in `ParceledListSlice.CREATOR.createFromParcel` →
`PackageInfo.<init>` → `readString8` → invalid UTF-8. Cause not identified.
Byte rewrite for code 36 is **skipped** — caller sees the server's
valid reply unchanged. Session 11 needs IDA decomp of
`PackageInfo::writeToParcel` / `BaseParceledListSlice::writeToParcel` on
this ROM before fixing.

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
  ```
offset 0:  [int32 strictModePolicy | STRICT_MODE_PENALTY_GATHER (0x80000000)]
offset 4:  [int32 workSource]                    (default -1)
offset 8:  [int32 PARCEL_HEADER_MAGIC = 0x53595354]  ← "SYST"
offset 12: [int32 n]                             (string16 length in chars)
offset 16: [2n bytes UTF-16 interface token]
offset ..: [int16 null terminator]
offset ..: [pad to 4-byte boundary]
offset ..: [AIDL args]
  ```
  `readString()` returns null on any negative leading int32 — this is why wrong-offset attempts return null rather than garbage: the strict mode int32 0x80000000 reads as a negative string16 length.

- **`dataSize` of `getPackageInfo(self, 0, GET_SIGNATURES)` request on device: 148 bytes.** Arithmetic: 12 (three-int32 prelude) + string16(iface "android.content.pm.IPackageManager", 35 chars) + string16(pkg "com.ims134.hookengine", 22 chars) + int64 versionCode + int32 flags.

---

## Session 11 — the next block

### Target: close the code 36 gap (tnFIN Phase A completion)

**Prerequisite (Rule 7 — probe before write):** IDA decomp of
- `PackageInfo::writeToParcel` (or the AIDL-generated equivalent on this ROM)
- `BaseParceledListSlice::writeToParcel` (or `ParceledListSlice::writeToParcel`)

### Two paths to evaluate once decomp is in hand

**A) Fix the Kotlin side.** The crash is in `ParceledListSlice.CREATOR.createFromParcel`,
the same call AOSP uses. If it crashes, either the C++ prelude `[ex=0][has=1]`
is slightly off, or this ROM added a wrapper. Fix is in
`ParcelRewriter.readParceledSlice`, not in the C++ shim. Half a session to
diagnose.

**B) Write a C++ `ParceledListSlice` walker.** Reads count, iterates N
entries, finds `PackageInfo` boundaries, nulls signatures in-place using
IDA-verified field offsets, re-emits the slice. Requires
`PackageInfo::writeToParcel` to be exported.

### Recommendation

Probe the Kotlin path first (synthetic test against a real
`getInstalledPackages` reply). If it also crashes, AOSP is broken on this
ROM (impossible) → we're reading wrong offset in the C++ prelude. If it
succeeds, our C++ prelude or swap is at fault.

### Not touching this session

- DualHost (Phase A not complete)
- `handleJHMethod` / `nativeCNVM` (Session 12)
- Any new coverage method

**One method per session discipline.** Session 11 = code 36 only.

### What to do FIRST in the new chat

Paste this file, then open with:

> "Continuing HookEngine / Mission #1. Coverage 13/90. Session 10 closed: tnFIN production delivery live for code 3. Hook target pivoted to `android::IPCThreadState::transact` (C++ symbol, dynsym-scanned, pure-native caller thread) after Java-layer `transactNative` hook was abandoned — CheckJNI ScopedCheck crashes ANY `jobject`-touching JNI call on the caller thread (Session 8 symptom extended to GetLongField in Session 10). Reply parcel's `mData` buffer is read-only post-transact; solved by copy-to-fresh-Parcel + `mData` pointer swap. 43 tests green, no SIGSEGV. Code 36 (getInstalledPackages) dispatched but byte-rewrite skipped — `ParceledListSlice` parse crashes on this ROM. Session 11 target: code 36. Here is the IDA decomp of `PackageInfo::writeToParcel` and `BaseParceledListSlice::writeToParcel`:"

Then paste the IDA decomp of:
1. `PackageInfo::writeToParcel` (or AIDL equivalent)
2. `BaseParceledListSlice::writeToParcel` (or `ParceledListSlice::writeToParcel`)
3. Optionally `PackageInfo::readFromParcel` and `BaseParceledListSlice::readFromParcel` — to understand the format end-to-end

---

## Rules of engagement (restated, for the record)

1. Every session opens with the mission header.
2. DualHost integration is out of scope until Phase A complete.
3. Coverage number in every message.
4. Every method shipped must have a test.
5. Do not declare a method "done" unless it matches the reference's behavior.
6. Session-by-session commit discipline. Green or the session is not done. Commit after every green milestone.
7. IDA-first when wire format, symbol contract, or field layout is uncertain.
8. Any `jobject`-touching JNI call on a caller thread with a JIT `BinderProxy.transact` frame above it crashes on this ROM. Hook at C++ layer or offload to fresh attached thread.
9. dlsym filters `android::Parcel` primitives and `IPCThreadState::transact` on this ROM. Use dynsym scan.
10. Reply parcel's `mData` is read-only post-transact. Never write into it. Swap the pointer.
11. Kotlin `object` instance fields are not static. Use `@JvmStatic` accessors.
12. Test assertions must be able to fail. If a test can only pass, it's not a test.
```