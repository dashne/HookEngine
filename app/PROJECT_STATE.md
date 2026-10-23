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


---

## §7.7 — CLOSED 2026-10-23

File-based result channel + integration test. Device verification:
`am instrument -w -r` → `numtests=11`, all suites green, final
`OK (11 tests)`.

- `writeSelftestFile(path)` JNI export in `native-lib.cpp` — reads
  `testreport::Snapshot()`, writes one `PASS|name|detail` or
  `FAIL|name|detail` line per entry, then `END <pass> <fail>`,
  then `Reset()`. Truncates at start (`fopen(path, "w")`).
- `RemoteService.onStartCommand` calls `writeSelftestFile(filesDir/selftest.txt)`
  after the End marker and before spawning the diagnostic Thread.
- `IntegrationTest.selftestReportIsComplete` deletes any stale file,
  calls `startService(RemoteService)`, polls up to 60s for an END
  marker, then asserts: file exists, END present, no `FAIL|` lines,
  exactly 23 `PASS|` lines.

**Why this matters:** per the Device test signal finding, this is
the only test path where a failed hook run is observable as a
non-zero Gradle outcome. The 10 tests from §7.5/§7.6 rely on
per-run `am instrument` output; this one goes through
`RemoteService`, which is the path the app actually uses. If the
app breaks and `writeSelftestFile` isn't reached, `IntegrationTest`
fails — that is the "assertion that can actually fail" applied to
the integration surface, not just the individual hook primitives.

**Observation:** `startService` on `RemoteService` from an
instrumentation context succeeded on the target device (API 37),
despite the app's `targetSdk 34` and Android 8+ background-service
restrictions. Likely because the process is foregrounded during
instrumentation. Not verified across other device states — if this
test ever fails with `IllegalStateException: Not allowed to start
service Intent`, that's the cause and the test must bind rather
than start.

**Next:** §7.9 (build-only CI, README limitations section).

---


---

## §7.9 — CLOSED 2026-10-23

Build-only CI workflow committed at `.github/workflows/test.yml`.
Runs `:app:assembleDebug :app:assembleDebugAndroidTest` on push/PR.
Does not run device tests. README documents this under **Testing → CI**
along with the two broken automation paths.

**Item 7 is CLOSED.** Final device state: `am instrument -w -r` →
`numtests=11`, `OK (11 tests)`. 33 underlying assertions (23 item + 8
regression + 1 integration + 1 Kotlin bug3), 0 failures.

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

**Next:** Phase 2 direction TBD (per-function reloc tests / new API level /
DualHost integration).

---


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

**Remaining Phase 2 work if pursued:** `arm64_reloc.cpp` internals
`enc_adrp`, `enc_adr`, `enc_adrp_add` are file-static and tested only
via their public callers (`EmitRelocated`). Making them public and
testing individually would catch encoding bugs that don't affect
`EmitRelocated`'s public contract. Not required — the public contract
is what callers depend on.

**Next:** Option B (second API level) or C (DualHost integration).

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

## Test suite status — 16/16 green

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

PmSpoof filter delivered; hidden API bypass via thread-attached reflection

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



