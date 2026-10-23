## Status

Last verified build: v29, verified on device 2026-10-23, items 1–5 all green (23 tests), Item 7 §7.1–§7.5 green on device.

Completed:
- Registry finding DISPROVED — RegistryCount() counts inline hooks, regDuring=1 observed. Assertion tightened in both directions.
- §7.1 — 23 sites migrated to testreport::Report, logcat byte-identical.
- §7.2 — test_suite.cpp dispatch, per-item harness runners extracted. Logcat byte-identical.
- §7.3 — JNI bridge (NativeTestBridge.runSuite / uninstallAll / setItem4Paths).
- §7.4 — NativeTestBridge.kt Kotlin object.
- §7.5 — NativeItemTest.kt parameterized runner. 5/5 green on device via am instrument.

Pending:
- §7.6 — regression suites (regression_bug1..5).
- §7.7 — selftest.txt file-based channel + IntegrationTest.kt.
- §7.9 — build-only CI workflow + README limitations section.

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

1. File-based result channel, not logcat scraping (§7.7, pending). RemoteService writes filesDir/selftest.txt; the integration test polls the file. Logcat stays as the human workflow. Required because logcat triple-counts.
2. C++ stays in C++. Primitives (ReadPageProt, AllocateMemoryNear, ShouldHookDlopen) live natively. Regression checks for Bugs 1, 2, 4, 5 will be C++. Bug 3 is Kotlin-side (§7.6, pending).
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

### §7.6 — PENDING

Regression suites for historical bugs 1–5, wired into test_suite.cpp:

| Suite | Bug | Location |
|---|---|---|
| regression_bug1 | HookJavaMethod destroyed page write permission | C++ |
| regression_bug2 | Fake_dlopen inline rescan caused execmod denials | C++ — assert ShouldHookDlopen() == false |
| regression_bug3 | MIUI routes through sPackageManager, not mPM | Kotlin — reflection on both fields |
| regression_bug4 | relay page leak (~510 installs → AllocateMemoryNear fails) | C++ — 1000 cycles at grace=0, relay_alloc == relay_free |
| regression_bug5 | trampoline lifetime under contention | C++ — install, hammer, uninstall grace=20ms, counters balanced |

test_suite.cpp already declares extern "C" int RunRegressionBugN(); for all five — bodies not yet written.

### §7.7 — PENDING

RemoteService writes filesDir/selftest.txt (truncate at start, PASS|name|detail per line, final END <pass> <fail>). IntegrationTest.kt polls the file, asserts 23 PASS| lines, 0 FAIL|, END marker present.

### §7.8 — CLOSED 2026-10-23

app/build.gradle.kts:
- testInstrumentationRunner "androidx.test.runner.AndroidJUnitRunner"
- testBuildType "debug"
- ndk { abiFilters += "arm64-v8a" } — required: test files contain ARM64 inline assembly and do not compile for x86/x86_64/armv7.
- androidTestImplementation for androidx.test.ext:junit:1.2.1, androidx.test:runner:1.6.2, androidx.test:rules:1.6.1

### §7.9 — PENDING

.github/workflows/test.yml, build-only:

name: build
on: [push, pull_request]
jobs:
build:
runs-on: ubuntu-latest
steps:
- uses: actions/checkout@v4
- uses: actions/setup-java@v4
with: { distribution: temurin, java-version: '17' }
- uses: android-actions/setup-android@v3
- run: ./gradlew :app:assembleDebug :app:assembleDebugAndroidTest

Compiles the harness. Does not run hooks. No CI task can verify device tests on this device.

---

## Assumed symbols — verified against actual headers

| Assumed | Verified |
|---|---|
| InstallInlineHookEx(void*, void*, void**, const char*) returns HookResult with HOOK_OK=0 | matches HookRegistry.h |
| UninstallInlineHook(void*) returns HookResult | yes |
| SetUninstallGraceMs(long) exists | yes, InlineHooker.cpp |
| RunItemNTests() callable | refactored to shared RunItemNInternal in §7.2 |
| Native lib name is hookengine | yes |
| installPmSignatureSpoof callable from test context | NO — private to RemoteService. Bug 3 regression (§7.6) needs extraction or duplicate logic. |
| RegistryCount() excludes inline hooks | DISPROVED — counts them |

---

## Execution order (historical)

1. ~~Confirm registry diagnosis~~ — DONE. Result: regDuring=1, finding disproved.
2. ~~Refactor 23 sites to Report, verify logcat byte-identical~~ — DONE §7.1.
3. ~~Add test_suite.cpp, extract per-item runners, verify logcat byte-identical~~ — DONE §7.2.
4. ~~Add JNI bridge + NativeTestBridge.kt, verify androidTest APK builds~~ — DONE §7.3/§7.4.
5. ~~Add NativeItemTest.kt, run am instrument, expect 5/5~~ — DONE §7.5.
6. Add RegressionTest.kt + C++ regression suites — NEXT (§7.6).
7. Add selftest.txt to RemoteService + IntegrationTest (§7.7).
8. Build-only CI (§7.9), update README limitations.

---

## What Item 7 does not cover

- Per-function granularity inside arm64_reloc (Phase 2).
- Bug 2 verification beyond ShouldHookDlopen() == false.
- Any device other than the API 37 / arm64 physical device.
- Runtime CI. Build-only.
- connectedDebugAndroidTest as a verification path — the task is broken on MIUI.

---


---

## §7.6 — CLOSED 2026-10-23

Regression suites for historical bugs 1–5. Device verification:
`am instrument -w -r` → `numtests=10`, all suites
`INSTRUMENTATION_STATUS_CODE: 0`, final `OK (10 tests)`.

Breakdown:

| Suite | Location | Assertions |
|---|---|---|
| bug1_page_prot_restored | C++ `native-lib.cpp` | ArtMethod page prot before == install == restore |
| bug2_dlopen_hooks_disabled | C++ `test_regression.cpp` | 9 paths match `ShouldHookDlopen` expectation |
| bug3_both_pm_fields_patched | Kotlin `RegressionTest.kt` | both `mPM` and `sPackageManager` hold same proxy class |
| bug4_no_relay_leak | C++ `test_regression.cpp` | 1000 cycles, alloc==free==1000, bytes restored, registry zero |
| bug5_deferred_free | C++ `test_regression.cpp` | 3 callers, grace=20ms, alloc==free==1, post==11 |

**Findings during §7.6:**

1. **`test_suite.cpp` dispatch was initially incomplete.** Only `regression_bug1` was wired; `bug2/4/5` fell through to `return -1` without calling their functions, so `Snapshot()` returned the empty post-Reset sink. Caught by Kotlin assertion `lines.isNotEmpty()` — `regression_bug4 produced no result lines`. Fixed by adding all four dispatch cases. This is the assertion that can fail, doing its job.

2. **Bug 3 C++ stub removed.** The initial §7.6 pass included `RunRegressionBug3()` that unconditionally reported `[PASS]`. Same "assertion that cannot fail" pattern as `regAfter == regBefore` pre-tightening. Deleted. Bug 3 is verified only by the Kotlin reflection assertion in `RegressionTest.kt`, which can fail — and does, if `PmSpoof.install` doesn't set both fields or sets them to different proxy classes.

3. **`PmSpoof.kt` extracted from `RemoteService`.** Needed so `RegressionTest.bug3_both_pm_fields_patched` can call `PmSpoof.install(ctx)` from the test. `RemoteService.installPmSignatureSpoof()` now delegates. Extraction is idempotent (`installed` flag) so the `:remote` process's own `onStartCommand` chain still works.

4. **`RegressionTest.kt` was created in the wrong source set** (`main/java` instead of `androidTest/java`) and had to be moved. Files in `main/` go into the app APK, not the test APK — `numtests` stayed at 5 until the move.

**Verified bug details on device:**

- bug4: `alloc=1000 free=1000` — no relay page leak over 1000 cycles at grace=0.
- bug5: `alloc=1 free=1` — deferred-free completes, counters balanced.
- bug1: `before == install == restore` — ArtMethod page prot survives hook/unhook cycle unchanged.
- bug2: all 9 `ShouldHookDlopen` paths match expected bool.
- bug3: `instance.mPM` and `static.sPackageManager` hold the same `$Proxy` class after `PmSpoof.install`.

**Next:** §7.7 (selftest.txt file channel + IntegrationTest.kt).

---

