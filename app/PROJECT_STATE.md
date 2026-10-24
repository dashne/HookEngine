Last verified build: Session 16, verified on device 2026-10-25, `OK (49 tests)`. Retry-with-backoff added to `IntegrationTest.selftestReportIsComplete` for the `:remote` `startService` flake (mitigation; retry recovery path not observed to fire — green run only). Coverage 14/90.

Session 16 closed 2026-10-25: `:remote` `startService` flake investigated. Root cause is AMS state — process is marked bad before our first `startService` at IntegrationTest.kt:21 — not a code crash. Passing runs show `:remote` starting cleanly (`attachBaseContext` → `onCreate` → `onStartCommand` → cleanup kill; no FATAL, no signal, no ScopedCheck crash). Failure rate observed across Sessions 13–16: 3 pass / 2 fail. Mitigation: retry-with-backoff (3 attempts, 2s/4s) in `IntegrationTest.selftestReportIsComplete`. Retry recovery path unverified — only the green path executed post-mitigation (`OK (49 tests)`, no `startService attempt` log lines). Not a root-cause fix. Coverage 14/90, unchanged.

Session 15 closed 2026-10-25: HANDLEJHMETHOD_DECOMP_SPEC.md added at repo root. ProxyBridge.cpp header points at it. InlineHooker.cpp gained `g_iihe_quiet`; RunRegressionBug4 gates it around the 1000-cycle stress loop (eliminates ~15,000 `IIHE(bug4):` log lines). AntiDetectionE2ETest three connect tests `@Ignore`'d (Rule 12 honesty; were passing-by-skip on loopback EPERM). 49 pass, 3 ignored, 0 fail. Coverage 14/90.

Session 14 closed 2026-10-25: two real bugs fixed from Claude code review of Session 13's nativeCNVM: (1) nativeCNVM arity guard read `strlen(sig)` AFTER `ReleaseStringUTFChars` — UB on every call, latent UAF, fixed by capturing sigLen before release; (2) `g_rewrite_effective_count` incremented unconditionally after the swap block — counted rewriter throws and zero-length results as "effective", fixed by gating on a local `swapped` bool. `ProxyBridgeSurfaceTest` observed running for the first time in `am instrument -r` output (Session 13's "52 tests" was aspirational; actual pre-Session-14 measurement was 48). `IntegrationTest.selftestReportIsComplete` passed — `:remote` process bad failure from Session 13 NOT reproduced. `rewriteEffective=3` matches the three "swapped reply" log lines exactly. 52 tests, 0 failed. Coverage 14/90 verified (not claimed).

Session 13 closed 2026-10-25: nativeCNVM decomp-verified vs sub_30BF7C + sub_30C238; 4 tests green in ProxyBridgeSurfaceTest. handleJHMethod DEFERRED (no sub_29E168 decomp). Session 13 claimed 52 tests / 51 pass / 1 fail; the actual measurement was 48 tests running and the ProxyBridgeSurfaceTest class not in the -r output — Session 14 corrected this.

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
- Mission #1 Phase A: nativeCNVM decomp-verified + tested (Sessions 13–14). Session 14 fixed two real bugs in the port.
- Session 14: two-bug fix (nativeCNVM UAF, g_rewrite_effective_count gate) verified in full `am instrument` run.
- Session 15: HANDLEJHMETHOD_DECOMP_SPEC.md ships. handleJHMethod implementation now becomes a mechanical edit when sub_29E168 decomp lands.
- Session 16: `:remote` flake characterized as AMS state (not a code crash); retry-with-backoff mitigation added; pass line confirmed as `OK (N tests)` (my Session 15 "run finished: is authoritative" claim was wrong — both are valid; see Device-test-signal section).

Pending:
- Mission #1 Phase A: tnFIN code 36 byte-rewrite (Session 12+, deferred pending target need or decomp).
- Mission #1 Phase A: handleJHMethod — BLOCKED on sub_29E168 decomp. HANDLEJHMETHOD_DECOMP_SPEC.md specifies the 7 questions whose answers unblock implementation.
- Mission #1 Phase A: nativeInit full config.
- Mission #1 Phase A: nativeOnLoad full DEX extract.
- `IntegrationTest.selftestReportIsComplete` retry recovery: not yet observed firing (no flake caught post-mitigation). Observed pattern 3 pass / 2 fail (Sessions 13–16). If a future run logs `startService attempt N failed`, that is the verification signal.

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
- **Pass:** `OK (N tests)` (standard AndroidJUnitRunner success line)
- **Pass:** `run finished: N tests, X failed, Y ignored` with X == 0 (custom line from a wrapper listener; only appears on green runs)
- **Fail:** `FAILURES!!!` followed by `Tests run: N, Failures: M` (standard JUnit failure line; the `OK` and `run finished` lines do NOT appear)
- **Additional fail signal:** `INSTRUMENTATION_STATUS_CODE: -2` per-test

**Session 16 correction:** Session 15's claim that `run finished:` is the authoritative pass line and `OK (N tests)` is unreliable was wrong. Both indicate "all passed"; the failing run does not emit either. `run finished:` is a custom line from a wrapper listener and appears only on green runs in this build. `OK (N tests)` is the standard AndroidJUnitRunner line and also appears only on green runs. Both are trustworthy. `INSTRUMENTATION_CODE: -1` remains untrustworthy in isolation — it fires for both success and failure — so use `OK (N tests)` or `FAILURES!!!` for pass/fail, not `INSTRUMENTATION_CODE`.

Verified device workflow (only method that works on this device):

    adb install -r -t app/build/outputs/apk/debug/app-debug.apk
    adb install -r -t app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
    adb logcat -c
    adb shell am instrument -w -r com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner

adb install succeeds where UTP does not — the security exception is specific to the UTP install path, not to pm install.

CI (§7.9) stays build-only. README must document this under the test-suite section, not as a footnote.

Session 14 addendum: the Session 13 baseline `OK (48 tests)` was not a full measurement; Session 14's `run finished: 52 tests, 0 failed, 0 ignored` was the first run in which `ProxyBridgeSurfaceTest` appeared in the -r class list. Session 15's `run finished: 49 tests, 0 failed, 3 ignored` is 49+3 = 52 total registered. Session 16's `OK (49 tests)` confirms 49 running (3 `@Ignore`'d tests not counted in the JUnit `Tests run:` total).

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
| InstallInlineHookEx accepts `nullptr` name safely | YES — LOGD call sites guard `name ? name : "?"`; snprintf guarded by `if (name)` (Session 15 verified) |

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

## Test suite status — 52 registered (49 pass + 3 ignored), 0 fail

Classes (Session 16 measurement, `OK (49 tests)`):
- IntegrationTest              (1)   selftest file channel; retry-with-backoff (Session 16) — GREEN last run, retry path unverified
- NativeItemTest               (5)   items 1–5 — GREEN
- ProcessHooksTest             (4)   allProcessHooksPass,
  spoofProcessStaticsApi37State,
  processMyUidIsNative,
  doesFrameworkSeeNativeHook — GREEN
- RegressionTest               (5)   bug1–bug5 — GREEN
- RelocTest                    (1)   40 per-function reloc cases — GREEN
- ProxyBridgeSurfaceTest       (4)   nativeCNVM* — GREEN (first observed running in -r at Session 14)
- AntiDetectionTest            (1)   allAntiDetectionTestsPass — GREEN
- AntiDetectionE2ETest         (4)   javaFileHidingFiresHook GREEN; three connect tests @Ignore'd (loopback bind EPERM on API 37)
- PmSpoofTest                  (4)   — GREEN
- ClassLoaderBridgeTest        (3)   — GREEN
- ClassLoaderProbeTest         (1)   — diagnostic
- DualApplicationTest          (2)   — GREEN
- BinderTransactE2ETest        (9)   — GREEN
- BinderTransactHookTest       (2)   — GREEN
- BinderTransactProbeTest      (1)   — diagnostic
- BinderProbeTest              (1)   — diagnostic
- AidlCodeProbeTest            (1)   — diagnostic
- Session11Code36ReplyTest     (1)   — diagnostic
- Session11ProbeTest           (2)   — diagnostic

Total 49 running + 3 ignored = 52 registered. 0 failed.

Underlying assertions: 33+ (items) + 9 (process_hooks) + 3×bug4 + 3×bug5 +
1×bug1 + 1×bug2 + 40 reloc + 4 nativeCNVM + … ≈ 90+ PASS lines, 0 FAIL.

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

### Current coverage: 14 / 90 (~16%)

Verified in Session 14 via `am instrument -r`: `ProxyBridgeSurfaceTest` four tests ran green (first observed running). Not claimed; measured. Session 15 + 16 did not move the number.

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
| `tnFIN` | `BinderTransactHook` + `ParcelRewriter` + `CppParcelRewriter` | ✅ Production delivery for code 3 (getPackageInfo). Code 36 (getInstalledPackages) dispatched but byte-rewrite deferred — see Session 12 decision note. Code 36 only matters for callers passing signature flags; not observed as a target requirement. |
| `nativeDNPO` | TracerPid filter | ⚠️ Partial — misses full ptrace detect |
| `nativeAMRHook` | Path hide list | ⚠️ Partial — no mount spoof |
| **`nativeCNVM`** | `ProxyBridge.nativeCNVM` + `ProxyBridge.cpp::nativeCNVM` | ✅ Decomp-verified vs sub_30BF7C + sub_30C238 (Session 13). 4 tests in `ProxyBridgeSurfaceTest`. Two-bug fix in Session 14 (UAF in arity guard, `g_rewrite_effective_count` gate). Three safe-strengthening divergences vs reference documented in ProxyBridge.cpp header. |
| **`handleJHMethod`** | — | ⏸️ **DEFERRED.** Body in `ProxyBridge.cpp` is a stub. HANDLEJHMETHOD_DECOMP_SPEC.md at repo root specifies the 7 decomp questions whose answers determine whether the existing scaffolding helpers can be wired together as-is. Not counted toward coverage. |

**Not started (~76 methods):**
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
3. ⚠️ `nativeCNVM` ✅ decomp-verified + tested (Session 13). Two-bug fixed Session 14.
   `handleJHMethod` ⏸️ DEFERRED — HANDLEJHMETHOD_DECOMP_SPEC.md at repo root. Requires sub_29E168 decomp.
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
3. **Coverage number in every message.** 14/90 now. Must be higher every session, with the honest exception noted when decomp blocks the only path. Sessions 11, 12, 15, 16 did not grow it; all stated so openly.
4. **Every method shipped must have a test.**
5. **Do not declare a method "done" unless it matches the reference's behavior**, not just its signature.
6. **Session-by-session commit discipline.** One method (or one logical group) per commit, tests green at every commit.
7. **When wire format, symbol contract, or field layout is uncertain, request IDA decomp of the reference implementation before writing parse code.** Do not iterate on offset guesses. Session 9 burned three rebuild cycles on this. Session 10 confirmed it. Session 11 reinforced it. Session 15 codified it into HANDLEJHMETHOD_DECOMP_SPEC.md so handleJHMethod implementation cannot start without the answers.
8. **On this ROM, any JNI call taking a `jobject` on a caller thread with a JIT-compiled `BinderProxy.transact` frame above it crashes under CheckJNI.** Hook at the C++ layer (`IPCThreadState::transact`) or offload JNI to a fresh attached thread.
9. **dlsym(RTLD_DEFAULT, ...) filters `android::Parcel` primitives and `IPCThreadState::transact` on this ROM.** Use dynsym scan.
10. **The reply parcel's `mData` buffer is read-only post-transact.** Never write into it. Copy to a fresh `malloc`'d buffer, swap the pointer.
11. **Kotlin `object` instance fields are not static.** Use `@JvmStatic` accessors.
12. **Test assertions must be able to fail.** If a test can only pass, it is not a test. Session 15 fixed three tests in AntiDetectionE2ETest that were silently passing on loopback-bind EPERM — now `@Ignore`'d with a reason pointing at real substitution coverage in AntiDetectionTest. The test count is honest: 49 pass, 3 ignored.
13. **Handoffs deliver whole-file replacements, not line-by-line edits.** Contiguous-block replacements where whole-file is impractical (large C++ files, one-function edits) must be clearly delimited with locate-by pattern and replace-with pattern. Every handoff must include: (a) change summary per file, (b) paste content, (c) verify-before-commit checklist, (d) explicit `git add` + `git commit -m "..."` block.

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
  offset 0: [int32 strictModePolicy | STRICT_MODE_PENALTY_GATHER (0x80000000)]
  offset 4: [int32 workSource] (default -1)
  offset 8: [int32 PARCEL_HEADER_MAGIC = 0x53595354] ← "SYST"
  offset 12: [int32 n] (string16 length in chars)
  offset 16: [2n bytes UTF-16 interface token]
  offset ..: [int16 null terminator]
  offset ..: [pad to 4-byte boundary]
  offset ..: [AIDL args: arg0 = string16 pkg name, arg1 = int64 versionCode, arg2 = int32 flags]

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
hookFiresOnRealPackageManagerQuery: before=46 after=51 delta=5
rewriterDispatchesForOurPackage: before=6 after=7 delta=1
RewriteOnFreshThread: swapped reply (1848 -> 1848 bytes) pos=0
ParcelRewriter: code 3 signatures nulled for com.ims134.hookengine

### Code 36 known issue (Session 11 target)

`getInstalledPackages` reply format from hex dump:
code 36 in: size=66024 first32=00000000 01000000 16010000 1e000000 61006e00 64007200 6f006900 64002e00

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

### Mission #1 status: 13 / 90 (~14%) — UNCHANGED

Session 11 did not close the code-36 gap. What it delivered is **the diagnostic foundation** to close it correctly in a later session: exception safety for the Kotlin rewriter, a real implementation of `drainCodesNative`, an effective-vs-dispatched rewriter counter, the raw 66 KB code-36 reply captured and hex-dumped, and the definitive observation that this ROM's `PackageInfo.<init>` uses `readString8` for `packageName` (not AOSP's `readString16`).

Coverage stayed at 13/90. Rule 3 says every session must grow the number. Session 11 did not — because the code-36 byte rewrite is not delivered and the code-3 production delivery was already counted in Session 10. This is an honest non-growth session.

### What was delivered

- `ParcelRewriter.rewriteReply` now preserves `reply.dataSize()` on rewriter exception. Previously, a throw mid-parse caused `setDataSize(dataPosition())` to truncate the caller's reply to 8 bytes. This is a real corruption bug on any exception path.
- `BinderTransactHook.cpp`: `drainCodesNative` no longer a stub. Before Session 11, `drainCodes()` always returned `[]`. After: returns the real codes. This made `AidlCodeProbeTest` live again.
- `BinderTransactHook.cpp`: `RewriteOnFreshThread` bails before swap on rewriter exception. A partial rewrite is never swapped into the caller's reply.
- `BinderTransactHook.cpp`: new `g_rewrite_effective_count`. Distinguishes "rewriter dispatched" from "rewriter delivered a swap".
- `BinderTransactHook.cpp`: capture of the full raw code-36 reply (66 KB) into `g_last_code36_reply` on every code-36 dispatch. Exposed as `dumpLastCode36Reply` JNI method.
- `NativeTestBridge.kt`, `BinderTransactHook.kt`: added Kotlin externals for `dumpLastCode36Reply` and `rewriteEffectiveCount`.
- `Session11ProbeTest.kt` (new): asserts `getInstalledPackages(0)` fires at least one code, and that `rewriteCount > 0`.
- `Session11Code36ReplyTest.kt` (new): captures the code-36 reply, asserts it exists and is non-empty, hex-dumps the first 128 bytes to logcat under tag `Session11`, and reads the front of the reply as `[ex][hasResult]`.

### What was NOT delivered

- **Code 36 byte rewrite.**
- **The exact layout of the 3 int32s between the class name and the first element.**

### Test results

`am instrument -w -r` → `OK (47 tests)`. Zero failures. Zero SIGSEGV.

### Why coverage did not increase

Rule 3 says every session must increase the coverage number. Session 11 did not — because code 3 was already counted (Session 10) and code 36 was not delivered as behavior. Coverage 13 → 13 is the honest reflection of "code-3 live, code-36 pending." No inflation.

---

## Session 12 — Q1 measured, code 36 deprioritized — 2026-10-24

### Mission #1 status: 13 / 90 (~14%)

### Q1 result (measured on device, Session 12)

    getInstalledPackages(0): 278 pkgs, 0 with non-null sigs
    getInstalledPackages(GET_SIGNATURES): 278 pkgs, 278 with non-null sigs

`PackageInfo.signatures` / `signingInfo` are null unless the caller
explicitly passes `GET_SIGNATURES` / `GET_SIGNING_CERTIFICATES`.

### Decision

**Deprioritize code 36. Defer Phase A item 2 (code-36 byte-rewrite) and
move to Phase A item 3 (`handleJHMethod` + `nativeCNVM`).**

### What does NOT change

- The code-36 skip-guard in `BinderTransactHook.cpp` stays in place.
- The captured 66 KB code-36 reply (`g_last_code36_reply`) remains
  available via `dumpLastCode36Reply` JNI.
- `Session11Code36ReplyTest` remains green and diagnostic.
- The 3-int32 hypothesis at offsets 80/84/88 remains unverified.

### Deliverables this session

- `BinderTransactE2ETest.rewriterExceptionPreservesReplySize` — the
  Session 11 exception-path fix now has a direct test.
- `AidlCodeProbeTest` narrowed from 15 probe calls to 4.
- This decision note.

### Test count: 48 green. Coverage: 13/90 (unchanged).

---

## Session 13 — nativeCNVM decomp-verified — 2026-10-25

### Mission #1 status: 13 / 90 (~14%) at time of close; 14/90 after Session 14 verification

`nativeCNVM` decomp-verified against `sub_30BF7C` + `sub_30C238`.
`ProxyBridgeSurfaceTest.kt` added with four tests. `ProxyBridge.cpp` with
`nativeCNVM` and a stub `handleJHMethod` added. `ProxyBridge.kt` with
`nativeCNVM` external only.

### What Session 13 actually delivered

- **`nativeCNVM`** decomp-verified vs `sub_30BF7C` (entry) + `sub_30C238` (cache init). 8 jmethodIDs cached in identical order. `Executable.artMethod` fieldID cached on API ≥ 30, guarded, not read by `nativeCNVM`. Tag → slot mapping identical for all 9 tags. `DeleteLocalRef(elem)` after every unwrap. `FromReflectedMethod(method)` → `GetObjectClass(obj)` → `CallNonvirtualVoidMethodA(obj, cls, mid, args)`. Three safe-strengthening divergences vs reference documented in `ProxyBridge.cpp` header.
- **`ProxyBridgeSurfaceTest.kt`** with four tests (nativeCNVM*).
- **`handleJHMethod`** — deferred. Body is a stub returning `nullptr`.
- **`IntegrationTest.selftestReportIsComplete`** failed with `:remote process bad`.

### Session 13's claims vs reality

Session 13 claimed "52 tests, 51 pass, 1 fail". Session 14 measured the actual test count was 48 running (ProxyBridgeSurfaceTest was not in the -r output). The 52/51/1 claim was aspirational; Session 14 corrected it. Session 15 confirmed 49 pass + 3 ignored = 52 registered.

### Next session

Phase A item 3 continued: `handleJHMethod` needs `sub_29E168` decomp. Session 14 identified two real bugs in `nativeCNVM` from Claude code review — see Session 14 section.

---

## Session 14 — nativeCNVM two-bug fix + test measurement — 2026-10-25

### Mission #1 status: 14 / 90 (~16%) — verified, not claimed

Session 14's deliverable was two bug fixes to Session 13's `nativeCNVM` port (from Claude code review) and the first true measurement of the test suite including `ProxyBridgeSurfaceTest`.

### Bugs fixed

| # | Bug | Root cause | Fix |
|---|---|---|---|
| 27 | `nativeCNVM` use-after-release of `sig` | Arity guard called `strlen(sig)` AFTER `ReleaseStringUTFChars`, with `FromReflectedMethod` and `GetObjectClass` (both GC safepoints) in between. UB on every call; latent UAF | Capture `sigLen = strlen(sig)` BEFORE release; use `sigLen` in guard |
| 28 | `g_rewrite_effective_count` incremented unconditionally | After the swap block closed, so rewriter throws and zero-length results were counted as "effective". The "swapped reply" log had the same bug | Introduced local `bool swapped` set only inside the swap branch; gated both `fetch_add` and `LOGD` on `swapped`. Added explicit "no swap (...)" diagnostic log |

### Measurement (what Session 13 claimed but did not verify)

- `run finished: 52 tests, 0 failed, 0 ignored` — first run in which `ProxyBridgeSurfaceTest` class appears in `-r` output.
- `nativeCNVMMatchesReferenceSignature`, `nativeCNVMNullArgsIsNoOp`, `nativeCNVMInvokesVoidMethodWithCorrectArgs`, `nativeCNVMCoercesBoxedIntegersToJvalueInt` — all four `finished:`, no `[FAIL]`.
- `ProxyBridge.nativeCNVM: arity mismatch n=0 paramCount=2 — refusing dispatch` — arity guard fires correctly with the fixed length source. `paramCount=2` for `"VIL"` with 2 params.
- `IntegrationTest.selftestReportIsComplete` PASSED. `:remote` process started cleanly, all hooks installed, `Service started. (v29)`, full item1-5 suite ran, `selftest.txt written: 23 pass`. Session 13's `process is bad` failure NOT reproduced.
- `rewriteEffective=3` matches the three "swapped reply" log lines exactly (two from `AidlCodeProbeTest` code-3 GET_SIGNATURES and GET_SIGNING_CERTIFICATES, one from `BinderTransactHookTest.hookFiresAndPassesThrough` code-3). The Session 13 "16 dispatch / 3 effective" reading was coincidental (counter over-counted but happened to match). The counter is now semantically correct.

### AntiDetectionE2ETest observation (fixed in Session 15)

Three connect tests (`connectToFridaPortBlockedEvenWithListener`, `connectToIdaPortBlockedEvenWithListener`, `connectToNonBlockedPortSucceeds`) returned early on `SocketException: EPERM` from `bind(127.0.0.1)` and were reported as PASSED by JUnit. Rule 12 violation — tests that cannot fail. Fixed in Session 15.

### Files touched

**Modified:**
- `app/src/main/cpp/ProxyBridge.cpp` — UAF fix; arity guard now reads `sigLen` after release. `handleJHMethod` unchanged (stub).
- `app/src/main/cpp/BinderTransactHook.cpp` — `g_rewrite_effective_count` gate on `swapped` local; added "no swap (...)" log.

### Coverage: 14/90 verified

`nativeCNVM` is now a measured 14, not a claimed 14. The four `ProxyBridgeSurfaceTest` tests ran green in `am instrument -r`.

### Next session

Session 15: no decomp available for `sub_29E168`. Deliver prep work: decomp spec + loose-end cleanup (AntiDetectionE2ETest honesty fix, bug4 log noise). Coverage stays 14/90 (honest no-growth session).

---

## Session 15 — handleJHMethod decomp spec + loose-end cleanup — 2026-10-25

### Mission #1 status: 14 / 90 (~16%) — unchanged, honest no-growth session

Session 15's deliverable was preparation: HANDLEJHMETHOD_DECOMP_SPEC.md at repo root, plus two loose-end cleanups (AntiDetectionE2ETest Rule-12 honesty, bug4 log noise suppression). No new methods shipped, so coverage did not grow. That is honestly stated per Rule 3.

### Deliverables

**15.1 — `HANDLEJHMETHOD_DECOMP_SPEC.md` (new file at repo root)**

Enumerates 7 decomp questions with decision trees that determine whether the existing scaffolding helpers (`ReflectIds`, `BoxResult`, `IsStaticMethod`, `BuildDescriptor`, `UnwrapArgs`) can be wired into `handleJHMethod` as-is or need modification:

- Q1 Dispatch path (`CallNonvirtualObjectMethodA` vs `CallObjectMethodA` vs static variant)
- Q2 `obj2` semantic (unused, declaring class, fallback receiver, or config key)
- Q3 Return-value boxing (matches current `BoxResult`? FindClass per call? raw primitive cast?)
- Q4 Exception semantics (check-and-clear, fallback, do-nothing)
- Q5 `Cache(env)` invocation (entry vs conditional vs never)
- Q6 Descriptor derivation (`BuildDescriptor` reflective iteration vs `ArtMethod::GetSignature()` vs caller-passed)
- Q7 `methodArr` arity / element use (single target method vs super-chain vs declaring-class-plus-params)

Plus §3 exit criteria: the 10 evidence points the decomp artifact must contain before Session 16 can land `handleJHMethod`. If absent from the artifact, Session 16 escalates for a re-dump.

`ProxyBridge.cpp` header comment updated to point at `HANDLEJHMETHOD_DECOMP_SPEC.md`.

**15.2 — `AntiDetectionE2ETest.kt` Rule-12 honesty fix**

Three connect tests were passing-by-skip on loopback-bind EPERM (Android 15+ unprivileged loopback bind restriction on API 37). Marked `@Ignore` with a reason string that points at real substitution coverage: `AntiDetectionTest.allAntiDetectionTestsPass` asserts the exact `Fake_connect` decision for ports 27042/27043/23946 and the allow cases 8080 and 8.8.8.8:27042, plus `connect_hook_installed == 1`. `javaFileHidingFiresHook` (no socket requirement) remains live.

Test count moved from 52 pass + 0 ignored to 49 pass + 3 ignored. Honest, visible in `-r` output.

**15.3 — bug4 log noise suppression**

`InlineHooker.cpp` gained `extern "C" int g_iihe_quiet = 0;` and `#define IIHE_LOG(...)` guarding the verbose `IIHE(...)` LOGD calls inside `InstallInlineHookEx`. `LOGE` calls unchanged. `test_regression.cpp::RunRegressionBug4` sets `g_iihe_quiet = 1` around its 1000-cycle stress loop, `= 0` after. `bug5` (single cycle) is untouched.

Eliminates ~15,000 lines of `IIHE(bug4):` noise per test run. All three bug4 [PASS] assertions preserved:
- `bug4_no_relay_leak: cycles=1000 bad=0 alloc=1000 free=1000`
- `bug4_target_bytes_restored: before=0x11000400 after=0x11000400`
- `bug4_inline_count_zero: registry=0`

### Measurement

`run finished: 49 tests, 0 failed, 3 ignored`

- `ignored: connectToFridaPortBlockedEvenWithListener(com.ims134.hookengine.AntiDetectionE2ETest)`
- `ignored: connectToNonBlockedPortSucceeds(com.ims134.hookengine.AntiDetectionE2ETest)`
- `ignored: connectToIdaPortBlockedEvenWithListener(com.ims134.hookengine.AntiDetectionE2ETest)`

Live tests still green: `finished: javaFileHidingFiresHook(...)`, all four `ProxyBridgeSurfaceTest`, all `AntiDetectionTest` decision-function assertions.

`IIHE(bug4):` count in run log: **0** (was ~15,000 in Session 14).

`IntegrationTest.selftestReportIsComplete` PASSED again — 2/2 since Session 13's `:remote` failure. (Session 16 later disproved the "one more green closes it" expectation — the flake recurred.)

### Files touched

**Added:**
- `HANDLEJHMETHOD_DECOMP_SPEC.md` (repo root)

**Modified:**
- `app/src/main/cpp/ProxyBridge.cpp` — header comment expanded, now points at `HANDLEJHMETHOD_DECOMP_SPEC.md`. Body unchanged (handleJHMethod still stub).
- `app/src/main/cpp/InlineHooker.cpp` — `g_iihe_quiet` flag; 15 `LOGD("IIHE(` → `IIHE_LOG("IIHE(` sites inside `InstallInlineHookEx`. All `LOGE` sites unchanged.
- `app/src/main/cpp/test_regression.cpp` — `extern "C" int g_iihe_quiet;` declaration; `RunRegressionBug4` sets `g_iihe_quiet = 1` before the 1000-cycle loop, `= 0` after.
- `app/src/androidTest/java/com/ims134/hookengine/AntiDetectionE2ETest.kt` — three `@Ignore` annotations; class-level comment explaining the ROM restriction and pointing at substitution coverage.

### Coverage: 14/90 (unchanged)

Session 15 is a "no growth" session because no new method shipped. Rule 3 is not satisfied; that is stated as an honest exception, not a norm. The next growth path is blocked on `sub_29E168` decomp. HANDLEJHMETHOD_DECOMP_SPEC.md removes that block from the critical path — when decomp lands, implementation is mechanical.

---

## Session 16 — `:remote` flake investigated, retry mitigated, retry path unverified — 2026-10-25

### Mission #1 status: 14 / 90 (~16%) — unchanged, stability investigation session

Session 16 was originally scoped as a stability confirmation: three consecutive green runs of `IntegrationTest.selftestReportIsComplete` (2/3 so far) would close the `:remote` item as flake. The third run **failed**. That converted the session into an investigation.

### Failure evidence

Run 16 first attempt (before mitigation), from `test_out.txt`:

java.lang.SecurityException: Unable to start service Intent { xflg=0x4
cmp=com.ims134.hookengine/.RemoteService }: Unable to launch app
com.ims134.hookengine/10428 for service Intent { cmp=com.ims134.hookengine/.RemoteService
}: process is bad
at android.app.ContextImpl.startServiceCommon(ContextImpl.java:2134)
at android.app.ContextImpl.startService(ContextImpl.java:2091)
at com.ims134.hookengine.IntegrationTest.selftestReportIsComplete(IntegrationTest.kt:21)

FAILURES!!!
Tests run: 49, Failures: 1


Failure occurs at **position 26 of 52** — the first touch of `:remote` in the whole suite. Nothing else starts `:remote` beforehand.

### Root cause: AMS state, not a code crash

Logcat from a **passing** run (`remote_crash_filtered.txt`) shows `:remote` starting cleanly:
02:30:53.787 ActivityManager: Start proc 7143:com.ims134.hookengine:remote/... for started-service
02:30:54.018 DualApplication.attachBaseContext: entering
02:30:54.090 PmSpoof: state list=true static=true instance=true
02:30:54.091 DualApplication: hooks installed, calling super.attachBaseContext
02:30:54.092 DualApplication.onCreate
02:30:57.402 PmSpoof: state list=true static=true instance=true
02:31:03.396 ActivityManager: Killing 7143:... (adj 500): stop com.ims134.hookengine due to finished inst

No `FATAL EXCEPTION`, no `signal 11`, no `SIGSEGV`, no `SIGABRT`, no `ScopedCheck` crash. `:remote` starts, runs through `attachBaseContext`, `onCreate`, `RemoteService.onStartCommand`, and is killed by AMS for cleanup.

"`process is bad`" is AMS `startProcessLocked` refusing to launch a process record already marked crashed. It is not a fresh crash. That means the state is set either (a) from a prior app-process lifetime on this ROM, or (b) by a Zygote/AMS fork failure that never reaches our code. Both fail before `IntegrationTest` runs.

**Conclusion:** the failure is not a bug in `DualApplication`, `PmSpoof`, `nativeAttachBaseContext`, or any of our hook code. Every passing run's logcat proves our `:remote` path is healthy when it runs. The AMS-side state is what flips. This is not fixable from user code — only mitigated.

### Observed flake rate

| Session | `selftestReportIsComplete` |
|---|---|
| 13 | fail (`process is bad`) |
| 14 | pass |
| 15 | pass |
| 16 run A | fail (`process is bad`) |
| 16 run B (post-retry patch, green path) | pass |

**3 pass / 2 fail = 60% pass rate.**

### Deliverable: retry-with-backoff in `IntegrationTest.kt`

Full-file replacement was applied. `IntegrationTest.selftestReportIsComplete` now loops `startService` up to 3 times with 2s then 4s backoff on `SecurityException`. If all 3 attempts fail, throws `AssertionError` with the last exception as cause (Rule 12 honest — a real failure still fails). If a flake is caught and the retry succeeds, the first attempt logs `Log.w` and the test proceeds normally.

The retry loop preserves the original assertions on `selftest.txt` (23 PASS entries, END marker).

### Verification

Green run 16e, `OK (49 tests)`, `Select-String 'startService attempt'` returns nothing — retry was not exercised (first attempt succeeded). **Retry recovery path unverified**; the mitigation has not been observed to rescue a flake yet.

Pass line confirmed as `OK (49 tests)` — standard JUnit line, not `run finished:`. Session 15's claim that `run finished:` is the authoritative pass line and `OK (N tests)` is untrustworthy was wrong; both indicate all-pass, and the failing run emits neither (`FAILURES!!!` + `Tests run: N, Failures: M` instead).

### Files touched

**Modified:**
- `app/src/androidTest/java/com/ims134/hookengine/IntegrationTest.kt` — retry-with-backoff on `startService`.

### Coverage: 14/90 (unchanged)

No new methods. Rule 3 not satisfied. Honest exception, not a norm.

### Next session (Session 17)

- **If `sub_29E168` decomp is available:** implement `handleJHMethod` using HANDLEJHMETHOD_DECOMP_SPEC.md's decision tree. Coverage 14 → 15. Real Phase A progress.
- **If not:** Session 17 is either a stability measurement (a few more runs to see if retry ever fires) or an escalation that Phase A is decomp-blocked. No other Phase A path is available — `nativeInit` also needs decomp.

---

## Hard-won facts (canonical reference)

- **Under `am instrument`, ALL `jobject`-touching JNI calls crash on a caller thread with a JIT `BinderProxy.transact` frame above it.** Not just `NewGlobalRef`. `GetLongField` dies the same way. (Rule 8)

- **`android::IPCThreadState::transact` is the correct hook for AIDL interception on this ROM.** Symbol `_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j`, in `libbinder.so` dynsym.

- **dlsym filters android::Parcel primitives on this ROM.** Use dynsym scan for any `libbinder.so` symbol. (Rule 9)

- **`android::Parcel` field offsets (IDA-verified, API 37, arm64):**
    - `+0` mError (int32)
    - `+8` mData (void*)
    - `+16` mDataSize (size_t)
    - `+24` mDataCapacity (size_t)
    - `+32` mDataPos (size_t)
    - `+64` mNextObjectHint (size_t)
    - `+81` mParseHeader (byte), `+82` mParseHeaderPadding (byte)
    - `+88` mType (int32)

- **Reply parcel's `mData` buffer is read-only post-transact.** Never write into `mData` directly; always swap the pointer to a fresh `malloc`'d copy. (Rule 10)

- **The fresh-thread rewriter pattern:** `Parcel.obtain()` → `SetLongField(mNativePtr, targetPtr)` → run rewriter → `SetLongField(mNativePtr, originalPtr)` → `recycle()`.

- **Kotlin `object` instance fields are not static.** Use `@JvmStatic` accessors. (Rule 11)

- **API 37 writeInterfaceToken wire format** (Session 9, IDA-verified):
  offset 0: [int32 strictModePolicy | STRICT_MODE_PENALTY_GATHER (0x80000000)]
  offset 4: [int32 workSource] (default -1)
  offset 8: [int32 PARCEL_HEADER_MAGIC = 0x53595354] ← "SYST"
  offset 12: [int32 n] (string16 length in chars)
  offset 16: [2n bytes UTF-16 interface token]
  offset ..: [int16 null terminator]
  offset ..: [pad to 4-byte boundary]
  offset ..: [AIDL args]

- **Code-36 reply on this ROM — observed layout (Session 11, 128 bytes):**
  [int32 ex=0][int32 hasResult=1][int32 N=278][int32 class_name_len=30][60 bytes UTF-16 "android.content.pm.PackageInfo"][int16 null][pad to 4][int32 unknown=0 at +80][int32 unknown=1 at +84][int32 unknown=-1 at +88][int32 packageName_len=41][41 bytes ASCII …]
  `packageName` uses `writeString8` on this ROM (MIUI change). The three int32s at 80/84/88 remain unverified without decomp.

- **`g_rewrite_effective_count` vs `g_rewrite_count`:** "dispatched" vs "delivered a swap." Session 14 fix: counter now gates on local `swapped` bool — previously incremented unconditionally, counting rewriter throws and zero-length results as effective. Session 14 verified: `rewriteEffective=3` matches three "swapped reply" log lines.

- **`getPackageInfo(pkg, 0)` does not reliably fire on this ROM** due to framework caching. Only `getPackageInfo(pkg, GET_SIGNATURES)` reliably issues IPC. Use `getInstalledPackages(...)` for code-36 dispatch tests.

- **`nativeCNVM` arity guard reads `sigLen` captured BEFORE `ReleaseStringUTFChars`.** Session 14 fix. Post-release `strlen(sig)` is UB — the buffer is unpinned, and `FromReflectedMethod`/`GetObjectClass` in between are GC safepoints. Do not move the `strlen` back after the release.

- **`handleJHMethod`'s scaffolding helpers exist but are unreachable from the stub body.** Session 15 made this deliberate via HANDLEJHMETHOD_DECOMP_SPEC.md. Do not modify any helper's behavior or add tests pinning their current behavior until the decomp spec's exit criteria are met. The helpers are compiled but not called — this is not a defect, it is the plan.

- **`g_iihe_quiet` in `InlineHooker.cpp`** gates the 15 verbose `IIHE(...)` LOGD lines inside `InstallInlineHookEx`. Errors (LOGE) always print. `test_regression.cpp::RunRegressionBug4` sets it around its 1000-cycle stress loop. Session 15 verified: 0 `IIHE(bug4):` lines in the run log (was ~15,000).

- **Loopback bind on API 37 / HyperOS fails with `SocketException: EPERM`** in the instrumentation process. Android 15+ restricts unprivileged `bind()` on `127.0.0.1`. This makes any test requiring a real loopback listener impossible on this device in the instrumentation process. `AntiDetectionE2ETest` three connect tests are `@Ignore`'d for this reason (Session 15). The decision-function coverage in `AntiDetectionTest` still asserts the exact `Fake_connect` behavior.

- **Pass/fail parsing from `am instrument -w -r` (Session 16 correction).** Both of the following are trustworthy pass signals:
    - `OK (N tests)` — standard AndroidJUnitRunner line
    - `run finished: N tests, 0 failed, Y ignored` — custom line from the wrapper listener, only appears on green runs
      Fail is `FAILURES!!!` followed by `Tests run: N, Failures: M`. Neither `OK` nor `run finished` appears on fail. `INSTRUMENTATION_CODE: -1` remains untrustworthy in isolation (fires for both pass and fail). Session 15's claim that `run finished:` is authoritative and `OK (N tests)` unreliable was wrong.

- **`:remote` `startService` flake is AMS state, not a code crash (Session 16).** `SecurityException: ... : process is bad` at `IntegrationTest.kt:21` fires when AMS has already marked the `:remote` process record bad — the crash is upstream, not from the `startService` call. Passing runs' logcat confirms `:remote` starts cleanly (attachBaseContext → onCreate → onStartCommand → cleanup kill; no FATAL, no signal). Observed 3 pass / 2 fail across Sessions 13–16. Retry-with-backoff in `IntegrationTest.selftestReportIsComplete` (3 attempts, 2s/4s) mitigates the symptom; recovery path unverified.

---

## Test count history

- Pre-Session 8: 16 classes, ~90 assertions
- Session 8: 32 tests
- Session 9: 39 tests
- Session 10: 43 tests
- Session 11: 47 tests
- Session 12: 48 tests
- Session 13: 52 tests (claim — unverified; actual -r showed 48 running, ProxyBridgeSurfaceTest not in output)
- Session 14: **52 tests** (verified — first run in which ProxyBridgeSurfaceTest appears in -r; 52 pass, 0 fail)
- Session 15: **52 registered = 49 pass + 3 ignored** (AntiDetectionE2ETest three connect tests @Ignore'd on loopback EPERM)
- Session 16: **52 registered = 49 pass + 3 ignored** (`OK (49 tests)` — 3 `@Ignore`'d tests not counted in JUnit total; retry mitigation added to IntegrationTest, recovery path unverified)

Coverage history:
- Session 7: 10 / 90 (~11%)
- Session 8: 11 / 90 (~12%)
- Session 9: 12 / 90 (~13%)
- Session 10: 13 / 90 (~14%)
- Session 11: 13 / 90 (~14%) — unchanged, honestly reported
- Session 12: 13 / 90 — unchanged, honestly reported
- Session 13: 14 / 90 — claimed; Session 14 verified
- Session 14: **14 / 90 (~16%)** — verified via `am instrument -r`
- Session 15: 14 / 90 — unchanged, honest no-growth (prep + cleanup, no new methods)
- Session 16: 14 / 90 — unchanged, `:remote` stability investigation + retry mitigation; no new methods

---

## What Session 17 must do FIRST

1. **Determine whether `sub_29E168` decomp is available.**
    - If yes: implement `handleJHMethod` using HANDLEJHMETHOD_DECOMP_SPEC.md's decision tree. Per Q1–Q7. Coverage 14 → 15. Do not skip any question. If a decision-tree branch requires escalation (e.g. Q1 shows direct `ArtMethod::Invoke`), stop and report.
    - If no: Session 17 is either (a) a stability measurement: 2–3 additional full-suite runs to observe whether the retry mitigation ever fires on a flake (log line `startService attempt N failed`), or (b) escalation: `sub_29E168` and `nativeInit` decomp are now the only Phase A blockers; without either, Phase A cannot progress.

2. **If `handleJHMethod` lands:** add Kotlin external in `ProxyBridge.kt`, add `handleJHMethod*` tests, coverage moves 14 → 15 in the same session. Test count 52 → 52+N.

3. **If `IntegrationTest.selftestReportIsComplete` retry fires** during a future run and the test ends green, record that as retry-recovery verification. Do not update the `:remote` known-issue note from "mitigated, unverified" to "resolved" until (a) retry recovery is observed, and (b) the underlying flake rate drops or is explained. The retry alone does not close the item; it merely makes the test robust.

4. **Do not** invent other work. `nativeInit` needs decomp. `tnFIN` code 36 needs a target or decomp. Every other Phase A path is decomp-gated. If neither `sub_29E168` nor `nativeInit` decomp is available by Session 17, the honest report is "Phase A blocked on decomp; Session 17 is either stability measurement or diagnostic."

---

## Rules of engagement (restated, for the record)

1. Every session opens with the mission header.
2. DualHost integration is out of scope until Phase A complete.
3. Coverage number in every message. 14/90 now. Must be higher every session, with the exception stated openly when decomp blocks all growth. Sessions 11, 12, 15, 16 all failed this rule honestly; that is the enforcement, not a habit.
4. Every method shipped must have a test.
5. Do not declare a method "done" unless it matches the reference's behavior.
6. Session-by-session commit discipline. Green or the session is not done. Commit after every green milestone.
7. IDA-first when wire format, symbol contract, or field layout is uncertain. Session 11: guessed synthetic format → OOM; guessed real offset → SIGABRT. Session 15: formalized this into HANDLEJHMETHOD_DECOMP_SPEC.md so `handleJHMethod` implementation cannot start on guesswork.
8. Any `jobject`-touching JNI call on a caller thread with a JIT `BinderProxy.transact` frame above it crashes on this ROM. Hook at C++ layer or offload to fresh attached thread.
9. dlsym filters `android::Parcel` primitives and `IPCThreadState::transact` on this ROM. Use dynsym scan.
10. Reply parcel's `mData` is read-only post-transact. Never write into it. Swap the pointer.
11. Kotlin `object` instance fields are not static. Use `@JvmStatic` accessors.
12. Test assertions must be able to fail. Session 15's AntiDetectionE2ETest fix: three tests were silently passing on `bind()` EPERM — marked `@Ignore` with reason pointing at real substitution coverage. Session 16's retry in `IntegrationTest` preserves fail-on-three-attempts (throws `AssertionError` with the original `SecurityException` as cause). Retry only masks transient AMS state; it does not hide real failures.
13. Handoffs deliver whole-file replacements or well-delimited contiguous-block replacements with locate-by/replace-with patterns. Every handoff must include: (a) change summary per file, (b) paste content, (c) verify-before-commit checklist, (d) explicit `git add` + `git commit -m "..."` block.

Header updated: Session 16 closed, `OK (49 tests)`, coverage 14/90.

Mission #1 coverage table: nativeCNVM noted as Session 14 two-bug fixed; handleJHMethod noted as blocked on HANDLEJHMETHOD_DECOMP_SPEC.md exit criteria.

"Order of attack" item 3 status unchanged from Session 15 — Session 16 was stability, not handleJHMethod.

Rule 3 evidence updated with Session 16 exception (stability investigation, not coverage growth).

Rule 12 evidence extended with Session 16 retry design (fail-on-three still fails).

"Current test count" 49 pass + 3 ignored = 52 registered, `OK (49 tests)`.

New full section: "Session 16 — :remote flake investigated, retry mitigated, retry path unverified."

"Hard-won facts" extended with: `OK (N tests)` as valid pass signal (Session 15 claim corrected), `:remote` AMS state not code crash.

"Test count history" and "Coverage history" appended with Session 16.

"What Session 17 must do FIRST" replaces old "What Session 16 must do FIRST."