Last verified build: Session 28, verified on device 2026-10-26. OK (1 test) on ActivityBootProbeTest. Wall A closed (Session 22), Wall B closed (Session 23). Wall C1 closed (Session 26, Looper fix — probe body posted to `Looper.getMainLooper()`). Wall C2 closed (Session 28, IContentProvider AttributionSource rewrite verified — ~40 `CP rewrite fired` lines, all `leading=92(preserved)`, `size=...→+12 delta=12`). Wall C3 named (Session 28, MIUI `ProcessManager.addMiuiApplicationThread` rejected from `Thread-26`, kernel-uid check on 10428, non-blocking background crash, rewritability TBD). Milestone 3 accepted (Session 24, "Viber boot" chapter closed). Milestone 4 in progress: `SplashActivity` instantiated, attached (`mBase=ContextThemeWrapper`, `mApplication=ManifestViberApplication`, `mWindow=PhoneWindow`), `onCreate`+`onStart`+`onResume` all succeeded. Wall C4 (behind `onResume`, likely `IWindowSession.addToDisplayAsUser` or `ActivityTaskManager.startActivity`) not yet reached. Coverage 14/37 unchanged (Sessions 22–28 all integration depth; no new reference methods ported since Session 14 — Rule 3 exception stated openly in every session narrative). Test suite 52 registered = 49 pass + 3 ignored. **Mission #1 remains 14/37. Wall C3 is the first wall in six sessions that may name a reference method port (`nativeInit` or a per-signature hook family member) — Session 29 diagnostic determines.**

Session 28 closed 2026-10-26: **Wall C2 CLOSED. Wall C3 NAMED. Milestone 4 advances to Activity-lifecycle-proven.**

Wall C2 (Settings ContentProvider caller identity mismatch) closed by extending the Session 23 rewriter to `IContentProvider`. Function renamed `ParcelRewriter_TryRewriteStorageRequest` → `ParcelRewriter_TryRewriteCallerIdentity`; IStorageManager branch unchanged; new IContentProvider branch. Empirical API 37 AttributionSource layout differs from AOSP: `[+0]=92(preserved) [+4]=uid→10428 [+8]=pid [+12]=tagLen [+16]=pkgLen→21 [+20]=pkgChars [+48]=null [+50]=pad [+52]=suffix`. Total rewrite +12 bytes. Verified: ~40 `CP rewrite fired: code=1/21 uid=10000->10428 pkg='com.viber.voip'->'com.ims134.hookengine' size=...→+12 delta=12 leading=92(preserved)` across the probe. Wall B non-regressed: `STORAGE rewrite fired: code=30 size=132->144 delta=12` still present.

Probe result: `newActivity.result=com.viber.voip.splash.SplashActivity`, `preAttach.mBase=androidx.appcompat.view.ContextThemeWrapper`, `preAttach.mApplication=com.viber.voip.ManifestViberApplication`, `preAttach.mWindow=com.android.internal.policy.PhoneWindow`, `onCreate.result=success`, `onStart.result=success`, `onResume.result=success`. SplashActivity is a live, attached, fully-initialized Activity instance in our process running Viber's own lifecycle callbacks. Every AIDL code path that fired during this (Settings reads, ContentProvider calls, PackageManager synthesis) was caller-identity-rewritten and accepted by the server.

Wall C3 named: `FATAL EXCEPTION: Thread-26` — `SecurityException: Permission Denial: ProcessManager.addMiuiApplicationThread() from pid=21318, uid=10428` at `miui.process.ProcessManagerProxy.addMiuiApplicationThread(ProcessManagerNative.java:657)` ← `miui.process.ProcessManager.addMiuiApplicationThread(ProcessManager.java:343)` ← `miui.contentcatcher.InterceptorProxy.lambda$addMiuiApplication$0(InterceptorProxy.java:123)`. MIUI contentcatcher interceptor spawns `Thread-26` during Activity creation; MIUI's server-side `Binder.getCallingUid()` check rejects because our kernel uid (10428) doesn't own `com.viber.voip`. Non-blocking: main thread continued after the background crash — `onCreate.result=success` at `07:05:40.794`, `onStart.result=success` at `07:05:40.795`, `onResume.result=success` at `07:05:40.811` — the FATAL fired at `07:05:40.748`, 46ms before onCreate returned. **Wall C3 rewritability TBD** — if the `addMiuiApplicationThread` parcel has no caller-identity field, the Wall B/C2 rewrite mechanism can't help. Session 29 diagnostic determines. Root-cause context: `Process.myUid()` returns 10000 in our process (Session 22 log: `FixApplicationInfo: ... uid=10000`), so caller-identity fields carry uid=10000; Wall C3's rejection is on kernel-side `Binder.getCallingUid()` (10428), which is different from any parcel field. Consequence if unfixed: MIUI interceptor doesn't attach to guest UI, downstream MIUI behavior (crash detection, memory management, screenshots) may be degraded for the guest.

Coverage 14/37 unchanged. Test suite 52 registered = 49 pass + 3 ignored unchanged. Mission #1 reconfirmed: **Wall C3 is the first wall in six sessions that may name a reference method port if the fix requires `nativeInit` or a per-signature hook family member.** Session 29 diagnosis is the gate.

Session 27 closed 2026-10-26: **Wall C2 refined, not fixed. Diagnostic dump captured.** First IContentProvider rewrite attempt assumed AOSP layout (`[uid][pid][pkg]`). Actual API 37 layout: `[92][uid][pid][tagLen][pkgLen][pkgChars]`. Read started at `[+0]=uid` — off by 4 bytes, plus another 4 for the tag field. Diagnostic-only: rewriter safely no-op'd on every attempt (`CP iface matched but pkg read failed aS=84 bytes: 5c000000 10270000 10270000 00000000 0e000000 63006f00...`), no partial corruption, no crash. Wall B fix intact (`STORAGE rewrite fired` present). Wall C1 operationally closed. Coverage 14/37 unchanged. Handoff withheld.

Session 26 closed 2026-10-26: **Wall C1 closed, Wall C2 named.** Wall C1 fix (Session 25 diagnosis: `Activity.<init>` needs Looper on caller thread) applied — probe body wrapped in `Handler(Looper.getMainLooper()).post { ... }` with `CountDownLatch` sync. Verified: `mainThread.name=main`, `mainThread.hasLooper=true`. `newActivity` advanced past `SplashActivity.<init>` — no second Looper wall. New wall: `SecurityException: Calling uid: 10428 doesn't match source uid: 10000` at `ContentProviderProxy.call(ContentProviderNative.java:784)` ← `Settings.Global.getInt` ← `PhoneWindow.<init>` (428/440) ← `Activity.attach(9482)` ← `Instrumentation.newActivity(1446)`. Wall C2 = Settings ContentProvider rejects caller identity. Category: Wall B-adjacent (same caller-identity mismatch mechanism, different AIDL interface — `IContentProvider` vs `IStorageManager`). Session 26 corrections: (1) `Instrumentation.newActivity` DOES call `Activity.attach` internally (stack trace proves it) — probe's explicit-attach block was dead code, removed in Session 27; (2) no second Looper wall — one `new Handler()` in `Activity.<init>`, posting to main cleared it. Coverage 14/37 unchanged.

Session 25 closed 2026-10-26: **Milestone 4 first-wall probe. Wall C1 named.** `ActivityBootProbeTest.kt` created: boots Viber via `GuestHost.load`, enumerates 347 activities, picks `com.viber.voip.splash.SplashActivity` as launcher, resolves `ActivityThread.currentActivityThread()` + `mInstrumentation` (= `AndroidJUnitRunner`), calls `Instrumentation.newActivity(...)`. Reached `Class.newInstance()` → `SplashActivity.<init>` → 8-layer androidx chain (`AppCompatActivity` → `FragmentActivity` → `androidx.activity.ComponentActivity` → `androidx.core.app.ComponentActivity` → `android.app.Activity`) → `Activity.<init>(Activity.java:1018)` → `new Handler()` → `RuntimeException: Can't create handler inside thread Thread[Instr: androidx.test.runner.AndroidJUnitRunner,5,main] that has not called Looper.prepare()`. **Wall C1 = Activity.<init> requires Looper on caller thread.** Container-side constraint (framework Handler ctor requires Looper), not Viber-specific. Every Activity on Android throws the same way on a Looper-less thread. Fix deferred to Session 26. Key findings: launcher is SplashActivity of 347 declared activities; SplashActivity class hierarchy is standard androidx; `ActivityThread.currentActivityThread()` and `mInstrumentation` both resolvable via HiddenApiBridge. Coverage 14/37 unchanged.

Session 24 closed 2026-10-26: **Scoping session. Option 1 chosen — milestone 3 accepted, "Viber boot" chapter closed.** Operator delegated the decision. Rationale: Wall A and Wall B are clean verifiable wins; milestone 3 verified 3× by the same methodology that found both walls; starting milestone 4 in the same mental session as closing milestone 3 mixes two cost domains. Zero code changes this session. Session 22 + 23 commit remains at HEAD. Coverage 14/37 unchanged. Test suite 52 registered unchanged. Session 25 starts milestone 4 as a new project.

Session 23 closed 2026-10-26: Wall B closed with `ParcelRewriter_TryRewriteStorageRequest` — intercepts `IStorageManager` transact prelude on the caller thread, rewrites `callingPackage` string16 from `"com.viber.voip"` (14 chars) to `"com.ims134.hookengine"` (21 chars), grows parcel by +12 bytes, forwards. AMS accepts (uid 10428 owns `com.ims134.hookengine`). Hook installed at `GuestHost.load` entry before guest boot. Verified 3 runs: `STORAGE rewrite fired: code=30 userId=0 size=132->144 delta=12` present each time; no `IO_POOL`/`TM:MESSAGES` crash, no `FATAL EXCEPTION`. Milestone 3 verified by 60s probe: no crash in 60s, two background IPackageManager IPC calls observed (code 3 at 3.5s post-boot, code 36 at 60s post-boot) — Viber Engine threads alive and doing work. Coverage 14/37 (unchanged — integration depth). Test suite unchanged: 52 registered = 49 pass + 3 ignored.

Session 22 closed 2026-10-26: Wall A resolved. Viber's `ViberApplication.onCreate` throws `IllegalArgumentException: unknown component class ii` at line 65 (inside `gu4.u`, a class-name registry lookup, not a PackageManager call). Providers had run 14/15 despite the throw — proof the DI (`bb2`) is set at lines 1-64. Fix: swallow the exception in `nativeCreateGuest` with correct JNI semantics — capture throwable ref, `ExceptionClear()`, THEN `FindClass` (previous attempt called `FindClass` with pending exception → JNI abort → SIGABRT). Providers went 8/15 → 14/15 with `Process.setArgV0(guestPkg)` (reflection, hidden on API 37 direct lookup) and `PatchActivityThreadState` (patches `mBoundApplication.processName`, `mBoundApplication.appInfo`, `mInitialApplication` after `makeApplication`). LoadedApk path works with `mClassLoader`/`mResources` pre-populated before `makeApplication`, skipping `canAccessDataDir` → `NETWORK_STACK` wall. Wall B surfaced: `IStorageManager.getVolumeList` rejects `callingPackage="com.viber.voip"` because our UID 10428 doesn't own that package.

Session 16 closed 2026-10-25: `:remote` `startService` flake investigated. Root cause is AMS state — process is marked bad before our first `startService` at IntegrationTest.kt:21 — not a code crash. Passing runs show `:remote` starting cleanly (`attachBaseContext` → `onCreate` → `onStartCommand` → cleanup kill; no FATAL, no signal, no ScopedCheck crash). Failure rate observed across Sessions 13–16: 3 pass / 2 fail. Mitigation: retry-with-backoff (3 attempts, 2s/4s) in `IntegrationTest.selftestReportIsComplete`. Retry recovery path unverified — only the green path executed post-mitigation. Coverage 14/90, unchanged.

Session 15 closed 2026-10-25: HANDLEJHMETHOD_DECOMP_SPEC.md added at repo root. ProxyBridge.cpp header points at it. InlineHooker.cpp gained `g_iihe_quiet`; RunRegressionBug4 gates it around the 1000-cycle stress loop (eliminates ~15,000 `IIHE(bug4):` log lines). AntiDetectionE2ETest three connect tests `@Ignore`'d (Rule 12 honesty; were passing-by-skip on loopback EPERM). 49 pass, 3 ignored, 0 fail. Coverage 14/90.

Session 14 closed 2026-10-25: two real bugs fixed from Claude code review of Session 13's nativeCNVM: (1) nativeCNVM arity guard read `strlen(sig)` AFTER `ReleaseStringUTFChars` — UB, latent UAF, fixed by capturing sigLen before release; (2) `g_rewrite_effective_count` incremented unconditionally after the swap block — counted rewriter throws and zero-length results as "effective", fixed by gating on local `swapped` bool. `ProxyBridgeSurfaceTest` observed running for the first time in `am instrument -r` output. 52 tests, 0 failed. Coverage 14/90 verified.

Session 13 closed 2026-10-25: nativeCNVM decomp-verified vs sub_30BF7C + sub_30C238; 4 tests green in ProxyBridgeSurfaceTest. handleJHMethod DEFERRED (no sub_29E168 decomp). Session 13 claimed 52 tests / 51 pass / 1 fail; actual measurement was 48 running, Session 14 corrected this.

Session 12 closed 2026-10-24: Q1 flags-check measured (code 36 only matters for signature-flag callers), code-36 byte-rewrite deferred, exception-path test added, AidlProbe narrowed. 48 tests green. Coverage 13/90 (unchanged — pivot to handleJHMethod next).

Session 11 closed 2026-10-24: tnFIN rewriter exception safety, drainCodes stub fix, code-36 reply captured + hex-dumped. 47 tests green. Coverage 13/90 (unchanged — code 36 not delivered).

Completed:
- Registry finding DISPROVED — RegistryCount() counts inline hooks, regDuring=1 observed.
- §7.1 — 23 sites migrated to testreport::Report.
- §7.2 — test_suite.cpp dispatch.
- §7.3 — JNI bridge (NativeTestBridge.runSuite / uninstallAll / setItem4Paths).
- §7.4 — NativeTestBridge.kt Kotlin object.
- §7.5 — NativeItemTest.kt parameterized runner.
- §7.6 — regression suites bug1–bug5.
- §7.7 — selftest.txt file channel + IntegrationTest.kt.
- §7.9 — build-only CI + README limitations. Item 7 CLOSED.
- Phase 2 — 40 per-function reloc cases.
- Mission #1 Phase A: tnFIN mechanism (Session 8), rewriter algorithm (Session 9), production delivery for code 3 (Session 10), code-36 dispatch confirmed (Session 11).
- Mission #1 Phase A: nativeCNVM decomp-verified + tested (Sessions 13–14).
- Session 15: HANDLEJHMETHOD_DECOMP_SPEC.md ships.
- Session 16: `:remote` flake characterized as AMS state; retry-with-backoff mitigation.
- Session 22: Wall A RESOLVED — Viber DI `unknown component class ii` swallowed via JNI-safe exception capture. Providers 8/15 → 14/15.
- Session 23: Wall B CLOSED — `ParcelRewriter_TryRewriteStorageRequest` for IStorageManager callingPackage. 3 runs verified. Milestone 3 verified.
- Session 24: milestone 3 accepted, "Viber boot" chapter closed. Zero code change.
- Session 25: milestone 4 first-wall probe delivered. Wall C1 named (Activity.<init> requires Looper).
- Session 26: Wall C1 closed (post-to-main fix). Wall C2 named (Settings ContentProvider caller identity).
- Session 27: Wall C2 diagnostic dump captured. Layout off by 4 bytes. Safe no-op.
- Session 28: Wall C2 CLOSED (IContentProvider AttributionSource rewrite verified). Wall C3 named (MIUI ProcessManager kernel-uid rejection). Milestone 4: SplashActivity `onCreate`+`onStart`+`onResume` all succeeded.

Pending:
- Mission #1 Phase A: tnFIN code 36 byte-rewrite (Session 12+, deferred pending target need or decomp).
- Mission #1 Phase A: handleJHMethod — BLOCKED on sub_29E168 decomp. HANDLEJHMETHOD_DECOMP_SPEC.md specifies the 7 questions whose answers unblock implementation.
- Mission #1 Phase A: nativeInit full config.
- Mission #1 Phase A: nativeOnLoad full DEX extract.
- `IntegrationTest.selftestReportIsComplete` retry recovery: not yet observed firing. If a future run logs `startService attempt N failed`, that is the verification signal.
- **Wall C3 diagnostic + conditional fix** (Session 29): determine `addMiuiApplicationThread` parcel layout; if rewritable, add third iface case; if not, document as unrewritable and defer `ProcessManagerService` server-side hook. May name a reference method port (`nativeInit`).
- **Wall C4 probe** (Session 29): extend ActivityBootProbeTest past `onResume` into `WindowManager.addView` / `IWindowSession.addToDisplayAsUser`. Wall C4 likely third instance of Wall B/C2 pattern (extends `tnFIN` further) or new Surface/AMS category.
- **Wall C5** (Session 30+): `ActivityTaskManager.startActivity` with spoofed ComponentName. AMS task record + launch flags + Surface render.

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

1. `:app:connectedDebugAndroidTest` — fails to install the test APK on this HyperOS device with `SecurityException: INSTALL_GRANT_RUNTIME_PERMISSIONS` (UTP-injected flag, refused by MIUI PackageInstallerService), logs `AndroidTestRunner failed`, and then reports `BUILD SUCCESSFUL`. Zero tests ran; the task returned green.

2. `am instrument -w` — returns `INSTRUMENTATION_CODE: -1` (Activity.RESULT_OK) on both success and failure. `Tests run: 6, Failures: 1` was observed with `INSTRUMENTATION_CODE: -1`. Exit code is 0 either way.

3. Consequence: no automated signal from the device is trustworthy as a CI gate. Reliable gate is parsing output:
- **Pass:** `OK (N tests)` (standard AndroidJUnitRunner success line)
- **Pass:** `run finished: N tests, X failed, Y ignored` with X == 0 (custom line from a wrapper listener; only appears on green runs)
- **Fail:** `FAILURES!!!` followed by `Tests run: N, Failures: M` (standard JUnit failure line; the `OK` and `run finished` lines do NOT appear)
- **Additional fail signal:** `INSTRUMENTATION_STATUS_CODE: -2` per-test

**Session 16 correction:** Both `OK (N tests)` and `run finished:` indicate all-pass; the failing run emits neither. `INSTRUMENTATION_CODE: -1` remains untrustworthy in isolation.

Verified device workflow (only method that works on this device):

    adb install -r -t app/build/outputs/apk/debug/app-debug.apk
    adb install -r -t app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
    adb logcat -c
    adb shell am instrument -w -r com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner

adb install succeeds where UTP does not — the security exception is specific to the UTP install path, not to pm install. CI (§7.9) stays build-only. README must document this.

Session 14 addendum: the Session 13 baseline `OK (48 tests)` was not a full measurement; Session 14's `run finished: 52 tests, 0 failed, 0 ignored` was the first run in which `ProxyBridgeSurfaceTest` appeared. Session 15's `run finished: 49 tests, 0 failed, 3 ignored` is 49+3 = 52 total registered. Session 16's `OK (49 tests)` confirms 49 running (3 `@Ignore`'d tests not counted in JUnit `Tests run:` total).

---

## Known issue — RESOLVED: duplicate log blocks, not duplicate runs

Suspected "two PROCESS STARTED lines and Item 1 block appearing three times" is expected:

- Two PROCESS STARTED: main + :remote processes. One line per process.
- Three emissions: per-test `testreport::Report()`, `LOGD("%s", out.str())` at function end, `Log.d("DualCore", runItemNTests())` in `RemoteService.onStartCommand`.

Same single run. Closed. Item 7 implication: the file-based channel (§7.7) is mandatory. Logcat scraping would count 3×.

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
| RemoteService.kt | Service started. (v27) → (v29) | applied |
| Known issue | Duplicate log blocks — RESOLVED | applied |

---

## Item 7 — deliverable status

Design decisions:

1. File-based result channel, not logcat scraping (§7.7).
2. C++ stays in C++. Primitives (ReadPageProt, AllocateMemoryNear, ShouldHookDlopen) live natively.
3. Aggregate runner with per-test reporting, not one JUnit method per C++ function.

### §7.1 — CLOSED 2026-10-23
app/src/main/cpp/TestReport.h added. All 23 [PASS]/[FAIL] sites migrated. Logcat output byte-identical.

### §7.2 — CLOSED 2026-10-23
test_suite.h / test_suite.cpp added. `RunTestSuite(env, name)` dispatches. Logcat byte-identical.

### §7.3 — CLOSED 2026-10-23
native-lib.cpp gained three JNI exports: `runSuite`, `uninstallAll`, `setItem4Paths`.

### §7.4 — CLOSED 2026-10-23
NativeTestBridge.kt Kotlin object.

### §7.5 — CLOSED 2026-10-23
NativeItemTest.kt parameterized runner, 5 cases. Device: `OK (5 tests)`.

### §7.8 — CLOSED 2026-10-23
app/build.gradle.kts: testInstrumentationRunner, testBuildType, ndk arm64-v8a filter, androidTestImplementation.

---

## Assumed symbols — verified against actual headers

| Assumed | Verified |
|---|---|
| InstallInlineHookEx(void*, void*, void**, const char*) returns HookResult with HOOK_OK=0 | matches HookRegistry.h |
| UninstallInlineHook(void*) returns HookResult | yes |
| SetUninstallGraceMs(long) exists | yes, InlineHooker.cpp |
| RunItemNTests() callable | refactored to shared RunItemNInternal in §7.2 |
| Native lib name is hookengine | yes |
| installPmSignatureSpoof callable from test context | NO — private to RemoteService. |
| RegistryCount() excludes inline hooks | DISPROVED — counts them |
| InstallInlineHookEx accepts `nullptr` name safely | YES — guarded (Session 15 verified) |

---

## Item 7 — CLOSED 2026-10-23

Final device state: `am instrument -w -r` → `numtests=11`, `OK (11 tests)`. 33 underlying assertions, 0 failures.

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

40 cases across `ClassifyInsn` (20), `EncodeBranch` (8), `EmitRelocated` (12). Device: `numtests=12`, `OK (12 tests)`.

Two test bugs found and fixed: `+(1LL<<27)` out-of-range in "edge in"; `make_adr(1, 0x200000)` overflow producing wrong `n`.

---

## API 37 finding — Process.MY_UID / MY_PID do not exist — 2026-10-23

`GetStaticFieldID(Process, "MY_UID", "I")` returns null + `NoSuchFieldError` on API 37. `Process.myUid()` / `Process.myPid()` are fast-JNI natives; no static field.

What works: `Binder.getCallingUid()` ArtMethod hook, `libc::getpid` inline hook, `getuid/geteuid/getgid/getegid` GOT hooks, `execve/execv/execvp/system/ptrace/kill` inline hooks.

What does NOT work: field spoofing of MY_UID/MY_PID (fields don't exist), ArtMethod hook of myUid/myPid/myTid/myTgid (fast-JNI tokens).

Consequence: `Process.myPpid` hook often not installed (fast-JNI, count 8 not 9, test accepts). Batch 2a as described should be retired. To spoof UID as seen by the target: (a) `Binder.getCallingUid()` — covered by Item 5, (b) `Process.myUid()` — needs native `getuid` hook (already in Hooks.cpp), (c) direct `getuid` syscall — out of scope.

---

## Test suite status — 52 registered (49 pass + 3 ignored), 0 fail

Classes (Session 16 measurement, `OK (49 tests)`):
- IntegrationTest (1) — GREEN
- NativeItemTest (5) — GREEN
- ProcessHooksTest (4) — GREEN
- RegressionTest (5) — GREEN
- RelocTest (1) — GREEN
- ProxyBridgeSurfaceTest (4) — GREEN
- AntiDetectionTest (1) — GREEN
- AntiDetectionE2ETest (4) — `javaFileHidingFiresHook` GREEN, three connect tests `@Ignore`'d
- PmSpoofTest (4) — GREEN
- ClassLoaderBridgeTest (3) — GREEN
- ClassLoaderProbeTest (1) — diagnostic
- DualApplicationTest (2) — GREEN
- BinderTransactE2ETest (9) — GREEN
- BinderTransactHookTest (2) — GREEN
- BinderTransactProbeTest (1) — diagnostic
- BinderProbeTest (1) — diagnostic
- AidlCodeProbeTest (1) — diagnostic
- Session11Code36ReplyTest (1) — diagnostic
- Session11ProbeTest (2) — diagnostic
- GuestLoadTest (1) — outcome probe; Rule-12-compliant
- ActivityBootProbeTest (1) — milestone 4 probe; Rule-12-compliant

Total 49 running + 3 ignored = 52 registered. 0 failed.

---

## PmSpoof filter delivered; hidden API bypass via thread-attached reflection

Two fixes: (1) `CheckHookCapability()` called inside `JavaHookInstall` (previously only `HookJavaMethod` was gated). (2) `Class.getDeclaredField` / `getDeclaredMethod` from `AttachCurrentThread`'d thread bypasses hidden-API caller-domain check. Resolves `BaseParceledListSlice.mList`, enabling PmSpoof list filtering.

Test suite: 21/21 green. PmSpoof 4/4. Process hooks 9/9.

---

## Batch 2c — DEFERRED pending design decision — 2026-10-23

Probe results on API 37 / HyperOS (`ClassLoaderProbeTest`): every class-loading entry point is a Java method. `JavaHookInstall` cannot hook any of them. Reference NativeEngine doesn't hook loadClass either — it caches `DexFile.openDexFileNative` (DEX-file-swapping, different architecture).

Three viable paths: (1) Java proxy on guest ClassLoader — high cost, fragile; (2) DEX-file-swapping via openDexFileNative hook — very high cost, matches reference; (3) Rely on guest/host classloader compatibility — zero cost, verify at integration time.

Recommendation: path 3 first, path 1 if needed, path 2 only if path 1 insufficient.

---

## MISSION #1 — Reproduce ApplicationProxy — declared 2026-10-24

### Scope (permanent, supersedes all prior plans)

Replicate `com.excelliance.kxqp.platform.ApplicationProxy` (decompiled) completely before any DualHost integration. **Denominator corrected to 37 in Session 20** — `JNI_OnLoad` shows `RegisterNatives(proxy, table, 0x25)` = 37, not 90. DualHost integration unblocked (Session 18). Reference reproduction is pull-driven: decomp a method when a real integration failure names it.

### Current coverage: 14 / 37 (~38%)

Verified via `am instrument -r` in Session 14. Sessions 15–28 all integration depth, no new ports. Honest no-growth on coverage counter; large growth on "usable" dimension and integration behavior.

**Delivered (matches reference):**

| Method | Ours | Status |
|---|---|---|
| `attachBaseContext` | `DualApplication.attachBaseContext` | ✅ |
| `onCreate` | `DualApplication.onCreate` | ✅ |
| `nativeOnLoad` | `DualApplication.cpp::nativeOnLoad` | ⚠️ Partial — records paths |
| `nativeFNLN` | `ClassLoaderBridge.installFor` | ✅ |
| `getCallingUid` | `FakeBinderGetCallingUid` | ✅ |
| `is64Bit` | Implicit (arm64-only) | ✅ |
| `nativeGetCpuABI` | Implicit (arm64-only) | ✅ |
| `tnFIN` | `BinderTransactHook` + `ParcelRewriter` + `CppParcelRewriter` | ✅ Code 3, code 36 dispatch, code 30 storage (Wall B), code 1/21 IContentProvider (Wall C2). Third iface (Wall C3, Session 29) TBD. |
| `nativeDNPO` | TracerPid filter | ⚠️ Partial |
| `nativeAMRHook` | Path hide list | ⚠️ Partial |
| `nativeCNVM` | `ProxyBridge.nativeCNVM` | ✅ decomp-verified, 4 tests |
| `handleJHMethod` | — | ⏸️ PARKED (Session 18) |

**Not started (~23 methods):** `nativeInit` (3 params), `nativeOnLoad` (full DEX extract), tnFIN code 36 byte-rewrite, per-signature hook family (13), field/method getters (~15), anti-detection remaining (~20), file/IO (~5), NLDS (~2), ANR/crash/debug (~13), misc (~7), public instance methods (~2), static init (DEX extraction).

**Wall C3 note (Session 28 close):** Wall C3 fix (Session 29) may be the first wall since Session 14 to require a reference method port. If `miui.process.ProcessManager` check is served from `nativeInit` (3-param config) or a per-signature hook family member, the port becomes the fix target, and coverage grows for the first time in seven sessions.

### Order of attack

**Phase A — Core engine (blocking everything else):**
1. ✅ `tnFIN` — mechanism, algorithm, code 3, code 36 dispatch, code 30 storage, code 1/21 IContentProvider
2. ⏸️ `tnFIN` code 36 byte-rewrite — DEFERRED (Session 12)
3. ⚠️ `nativeCNVM` ✅; `handleJHMethod` ⏸️ DEFERRED
4. 🔲 `nativeInit` (full config)
5. 🔲 `nativeOnLoad` (full DEX extract, if needed)

**Phase B — Java hook surface:** per-signature hook family (13), field/method getters (~15).
**Phase C — Anti-detection full suite (~20).**
**Phase D — ANR / crash / debug (~13).**
**Phase E — Misc + public instance methods.**

### Rules of engagement

1. **Every session opens with the mission header.** No exceptions.
2. **DualHost integration may proceed at any coverage level.** Reference reproduction is best-effort per method: a specific reference method is decomp'd and ported when a logged integration failure names it, not before.
3. **Coverage number in every message.** 14/37 now. Must be higher every session, with the honest exception stated when decomp or integration depth blocks all growth. Sessions 11, 12, 15, 16, 17, 18, 19+20, 22, 23, 24, 25, 26, 27, 28 all did not grow it; all stated so openly.
4. **Every method shipped must have a test.**
5. **Do not declare a method "done" unless it matches the reference's behavior.**
6. **Session-by-session commit discipline.** Green or the session is not done.
7. **IDA-first when wire format, symbol contract, or field layout is uncertain.** Do not iterate on offset guesses.
8. **Any `jobject`-touching JNI call on a caller thread with a JIT BinderProxy.transact frame above it crashes on this ROM.** Hook at C++ layer or offload to fresh attached thread.
9. **dlsym filters `android::Parcel` primitives and `IPCThreadState::transact` on this ROM.** Use dynsym scan.
10. **Reply parcel's `mData` is read-only post-transact.** Never write into it. Swap the pointer.
11. **Kotlin `object` instance fields are not static.** Use `@JvmStatic` accessors.
12. **Test assertions must be able to fail.** Rule 12 stands.
13. **Handoffs deliver whole-file replacements or well-delimited contiguous-block replacements.** Every handoff must include (a) change summary per file, (b) paste content, (c) verify-before-commit checklist, (d) explicit git commit block. **Session 28 addendum: full replacement preferred over delimited edits for `PROJECT_STATE.md` when operator asks; delimited edits acceptable for large C++ files.**
14. **Every test-run instruction must include rebuild + `adb install -r -t` of both APKs.**
15. **Engine behavior is native.** Kotlin/Java in `app/src/main` limited to: JNI surface declarations, `DualApplication`, and pre-port Kotlin proxies (`PmSpoof`, `ParcelRewriter`, `BinderTransactHook`, `ClassLoaderBridge`, `GuestHost` harness). Test files (androidTest) may be Kotlin. Kotlin engine code additions forbidden.

---

## Session 7 — BinderProbe results + strategy correction — 2026-10-24

`BinderProbe.cpp` scans `libbinder.so` `.dynsym`. Key findings:

1. **`BinderProxy::transact` is NOT a C++ symbol.** Java `native` method via `RegisterNatives` in `libandroid_runtime.so`.
2. **Reference `tnFIN` signature confirms** — JNI signature of hooked `BinderProxy.transact`.
3. **All Parcel I/O methods exported** in `libbinder.so` (data, dataSize, dataPosition, setDataPosition, readInt32, writeInt32, readString16, etc.).
4. **`IPCThreadState::transact` IS exported** — `_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j`.

Strategy correction (superseded by Session 10): hook Java `BinderProxy.transact`, then pivot to `IPCThreadState::transact`.

---

## Session 8 — tnFIN mechanism delivered — 2026-10-24

`BinderProxy.transactNative` hooked via `JavaHookInstall`. Key findings: (1) `transact` is Java wrapper, (2) `transactNative` is native, (3) `data_` (offset 24) is JIT stub not libart pointer, (4) `FindEntrySlot` accepts any executable `data_ >= 0x1000`, (5) `Binder.getCallingUid` still hooks via old `dladdr` path.

---

## Session 9 — tnFIN rewriter algorithm delivered — 2026-10-24

Rewriter algorithm + wire format IDA-verified. Bugs fixed: (18) missed PARCEL_HEADER_MAGIC at offset 8, (19) premise-broken test deleted, (20) premise-broken test replaced. Rules-amendment: request IDA decomp first, do not iterate on offset guesses.

---

## Session 10 — tnFIN production delivery (code 3) — 2026-10-24

Pivot: `IPCThreadState::transact` pure C++ hook, JNI on fresh thread. Bugs fixed: 21 (SIGSEGV from jobject-touching JNI on caller thread), 22 (dlsym filtering), 23 (symbol mangling wrong), 24 (Kotlin object field not static), 25 (write to read-only `mData`), 26 (`mDataPos` set to end-of-data). `OK (43 tests)`.

---

## Session 11 — rewriter exception safety + code-36 diagnostics — 2026-10-24

Diagnostic foundation: exception preservation, `drainCodes` real impl, `g_rewrite_effective_count`, 66 KB code-36 reply captured. `OK (47 tests)`. Coverage unchanged (honest no-growth).

---

## Session 12 — Q1 measured, code 36 deprioritized — 2026-10-24

`getInstalledPackages(0)` → 278 pkgs, 0 with non-null sigs. `getInstalledPackages(GET_SIGNATURES)` → 278 with sigs. Decision: deprioritize code 36, move to nativeCNVM + handleJHMethod. 48 tests green.

---

## Session 13 — nativeCNVM decomp-verified — 2026-10-25

Decomp-verified vs `sub_30BF7C` + `sub_30C238`. `ProxyBridgeSurfaceTest.kt` added (4 tests). `handleJHMethod` stub. `:remote` flake surfaced.

---

## Session 14 — nativeCNVM two-bug fix + test measurement — 2026-10-25

Bugs fixed: (27) UAF in arity guard, (28) `g_rewrite_effective_count` unconditional increment. Measured `run finished: 52 tests, 0 failed`. `rewriteEffective=3` matches exactly.

---

## Session 15 — handleJHMethod decomp spec + loose-end cleanup — 2026-10-25

`HANDLEJHMETHOD_DECOMP_SPEC.md` at repo root. AntiDetectionE2ETest Rule-12 fix (3 `@Ignore`'d). `g_iihe_quiet` for bug4 noise suppression. 49 pass, 3 ignored.

---

## Session 16 — `:remote` flake investigated, retry mitigated, retry path unverified — 2026-10-25

Failure: `SecurityException: ... process is bad` at `IntegrationTest.kt:21`. AMS state, not code crash. Retry-with-backoff added. Retry recovery path unverified. 3 pass / 2 fail across Sessions 13–16.

---

## Session 17 — stale test APK discovered — 2026-10-25

Four-session-old discipline gap surfaced: no session from 14 onward instructed `adb install -r -t`. Every "pass" through 17c ran the pre-retry test APK. Rule 14 added.

---

## Session 18b/18c/18d — guest visibility + signature null-out

- 18b — `probeGuestSignatureSpoof`. First run: `NameNotFoundException` (package visibility).
- 18c — `<queries><package android:name="com.viber.voip" /></queries>` added. Four diagnostic rows all succeeded.
- 18d — `PmSpoof.signatureNullOutPackages` set added. First pivot-driven behavior extension.

---

## Session 18 — DualHost pivot, Viber Application boots — 2026-10-25

Mission #1 status: 14/90 — coverage unchanged, pivot validated. Rule 2 amended: DualHost integration unblocked, reference reproduction pull-driven. `handleJHMethod` parked indefinitely per operator.

Deliverables: `GuestHost.kt`, `GuestClassLoader.kt`, `GuestLoadTest.kt`. Four walls closed: (1) writable dex file, (2) `attachBaseContext` hierarchy, (3) Kotlin stdlib shadow, (4) `outcome=booted`.

Rule 12 note: `GuestLoadTest` asserts only `result.outcome != "?"`. Boot success is data, not gate.

Coverage: 14/90 (unchanged). Pivot is strategic realignment, not coverage growth.

---

## Session 19 — ContentProvider installation probe — 2026-10-25

`GuestHost.installGuestProviders` added. 15 declared, 7 succeeded, 8 failed. Root cause: `GuestContext.getResources()` returned host resources, Viber's `androidx.startup.InitializationProvider` string lookup failed, cascade into `bb2.*` NPEs.

No anti-tamper observed — positive signal for PmSpoof + ParcelRewriter null-out path. MlKitInitProvider "already initialized" is test-env noise.

Next: fix `GuestContext.getResources()` to `pm.getResourcesForApplication(guestAppInfo)`.

---

## Session 19+20 — native boot via ApplicationProxy; provider wall named — 2026-10-26

Mission #1 status: 14/37 (denominator corrected from 90 in Session 20). Reference decomp: `KXQPApplication` → `VmApplication` → `ApplicationProxy` → guest Application boot. `ApplicationProxy` is not in manifest.

Deliverables: `ApplicationProxy.kt`, `ApplicationProxy.cpp`, `DualApplication.kt` reference-shape, `GuestHost.kt` test fallback, `GuestClassLoader.kt` guest-first delegation, `GuestLoadTest.kt`, `<queries>` manifest, `PmSpoof.signatureNullOutPackages`.

Verified: `outcome=booted-at-startup`, `instantiated=true attached=true onCreated=true`.

The wall: `androidx.startup.InitializationProvider` → `getProviderInfo` → AMS `NameNotFoundException`. 7/15 providers succeed. Not implemented, not touched: `BinderTransactHook` interception of `IPackageManager.getProviderInfo`.

Corrections: `nativeInit` is 3 params not 9; `nativeInit` (arm64) == `nativeOnLoad` (x86); reference registers 37 not 90; `ApplicationProxy` not in manifest; Session 19e's `LoadedApk.makeApplication` path was our invention.

---

## Session 22 — Wall A closed, providers 8/15 → 14/15 — 2026-10-26

Mission #1 status: 14/37 coverage, integration depth up. Two fixes:

**Fix 1** — `Process.setArgV0` via reflection (hidden on API 37). `ReflectGetDeclaredMethod(env, Process.class, "setArgV0", {String.class})` via `HiddenApiBridge`, then invoke. Works.

**Fix 2** — `PatchActivityThreadState` after `makeApplication`. Patches `mBoundApplication.processName`, `mBoundApplication.appInfo`, `mInitialApplication`. Providers jumped to 14/15.

**Bonus** — `LoadedApk` path works with `mClassLoader`/`mResources` pre-populated. Skips `canAccessDataDir` → `NETWORK_STACK` wall.

**Wall A** — `ViberApplication.onCreate` line 65 throws `IllegalArgumentException: unknown component class ii` (gu4.u, Viber-internal registry lookup). Fixed by swallow in `nativeCreateGuest` with correct JNI semantics: get throwable ref → `ExceptionClear()` → THEN `FindClass`. Previous version called `FindClass` with pending exception → JNI abort → SIGABRT.

**Wall B** — `IStorageManager.getVolumeList(uid=10428, callingPackage="com.viber.voip")` rejected. Not addressed in Session 22.

Files: `ApplicationProxy.kt`, `DualApplication.kt`, `GuestClassLoader.kt`, `GuestHost.kt`, `PmSpoof.kt`, `ApplicationProxy.cpp`, `PROJECT_STATE.md`.

Coverage: 14/37 unchanged. Rule 3 exception stated openly.

---

## Session 23 — Wall B closed; milestone 3 verified — 2026-10-26

Mission #1 status: 14/37 coverage, milestone 3 verified.

Wall B fix: `ParcelRewriter_TryRewriteStorageRequest` in `CppParcelRewriter.cpp`. Called from `Hook_Transact` before `_orig_transact`. Parses AIDL request prelude, rewrites `callingPackage` from `"com.viber.voip"` (14 chars) to `"com.ims134.hookengine"` (21 chars), +12 byte delta.

Hook installed at `GuestHost.load` entry before guest boot. Verified 3 runs: `STORAGE rewrite fired: code=30 userId=0 size=132->144 delta=12` each time.

Milestone 3 verified by 60s probe: no crash, background IPC at 3.5s (code 3) and 60s (code 36). Zero FATAL, zero signal, zero ANR.

Files: `CppParcelRewriter.cpp`, `BinderTransactHook.cpp`, `GuestHost.kt`, `GuestLoadTest.kt`.

Coverage: 14/37 unchanged. Rule 3 exception stated openly.

---

## Session 24 — scoping decision, milestone 3 accepted — 2026-10-26

See Session 24 close narrative at top.

---

## Hard-won facts (canonical reference)

- **Under `am instrument`, ALL `jobject`-touching JNI calls crash on a caller thread with a JIT `BinderProxy.transact` frame above it.** Not just `NewGlobalRef`. (Rule 8)

- **`android::IPCThreadState::transact` is the correct hook for AIDL interception on this ROM.** Symbol `_ZN7android14IPCThreadState8transactEijRKNS_6ParcelEPS1_j`, in `libbinder.so` dynsym.

- **dlsym filters android::Parcel primitives on this ROM.** Use dynsym scan. (Rule 9)

- **`android::Parcel` field offsets (IDA-verified, API 37, arm64):**
    - `+0` mError (int32)
    - `+8` mData (void*)
    - `+16` mDataSize (size_t)
    - `+24` mDataCapacity (size_t)
    - `+32` mDataPos (size_t)
    - `+64` mNextObjectHint (size_t)
    - `+81` mParseHeader (byte), `+82` mParseHeaderPadding (byte)
    - `+88` mType (int32)

- **Reply parcel's `mData` buffer is read-only post-transact.** Never write into `mData` directly; always swap the pointer. (Rule 10)

- **The fresh-thread rewriter pattern:** `Parcel.obtain()` → `SetLongField(mNativePtr, targetPtr)` → run rewriter → `SetLongField(mNativePtr, originalPtr)` → `recycle()`.

- **Kotlin `object` instance fields are not static.** Use `@JvmStatic` accessors. (Rule 11)

- **API 37 writeInterfaceToken wire format** (Session 9, IDA-verified):
  offset 0: [int32 strictModePolicy | 0x80000000]
  offset 4: [int32 workSource]
  offset 8: [int32 PARCEL_HEADER_MAGIC = 0x53595354]
  offset 12: [int32 n]
  offset 16: [2n bytes UTF-16 interface token]
  offset ..: [int16 null terminator]
  offset ..: [pad to 4-byte boundary]
  offset ..: [AIDL args]

- **Code-36 reply on this ROM — observed layout (Session 11):** `packageName` uses `writeString8` on this ROM (MIUI change). Three int32s at 80/84/88 unverified.

- **`g_rewrite_effective_count` vs `g_rewrite_count`:** dispatched vs delivered. Session 14 fix: gate on local `swapped` bool.

- **`getPackageInfo(pkg, 0)` does not reliably fire on this ROM** due to framework caching. Only `getPackageInfo(pkg, GET_SIGNATURES)` reliably issues IPC. Use `getInstalledPackages(...)` for code-36 dispatch tests.

- **`nativeCNVM` arity guard reads `sigLen` captured BEFORE `ReleaseStringUTFChars`.** Session 14 fix. Post-release `strlen(sig)` is UB.

- **`handleJHMethod`'s scaffolding helpers exist but unreachable from stub body.** Session 15 made deliberate via HANDLEJHMETHOD_DECOMP_SPEC.md.

- **`g_iihe_quiet` in `InlineHooker.cpp`** gates verbose `IIHE(...)` LOGD lines inside `InstallInlineHookEx`. Session 15 verified: 0 `IIHE(bug4):` lines (was ~15,000).

- **Loopback bind on API 37 / HyperOS fails with `SocketException: EPERM`.** AntiDetectionE2ETest three connect tests `@Ignore`'d.

- **Pass/fail parsing from `am instrument -w -r`:** `OK (N tests)` OR `run finished: N tests, 0 failed, Y ignored`. Fail is `FAILURES!!!` + `Tests run: N, Failures: M`. `INSTRUMENTATION_CODE: -1` untrustworthy in isolation.

- **`:remote` `startService` flake is AMS state, not code crash (Session 16).** `process is bad` when AMS has marked record bad. 3 pass / 2 fail across Sessions 13–16. Retry-with-backoff mitigates symptom; recovery path unverified.

- **`Process.setArgV0` is hidden on API 37.** Fix: `ReflectGetDeclaredMethod(env, Process.class, "setArgV0", {String.class})` via `HiddenApiBridge`. Works. (Session 22)

- **`ActivityThread.mBoundApplication.processName` is not patched by `setArgV0`.** Patch separately via `PatchActivityThreadState` after `makeApplication`. (Session 22)

- **`LoadedApk` path works if `mClassLoader` and `mResources` pre-populated before `makeApplication`.** Skips `canAccessDataDir` → `NETWORK_STACK` wall. (Session 22)

- **Viber DI (`bb2`) set inside `ViberApplication.onCreate` lines 1-64, then line 65 throws `unknown component class ii`.** Swallow with correct JNI semantics: capture throwable ref, `ExceptionClear()`, THEN `FindClass`. (Session 22)

- **`IStorageManager` transacts use code 30 = `getVolumeList` on API 37.** AMS rejects `callingPackage` mismatch. Fix: rewrite callingPackage string16 (14→21 chars, +12 delta). `ParcelRewriter_TryRewriteStorageRequest` in `CppParcelRewriter.cpp` on caller thread (pure C++, no JNI). (Session 23)

- **`BinderTransactHook` must be installed BEFORE guest boot.** (Session 23)

- **Milestone 3 (engine ready) verification standard.** Same methodology that found Walls A and B. 60s probe silence + active background IPC. (Session 23)

- **MLKit init failure is env noise.** Not a Viber signal. 14/15 providers is healthy. (Session 19a)

- **API 37 `IContentProvider.AttributionSource` layout differs from AOSP** (Session 28, verified): `[+0]=leading int32 (92, purpose unknown — PRESERVE) [+4]=uid [+8]=pid [+12]=tag length (0) [+16]=pkg length [+20]=pkg chars UTF-16 [+48]=null [+50]=pad [+52]=suffix`. Total rewrite +12 bytes; uid at `[+4]`, pkg at `[+16]`, suffix from `[+52]`. Leading 92 preserved and accepted by server — not a length validated on this call path. Do not trust AOSP stack traces for this iface on this ROM.

- **Wall C2 dispatch was identical to Wall B dispatch.** `ParcelRewriter_TryRewriteCallerIdentity` handles both `IStorageManager` (code 30) and `IContentProvider` (codes 1 and 21) via iface-token match. Intended architecture for future caller-identity walls: add iface case, don't refactor.

- **`Instrumentation.newActivity` DOES call `Activity.attach` internally** (Session 26 stack trace: `Instrumentation.newActivity(1446)` → `Activity.attach(9482)`). Code assuming `newActivity` returns unattached Activity will double-attach or NPE.

- **MIUI's `miui.contentcatcher.InterceptorProxy.addMiuiApplication` spawns `Thread-26` during Activity creation** (Session 28, Wall C3). Thread-26 calls `miui.process.ProcessManager.addMiuiApplicationThread()` which MIUI's server checks with `Binder.getCallingUid()` (kernel-assigned 10428, not parcel's 10000). SecurityException → FATAL on Thread-26 → does NOT kill process. Rewritability TBD (Session 29). If unrewritable: `ProcessManagerService` server-side hook (native, aggressive) or accepted-wall documentation. Consequence: MIUI interceptor doesn't attach, downstream MIUI behavior degraded.

---

## Test count history

- Pre-Session 8: 16 classes, ~90 assertions
- Session 8: 32 tests
- Session 9: 39 tests
- Session 10: 43 tests
- Session 11: 47 tests
- Session 12: 48 tests
- Session 13: 52 (claim — unverified)
- Session 14: **52 verified** (first run with ProxyBridgeSurfaceTest in -r)
- Session 15: **52 registered = 49 pass + 3 ignored**
- Session 16: **52 registered = 49 pass + 3 ignored**
- Session 17: **52 registered = 49 pass + 3 ignored**
- Session 22: **52 registered = 49 pass + 3 ignored**
- Session 23: **52 registered = 49 pass + 3 ignored**
- Session 24: **52 registered = 49 pass + 3 ignored** (scoping session)
- Session 25: **52 registered = 49 pass + 3 ignored** (ActivityBootProbeTest added as 1-test probe, `OK (1 test)`, diagnostic)
- Session 26: **52 registered = 49 pass + 3 ignored** (probe fix + Wall C2 discovery)
- Session 27: **52 registered = 49 pass + 3 ignored** (probe with wrong layout)
- Session 28: **52 registered = 49 pass + 3 ignored** (Wall C2 fix verified, `lifecycle.result=onCreate+onStart+onResume all succeeded`, background Thread-26 FATAL doesn't fail probe)

---

## Coverage history

- Session 7: 10 / 90 (~11%) — historical denominator, corrected in Session 20
- Session 8: 11 / 90
- Session 9: 12 / 90
- Session 10: 13 / 90
- Session 11: 13 / 90 — unchanged, honestly reported
- Session 12: 13 / 90 — unchanged
- Session 13: 14 / 90 — claimed; Session 14 verified
- Session 14: **14 / 90 (~16%)** — verified
- Session 15: 14 / 90 — honest no-growth
- Session 16: 14 / 90 — stability + retry
- Session 17: 14 / 90 — stale APK fix
- Session 18: 14 / 90 — DualHost pivot
- Sessions 19+20: **14 / 37** — denominator corrected (37, not 90)
- Session 22: **14 / 37** — Wall A closed
- Session 23: **14 / 37** — Wall B closed, milestone 3 verified
- Session 24: **14 / 37** — scoping session
- Session 25: **14 / 37** — Wall C1 named
- Session 26: **14 / 37** — Wall C1 closed, Wall C2 named
- Session 27: **14 / 37** — Wall C2 diagnostic
- Session 28: **14 / 37** — Wall C2 closed, Wall C3 named, milestone 4 to onResume

---

## What Session 29 must do FIRST

Session 29 has two parallel workstreams. **All engine changes are native C++ (`CppParcelRewriter.cpp`).** The test file (`ActivityBootProbeTest.kt`) is Kotlin — allowed per Rule 15 (test-only).

### Workstream A — Wall C3 diagnosis + conditional fix

**Phase A1 — Diagnostic (add iface-token dump for MIUI interfaces to `CppParcelRewriter.cpp`).**

Add a diagnostic block in `ParcelRewriter_TryRewriteCallerIdentity`, after the iface-token read succeeds and before the existing dispatch. Fire only when iface token contains `"ProcessManager"` OR starts with `"miui."`. Cap to first 3 unique (iface, code) pairs to avoid spam. Log: iface ascii, code, first 128 bytes of prelude hex. Return false (passthrough — no rewrite).
MIUI prelude dump: iface='...' code=N aS=M hex=<128 bytes>

Rebuild, install, run probe, capture logcat. Read the dump.

**Phase A2 — Decision tree.**

- **Rewritable-pattern-B (Wall B layout):** prelude after token is `[userId int32] [callingPackage String16]` — reuse Wall B logic, new iface token.
- **Rewritable-pattern-C (Wall C2 layout):** prelude after token is `[leading int32] [uid] [pid] [tagLen] [pkgLen] [pkg chars]` — reuse Wall C2 logic, new iface token.
- **Unrewritable:** prelude after token has no caller-identity field (bare ints, no pkg String16, no AttributionSource). Rewrite mechanism can't help.

**Phase A3 — Conditional fix (only if A2 = rewritable).**

Add third iface case to `ParcelRewriter_TryRewriteCallerIdentity`. New `else if` branch with the pattern-B or pattern-C logic. Rebuild, install, rerun. Verify `FATAL EXCEPTION: Thread-26` gone from logcat.

If A3 succeeds and `FATAL EXCEPTION: Thread-26` is gone: Wall C3 closed.

If A3 fails with a new MIUI-side rejection (different exception, same Thread-26 origin): name the sub-wall. Possibly a second attribution field in a different AIDL interface. Investigate in Session 30.

**Phase A4 — If unrewritable (documentation only).**

Log a distinct line: `Wall C3 UNREWRITABLE: iface='...' has no caller identity in prelude`. Document in session close. Defer `ProcessManagerService` server-side hook to future session (aggressive, cross-process). Proceed to Workstream B. **No Kotlin workaround for the interceptor spawn** (Rule 15 violation). No exception-catcher suppression.

### Workstream B — Wall C4 probe extension (Kotlin test-only)

**Phase B1 — Extend `ActivityBootProbeTest.kt`.**

After `onResume.result=success`, add:

```kotlin
// Wall C4 probe — WindowManager.addView
try {
    val wm = activity.window.windowManager
    val decor = activity.window.decorView
    val lp = decor.layoutParams
    report("wm", wm.javaClass.name)
    report("decorView", decor.javaClass.name)
    report("lp", lp?.javaClass?.name)
    wm.addView(decor, lp)
    report("addView.result", "success")
    report("decorView.isAttachedToWindow", decor.isAttachedToWindow)
    report("decorView.width", decor.width)
    report("decorView.height", decor.height)
} catch (t: Throwable) {
    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
    report("addView.threw", "${cause.javaClass.name}: ${cause.message}")
    report("addView.stack", cause.stackTraceToString().lines().take(25).joinToString(" | "))
}

Phase B2 — Categorize Wall C4.

SecurityException: Calling uid: ... on IWindowSession.addToDisplayAsUser: third instance of Wall B/C2 pattern. Extend rewriter to android.view.IWindowSession. Wall C4 closes with fourth iface case.

BadTokenException / WindowManager.BadTokenException: AMS rejects token because no task record exists. AMS-registration category. Wall C4 = task record wall.

RuntimeException on addView: Surface/SurfaceFlinger category. New mechanism needed (defer, possibly milestone 5 scope).

addView.result=success: Window registered. Wall C5 is AMS task record / launch flags (Session 30).

Phase B3 — No fix in Session 29. Wall C4 named only. Fix deferred to Session 30+.

Order of execution
A1 — diagnostic dump, build, run, capture (30 min)

A2 — read dump, decide rewritability (5 min)

A3 (if rewritable) OR A4 (if not) — conditional change to CppParcelRewriter.cpp (30 min)

B1 — probe extension (parallel with A3, single file change, 15 min)

Build — A3 + B1 together (single rebuild, single install)

Run — probe

A4 verify — FATAL EXCEPTION: Thread-26 gone from logcat

B3 — read addView.threw or addView.result in activity-boot.txt, categorize Wall C4

Two builds max. One commit at session close.

Files touched
Native (A3, if rewritable): app/src/main/cpp/CppParcelRewriter.cpp — add third iface case. Full replacement (Session 28 addendum to Rule 13: full replacement preferred for CppParcelRewriter.cpp since it's the entire rewriter).

Native (A1): app/src/main/cpp/CppParcelRewriter.cpp — add diagnostic dump in the same file. Same full replacement covers A1 + A3.

Test (B1): app/src/androidTest/java/com/ims134/hookengine/ActivityBootProbeTest.kt — add Wall C4 probe block. Full replacement.

No changes: ApplicationProxy.cpp, GuestHost.kt, PmSpoof.kt, DualApplication.kt, ApplicationProxy.kt, GuestClassLoader.kt, BinderTransactHook.cpp (unless iface dispatch needs new extern), PROJECT_STATE.md (until session close).

Mission #1 implications
Wall C3 fix may name nativeInit. If MIUI's ProcessManager.addMiuiApplicationThread check requires server-side uid verification that maps to a nativeInit port (3-param config, (Ljava/lang/String;Ljava/lang/String;I)), coverage grows for the first time in seven sessions.

Wall C4 fix likely extends tnFIN (fourth iface case — IWindowSession). No new port. Integration depth continues.

No new reference method port unless Wall C3 diagnosis names one.

Session 29 must NOT do
No handleJHMethod decomp. Parked (Session 18).

No milestone 5 work. Milestone 4 in progress.

No 60s milestone-3 probe rerun. Closed Session 23.

No Kotlin engine code. All fixes native C++. Test file changes allowed.

No edits to ApplicationProxy.cpp, GuestHost.kt, PmSpoof.kt, DualApplication.kt, ApplicationProxy.kt, GuestClassLoader.kt. Session 29 is CppParcelRewriter.cpp + ActivityBootProbeTest.kt only.

No Looper.prepare() on test thread. Post to main.

No generalizing rewriter to "all interfaces." Third iface case if rewritable; generalization only when a fourth iface demands it.

No root-cause fix on Process.myUid(). Deferred.

No fix on Wall C4 in Session 29. Name only.

No declaring milestone 4 closed. Wall C4, C5, and render pipeline all pending.

No handoff. Withheld until operator demands.

Session 29 deliverable
Either:

Wall C3 CLOSED + Wall C4 NAMED (if A2 shows rewritable), OR

Wall C3 UNREWRITABLE + Wall C4 NAMED (if A2 shows bare transact, documentation only).

Either way, Session 29 closes with a named Wall C4. Commit at close.

Session 18 — DualHost pivot, Viber Application boots — 2026-10-25 (preserved narrative)
Deliverables: GuestHost.kt, GuestClassLoader.kt, GuestLoadTest.kt. Coverage: 14/90 (unchanged). Pivot validated.


---

## Session 29 — Open (elongated)

**Mission #1 status: 14/37 coverage.** Wall C2 closed. Wall C3 named. Milestone 4 at `onResume`. Wall C4 behind it, unreached.

**Session 29 has two parallel workstreams and 2-3 phases within each. Commit at session close when state is verified.**

---

### The strategic picture

Session 28 landed the biggest milestone-4 advance since the project started. `SplashActivity` is now a live, attached Activity instance running its own lifecycle callbacks. It has a `PhoneWindow`, a `ContextThemeWrapper` base, a live `ManifestViberApplication` reference, and it responded correctly to `onCreate`, `onStart`, `onResume`.

But it isn't on screen yet. Two walls stand between here and "Viber UI visible":

**Wall C3** — MIUI's interceptor threads behind the UI creation flow are still rejected by their server-side `Binder.getCallingUid()` check. Non-blocking for the probe, but it means MIUI's crash detection, memory management, and screenshot APIs don't attach to our guest Activity. If Viber's UI relies on any MIUI-specific integration (which on HyperOS it likely does for some features), those features will silently fail later. It's not a wall in front of us — it's a leak in the foundation.

**Wall C4** — the actual physical window. `WindowManager.addView` is what makes an Activity visible. Behind `addView` is `IWindowSession.addToDisplayAsUser`, which goes to `WindowManagerService` on the server side. WMS checks whether the window's token is a valid Activity token, whether the calling uid owns the task, whether the surface can be allocated. First wall here names milestone 4's real remaining cost.

Both walls are relevant to Session 29. C3 is diagnosis + conditional fix. C4 is probe + naming.

**Mission #1 implication:** Wall C3 is the first wall in seven sessions that might name a reference method port. If MIUI's rejection comes from a code path the reference ports as `nativeInit` or a per-signature hook family member, coverage grows for the first time since Session 14. That's not guaranteed, but it's the first honest opportunity. The diagnostic outcome (A2) determines whether Session 29 grows coverage or documents a wall.

---

### Workstream A — Wall C3 diagnostic and conditional fix

**Why this must be diagnosis-first, not fix-first:**

Sessions 25-28 taught a hard lesson: **do not guess parcel layouts**. Wall C1 was a fake fix we caught only because the stack trace told us. Wall C2's first attempt (Session 27) was 4 bytes off and produced a safe no-op because we had a diagnostic dump in place. That dump is why we didn't corrupt the parcel and didn't need a full session rollback.

Wall C3's prelude layout is unknown. It may be:
- Pattern B (Wall B): `[userId int32] [callingPackage String16]` — MIUI's older APIs used this.
- Pattern C (Wall C2): `[leading] [uid] [pid] [tagLen] [pkgLen] [pkg chars]` — newer AttributionSource pattern.
- Something else entirely — MIUI's `ProcessManager` is not AOSP, may use proprietary layouts.
- Bare transact — no caller identity in the parcel at all. The check is purely kernel-side.

**The dump answers this empirically in one build cycle.** No decomp needed, no guessing. That's the same discipline that fixed Wall B and Wall C2.

**Phase A1 — diagnostic instrumentation**

The current `ParcelRewriter_TryRewriteCallerIdentity` reads the prelude and dispatches on iface token. The diagnostic hooks in **between** iface-token read and dispatch:

```cpp
// Diagnostic: dump MIUI ProcessManager transacts that we don't rewrite
// so we can decode the layout empirically. Fires at most 3 unique
// (iface, code) pairs per process to avoid log spam.
static int g_miui_dump_count = 0;

if ((strstr(ifaceAscii, "ProcessManager") != nullptr ||
     strncmp(ifaceAscii, "miui.", 5) == 0) &&
    g_miui_dump_count < 3) {
    char hexbuf[512];
    int hpos = snprintf(hexbuf, sizeof(hexbuf),
                        "MIUI prelude dump: iface='%s' code=%d aS=%zu bytes: ",
                        ifaceAscii, code, g_getPos(parcel));
    size_t dumpLen = (dataSize - g_getPos(parcel) < 128)
                     ? (dataSize - g_getPos(parcel)) : 128;
    for (size_t i = 0; i < dumpLen && hpos < 460; i++) {
        hpos += snprintf(hexbuf + hpos, sizeof(hexbuf) - hpos,
                         "%02x", pData[g_getPos(parcel) + i]);
    }
    LOGD("%s", hexbuf);
    g_miui_dump_count++;
    // Fall through — do NOT rewrite, diagnostic only.
}

The ifaceAscii variable is a small helper: convert the UTF-16 iface token to a UTF-8 ascii buffer for strstr/strncmp. Add it inline in the diagnostic path.

Report the dump to logcat under DualCore tag. Not to activity-boot.txt — the diagnostic is in the native layer, and activity-boot.txt is written by the Kotlin probe. Grep for MIUI prelude dump in logcat.

Phase A2 — decision tree based on dump

Read the dump like a forensic examiner:

text
MIUI prelude dump: iface='miui.process.IProcessManager' code=N aS=M bytes: <128 bytes hex>
Where aS = g_getPos(parcel) after the iface token is consumed. So hex[0..3] is the first int32 after the token.

Branch 1 — Pattern-B (Wall B layout):

text
hex: [u s e r I D  (4 bytes)] [p k g L e n (4 bytes)] [p k g C h a r s UTF-16...]
Interpretation: [userId int32] [String16 length] [String16 content]. userId will be something like 0 or -1. The String16 length will be 14 (for "com.viber.voip"). Fall through to Phase A3 with pattern-B logic.

Branch 2 — Pattern-C (Wall C2 layout):

text
hex: [l e a d i n g (4 bytes)] [u i d (4 bytes)] [p i d (4 bytes)] [t a g L e n (4 bytes)] [p k g L e n (4 bytes)] [p k g C h a r s UTF-16...]
leading = some constant (may be 92 for Bundle header, or a MIUI-specific value). uid = 10000. pid = 10000. tagLen = 0. pkgLen = 14. Fall through to Phase A3 with pattern-C logic.

Branch 3 — Bare transact (no caller identity):

text
hex: [n u m e r i c  v a l u e s] [n o  s t r i n g 1 6  l e n g t h s  w h e r e  1 4  a p p e a r s]
No String16 length of 14 anywhere in the first 128 bytes. The parcel carries only numeric args. The caller identity is coming from Binder.getCallingUid() on the server side, and there's nothing for the rewriter to modify. Unrewritable via transact.

Branch 4 — Unexpected layout:
Some other structure. Document what it looks like and defer Phase A3. The dump gives a starting point for Session 30.

Phase A3 — conditional fix (only if Branch 1 or 2)

Add a third else if branch to ParcelRewriter_TryRewriteCallerIdentity:

cpp
static const char16_t kMiuiProcessManagerIface[] = u"miui.process.IProcessManager";
constexpr size_t kMiuiProcessManagerIfaceLen =
    sizeof(kMiuiProcessManagerIface)/sizeof(char16_t) - 1;

} else if (ifaceLen == kMiuiProcessManagerIfaceLen &&
           memcmp(iface, kMiuiProcessManagerIface, kMiuiProcessManagerIfaceLen * sizeof(char16_t)) == 0) {
    // ── MIUI ProcessManager branch (Session 29, Wall C3) ──
    // Layout TBD from Session 29 Phase A1 diagnostic. Placeholder below
    // shows the pattern-B shape. If diagnostic shows pattern-C, replace
    // with the Wall C2 branch body unchanged (uid + pkg rewrite, +12 delta).
    int32_t userId = 0;
    if (!SafeReadInt32(parcel, &userId)) break;
    size_t cpLen = 0;
    const char16_t* cp = SafeReadString16(parcel, &cpLen);
    if (!cp) break;
    if (cpLen != kOldLen || memcmp(cp, kOld, kOldLen * sizeof(char16_t)) != 0) {
        LOGD("MIUI iface matched callingPackage='%.*s' (code=%d) — not rewriting",
             (int)cpLen, (const char*)cp, code);
        break;
    }
    // ... rewrite body identical to Wall B (rewrite callingPackage at cp offset)
}
The actual pattern depends on A1 output. Phase A3 is executed after Phase A1, using the actual decoded layout.

Phase A4 — verification

Read logcat for FATAL EXCEPTION: Thread-26. If gone: Wall C3 closed. If present with same exception: rewrite didn't reach the correct field, or MIUI's check is server-side kernel-uid, not parcel-field. Investigate.

Read logcat for MIUI rewrite fired: (add a distinct log line so we know the fix reached the parcel). If not present: iface token doesn't match — check the actual token string from A1 dump.

Phase A4 (unrewritable) — documentation

If A2 says Branch 3 or 4:

Log a distinct line: MIUI prelude: UNREWRITABLE — no caller identity in parcel.

Document in Session 29 close as Wall C3 = unrewritable-via-transact.

Defer ProcessManagerService server-side hook to Session 32+ (needs cross-process AIDL hook, aggressive, not blocking milestone 4).

Proceed to Workstream B.

Workstream B — Wall C4 probe extension
Why this must be probe-only, not fix:

Session 25 taught the lesson (Wall C1: probe first, fix after — the fix worked because we knew the exception was Looper). Session 26 showed the reverse (Wall C2 discovery — the fix wasn't applied because we correctly refused to preempt). Session 28 confirmed (Wall C2 fix landed because the exception was clear, not because we pre-guessed).

Wall C4 might be any of:

Another AIDL identity check (IWindowSession.addToDisplayAsUser) — Wall B/C2 pattern, fixable in transact rewriter.

An AMS task record check (BadTokenException) — needs AMS registration, new mechanism.

A Surface allocation failure — needs real Surface, new mechanism.

Something completely different — WMS-specific MIUI/Android 17 check.

The probe extension names it in one build cycle. The fix follows in Session 30.

Phase B1 — the extension

The current probe ends at onResume.result=success. The extension runs immediately after:

kotlin
// Wall C4 probe — WindowManager.addView
// ActivityThread.handleResumeActivity (the real launcher) calls
// wm.addView(decorView, layoutParams) right after onResume. We replicate
// that pattern with the Activity's own WindowManager (not getSystemService,
// which returns a different WM instance).
try {
    val wm = activity.window.windowManager
    val decor = activity.window.decorView
    val lp = decor.layoutParams
    report("wallC4.wm", wm.javaClass.name)
    report("wallC4.decorView", decor.javaClass.name)
    report("wallC4.lp", lp?.javaClass?.name)
    wm.addView(decor, lp)
    report("wallC4.addView.result", "success")
    report("wallC4.decorView.isAttachedToWindow", decor.isAttachedToWindow)
    report("wallC4.decorView.width", decor.width)
    report("wallC4.decorView.height", decor.height)
} catch (t: Throwable) {
    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
    report("wallC4.addView.threw", "${cause.javaClass.name}: ${cause.message}")
    report("wallC4.addView.stack",
        cause.stackTraceToString().lines().take(25).joinToString(" | "))
}
Why activity.window.windowManager not activity.getSystemService(WINDOW_SERVICE):

Activity.getSystemService(WINDOW_SERVICE) returns the app-level WindowManagerImpl (a singleton per Context).

Activity.window.windowManager returns the per-window WindowManagerImpl created by Activity.attach, which is the specific one ActivityThread.handleResumeActivity calls addView on.

The per-window WM is the one that carries the correct token in its mParentWindow.mAppToken and mParentWindow.mToken. If we use the wrong one, the addView will fail with BadTokenException for the wrong reason (we'd be debugging a false wall).

Phase B2 — categorize Wall C4

After the probe runs, read activity-boot.txt. Four possible outcomes:

Outcome 1 — wallC4.addView.threw = java.lang.SecurityException: Calling uid: ... doesn't match source uid: ...

Third instance of Wall B/C2 pattern. IWindowSession.addToDisplayAsUser is the AIDL interface.

Wall C4 = same category as C2. Fix in Session 30: add fourth iface case to ParcelRewriter_TryRewriteCallerIdentity for android.view.IWindowSession. Verified by same technique (log-and-rewrite with dump).

This would be a clean, expected outcome. Multiple AIDL interfaces in the same category is normal — the rewriter is designed for this.

Outcome 2 — wallC4.addView.threw = android.view.WindowManager$BadTokenException: Unable to add window — token android.os.BinderProxy@... is not valid; is your activity running?

AMS doesn't recognize our token. The token came from activity.window.getAttributes().token, which Activity.attach set to some fake value (or null). AMS's WindowManagerService.addWindow checks the token against ActivityTaskManagerService task records, finds none, rejects.

Wall C4 = AMS-registration category. Fix in Session 30 requires ActivityTaskManager.startActivity with a spoofed ComponentName that AMS accepts, or direct ActivityTaskManager.attachApplication with a synthetic ActivityClientRecord. Both are big. Possibly 2-3 weeks.

This is the "real wall" of milestone 4 — the one that turns the milestone from "3-4 sessions" into "3-6 weeks."

Outcome 3 — wallC4.addView.threw = java.lang.RuntimeException: <something about Surface / RenderThread / Choreographer>

Surface allocation failure. IWindowSession.addToDisplayAsUser went through, WMS accepted the window, but SurfaceFlinger couldn't allocate a Surface for the render pipeline.

Wall C4 = render-pipeline category. Fix requires cross-process Surface handoff — this is what BlackBox/VirtualApp call "surface flipping" and it's the hardest part of container-app rendering. Session 30+ scope: possibly 4-6 weeks.

This outcome would be bad news relative to Outcomes 1 or 2. But it would be the honest answer.

Outcome 4 — wallC4.addView.result = success + wallC4.decorView.isAttachedToWindow = true

WMS accepted the window, allocated the Surface, attached the decorView. Viber's SplashActivity is technically visible on screen (though it's likely showing a splash theme background or a blank frame — the real UI requires the app to render on the correct Surface).

Wall C5 (Session 30+) = ActivityTaskManager.startActivity + task record + real render handoff. This is where the milestone goes from "Activity lifecycle proven" to "Viber UI visible" — probably 2-4 weeks.

This outcome is excellent news. It means the render pipeline works and milestone 4 is 60-70% done.

Phase B3 — no fix in Session 29

Whichever outcome, Wall C4 is named and documented. Fix deferred. Session 30 opens on the fix.

Why no fix: because the fix cost varies by 10x across outcomes. Session 29 must commit to a scope before the cost is known. Naming first is correct.

Files touched in Session 29 (full content)
app/src/main/cpp/CppParcelRewriter.cpp — full replacement (A1 diagnostic + A3 conditional, if rewritable)

The full file is the Session 28 version plus:

A small helper for iface-token ascii conversion (ifaceToAscii).

Diagnostic dump block inserted between iface-token read and dispatch.

Third else if branch (if A2 = rewritable), using whichever layout the dump showed.

Since A1 runs before A3, the file is edited twice in Session 29:

First edit (after A1 build/run): add diagnostic block only. Rebuild, run, capture dump. Decide A2.

Second edit (after A2): add third iface case. Rebuild, run, verify A4.

Full replacement for each edit. Session 28's file is the base.

app/src/androidTest/java/com/ims134/hookengine/ActivityBootProbeTest.kt — full replacement (B1 extension)

The Session 28 version plus the Wall C4 probe block above, inserted after onResume.result=success.

Total file changes across Session 29:

2 builds of CppParcelRewriter.cpp (diagnostic + fix, if applicable)

1 build of ActivityBootProbeTest.kt

1 combined build (after A3 + B1 land together) OR 1 test-only build (if A2 = unrewritable)

2-3 am instrument runs

Commit at session close, single commit with both files changed (A3 + B1) or single commit with test file only (A1 diagnostic + B1, A2 = unrewritable).

Order of operations — step by step
Backup current state. git status --short should be clean. git log -1 shows Session 28.

Edit 1 — diagnostic in CppParcelRewriter.cpp. Full replacement with A1 diagnostic block added. No third iface case yet.

Build + install + run.

powershell
./gradlew :app:assembleDebug :app:assembleDebugAndroidTest
adb install -r -t app\build\outputs\apk\debug\app-debug.apk
adb install -r -t app\build\outputs\apk\androidTest\debug\app-debug-androidTest.apk
adb logcat -c
adb shell am instrument -w -r -e class com.ims134.hookengine.ActivityBootProbeTest com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner
adb logcat -d -v time DualCore:D AndroidRuntime:E *:F > logcat-s29-a1.txt
Select-String -Path logcat-s29-a1.txt -Pattern 'MIUI prelude dump|FATAL EXCEPTION|Thread-26' | Out-File logcat-s29-a1-slice.txt
Get-Content logcat-s29-a1-slice.txt
Read the dump. Decide A2 (rewritable / unrewritable).

Edit 2 (if rewritable) — add third iface case. Full replacement with diagnostic retained (useful for future MIUI ifaces) and third iface case added. If unrewritable, diagnostic kept, no fix, documentation only.

Edit 3 — probe extension in ActivityBootProbeTest.kt. Full replacement with Wall C4 probe block.

Build + install + run. Same as step 3, then:

powershell
adb shell run-as com.ims134.hookengine cat files/activity-boot.txt > activity-boot-s29.txt
Get-Content activity-boot-s29.txt
adb logcat -d -v time ActivityBoot:D DualCore:D AndroidRuntime:E *:F > logcat-s29-final.txt
Select-String -Path logcat-s29-final.txt -Pattern 'MIUI rewrite fired|FATAL EXCEPTION|Thread-26|wallC4|addView' | Out-File logcat-s29-final-slice.txt
Get-Content logcat-s29-final-slice.txt
Analyze outcomes.

If Wall C3 was rewritable and MIUI rewrite fired appears: check FATAL EXCEPTION: Thread-26 is gone. If gone: Wall C3 closed.

If Wall C3 was unrewritable: confirm no FATAL EXCEPTION: Thread-26 change. Document as unrewritable.

Wall C4: read wallC4.addView.result or wallC4.addView.threw in activity-boot.txt. Categorize based on Outcome 1-4 above.

Commit at close.

powershell
git add app\src\main\cpp\CppParcelRewriter.cpp
git add app\src\androidTest\java\com\ims134\hookengine\ActivityBootProbeTest.kt
git add app\PROJECT_STATE.md
git commit -m "Session 29: Wall C3 <closed|unrewritable> — <description>. Wall C4 named — <category>. Milestone 4 at <state>. Coverage 14/37 <unchanged|15/37>."
git push origin master
What each outcome means for Session 30
C3 outcome	C4 outcome	Session 30 scope	Effort
Closed (rewrite worked)	Outcome 1 (identity check)	Add fourth iface case (IWindowSession), verify, milestone 4 to addView-success	1-2 sessions
Closed (rewrite worked)	Outcome 2 (BadToken)	AMS registration — big scope, ActivityTaskManager.attachApplication path	3-6 weeks
Closed (rewrite worked)	Outcome 3 (Surface)	Render pipeline — hard, possibly milestone 5 prerequisite	4-6 weeks
Closed (rewrite worked)	Outcome 4 (addView success)	ActivityTaskManager.startActivity + task record + real render	2-4 weeks
Unrewritable	Outcome 1	Add fourth iface case; C3 documented unrewritable, defer	1-2 sessions
Unrewritable	Outcome 2/3/4	Same as row above for C4	Same
Unrewritable + names nativeInit	Any	Port nativeInit (Mission #1 growth); fix C3 natively	2-3 sessions + port
The best case (C3 closed, C4 Outcome 1) is one more 1-2 session wall before addView succeeds. The worst case (C3 closed, C4 Outcome 3) is a 4-6 week render pipeline that may or may not be in milestone 4's scope.

Mission #1 grows only if C3 diagnostic names nativeInit or a per-signature hook family member. That's the first coverage-growth opportunity since Session 14. Session 29's A2 decision determines whether it happens.

Session 29 must NOT do (restated)
No handleJHMethod decomp

No milestone 5 work

No 60s milestone-3 probe rerun

No Kotlin engine code

No edits outside CppParcelRewriter.cpp + ActivityBootProbeTest.kt

No Looper.prepare() on test thread

No rewriter generalization beyond third iface case

No root-cause fix on Process.myUid()

No Wall C4 fix this session

No milestone 4 closed declaration

No handoff

Deliverable
activity-boot-s29.txt — probe output including wallC4.* lines

logcat-s29-a1-slice.txt — A1 diagnostic dump showing MIUI prelude layout

logcat-s29-final-slice.txt — final run showing MIUI rewrite fired (or absence) and FATAL EXCEPTION: Thread-26 state

Three files. One commit. Session 29 closes when all three are reviewed and Wall C3 (closed or unrewritable) + Wall C4 (named) are recorded in PROJECT_STATE.md.

Ready to run Phase A1. Edit CppParcelRewriter.cpp with the diagnostic block, build, run, paste the MIUI prelude dump. That's the first output needed — everything else depends on it.

Coverage stays 14/37 unless A2 names nativeInit. Rule 3 exception stated openly either way.