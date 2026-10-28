
These are genuine physical-device signals. On a bootloader-unlocked device
they would read `'orange'` / `'0'` / `'unlocked'` / `'enforcing'` — a real
detector signal that cannot be spoofed from an app-domain engine because
AMS/system_server reads them from the bootloader at boot time. Not
asserted by `BuildPropTest` (would fail on a legitimately-unlocked device
without the engine being at fault). Documented as physical-device
characteristic, same honest position as §5.12's device-is-real position.

### The 6 assertions in `BuildPropTest` (all Rule-12 falsifiable)

1. `roBuildTagsRelease` — `sysProp("ro.build.tags") == "release-keys"`. Fails if property GOT hook regresses, device is test-keys-signed, or kPropSpoofs entry removed.
2. `buildTagsStaticRelease` — `Build.TAGS == "release-keys"` and does not contain `"test-keys"`. Fails on a test-keys device (honest position).
3. `roSecureIsOne` — `sysProp("ro.secure") == "1"`. Fails on a debuggable build or if kPropSpoofs loses ro.secure.
4. `buildTagsPropertyMatchesStatic` — property surface and static-final surface agree. Fails on a future port to a test-keys device where the property spoof returns "release-keys" while `Build.TAGS` stays "test-keys". Honest-position cross-check.
5. `signatureNulledLegacy` — `getPackageInfo(own, GET_SIGNATURES).signatures` is null/empty. Regression guard for S18 path.
6. `signatureNulledModern` — `getPackageInfo(own, GET_SIGNING_CERTIFICATES).signingInfo.apkContentsSigners` is null/empty. Regression guard for S62 path.

Every assertion is falsifiable — a real regression, device swap, or state
pollution fails one or more.

### Meta-lesson (S62, extends S59/S60/S61 class)

**"A single surface can be defended by multiple independent walls; each
requires its own bypass."** The signature null-out needed four distinct
bypasses in sequence (top-level `api=blocked`, field rename, silent
`declaredFields` filter, multiple Signature[] fields). Each wall was a
different mechanism; getting past one did not get past the next.
**Instrument every step and log the state at each hop, or you will chase
the wrong wall.**

Complements S61: S61 said "the functional goal may already be achieved by
the process's real state." S62 adds: "even when the functional goal IS
achieved by real state, a related surface may still need engineering —
distinguish the goal from the surface."

**Test-isolation note:** `BuildPropTest.@Before` calls
`NativeTestBridge.installFileHooks()` — same pattern as AdbDetectTest,
DebuggerDetectTest, EmulatorDetectTest. Documented in the class header.

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal:
- `booted-viber-order` + no new FATAL + providers ≥14 = canary green
- Any new FATAL, any boot regression, any provider count drop = canary red

**Canary state at Session 62 close (fresh process):** `outcome=booted-viber-order`, `elapsedMs=1691ms`, `providersAttempted=15`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise, unchanged from S61). Verified from a fresh process at S62 close.

**Measurement discipline:** canary must be measured from fresh process — `-e class com.ims134.hookengine.GuestLoadTest` as the only class. In-suite readings (~position 100 of 191) show `providersAttempted=3, providersSucceeded=2` because 100 test classes fragment the process — that is **not** the canary measurement.

---

## Test suite state (as of Session 62 close)

**Executed:** 185 · **Registered:** 191 · **Ignored:** 6 · **Failed:** 0

Registered test classes (post-S62):

| Class | Tests | Status |
|---|---|---|
| NativeItemTest | 5 | ✅ |
| ProcessHooksTest | 4 | ✅ |
| RegressionTest | 5 | ✅ |
| RelocTest | 1 (49 assertions) | ✅ |
| ProxyBridgeSurfaceTest | 4 | ✅ |
| NativeInitTest | 1 | ✅ |
| AbiProbeTest | 2 | ✅ |
| ThreadStatusProbeTest | 1 | ✅ |
| MountsProbeTest | 2 | ✅ |
| AntiDetectionTest | 1 | ✅ |
| AntiDetectionE2ETest | 4 (1 live + 3 ignored) | ✅ |
| PmSpoofTest | 4 | ✅ |
| ClassLoaderBridgeTest | 3 | ✅ |
| DualApplicationTest | 2 | ✅ |
| BinderTransactE2ETest | 9 | ✅ |
| BinderTransactHookTest | 2 | ✅ |
| DynamicAttachTest | 6 | ✅ S42 |
| ChainHookTest | 6 | ✅ S43 |
| ArtMethodTraceTest | 5 | ✅ S44 |
| ConditionalHookTest | 6 | ✅ S45 |
| ReturnModTest | 6 | ✅ S46 |
| SvcHookTest | 6 | ✅ S47 |
| RegInspectTest | 6 | ✅ S48 |
| MemoryOpsTest | 6 | ✅ S49 |
| MapsEnumTest | 6 | ✅ S50 |
| ServiceManagerTest | 8 | ✅ S51 |
| SignalOverrideTest | 6 | ✅ S52 |
| BacktraceTest | 6 | ✅ S53 |
| NativeFnlnTest | 6 | ✅ S54 |
| DexLoadTest | 6 | ✅ S55 |
| DexMergeTest | 6 | ✅ S56 |
| PltResolveTest | 7 | ✅ S57 |
| SyntheticServiceTest | 6 | ✅ S58 |
| AdbDetectTest | 6 (2 ignored) | ⚠️ S59 |
| DebuggerDetectTest | 6 | ✅ S60 |
| EmulatorDetectTest | 6 | ✅ S61 |
| **BuildPropTest** | **6** | **✅ S62** |
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several (incl. DebuggerDetectProbeTest 1, DebuggerDetectProbe2Test 2, EmulatorDetectProbeTest 1, **BuildPropProbeTest 1**) | non-gating |

**Test count history (registered; the count includes `@Ignore`'d test methods):**
- S40: 51 · S41: 51 · S42: 57 · S43: 63 · S44: 68 · S45: 74 · S46: 80 · S47: 86 · S48: 92 · S49: 98
- S50 close: 104 · S51 close: 112 · S52 close: 118 · S53 close: 124
- S54 close: 130 · S55 close: 136 · S56 close: 142 (+6 DexMergeTest)
- S57 close: 149 (+7 PltResolveTest)
- S58 close: 155 (+6 SyntheticServiceTest)
- S59 close: 168 (per full-suite gate)
- S60 close: 177 (+9: DebuggerDetectTest 6, DebuggerDetectProbe2Test 2, DebuggerDetectProbeTest 1)
- S61 close: 183 per handoff / **184 measured in-session** (small drift; handoff undercounted by 1). The 184 figure is the S62 baseline.
- **S62 close: 191** (+7: BuildPropProbeTest 1, BuildPropTest 6). Handoff predicted 189 assuming baseline 183; actual baseline was 184 so the arithmetic lands at 191.

---

## Session 63 first actions (authoritative)

1. Confirm Session 62 commit landed (`git log --oneline -3`).
2. **Fresh-process canary rerun** to confirm S62 no-regression. Expected
   `outcome=booted-viber-order`, `providersAttempted=15`, `providersSucceeded=14`.
3. **Probe the `/proc/net/tcp` surface first.** Before writing any filter:
  - Enumerate `/proc/net/tcp`, `/proc/net/tcp6`, `/proc/net/udp`, `/proc/net/udp6`
  - Count sockets the process itself has open that appear in these tables
  - Check whether the engine's own listener (if any) appears
  - Measure: is loopback listening present in the default view? Which lines belong to us?
  - Probe via both `File("/proc/net/tcp").readLines()` (Java path — `libjavacore` GOT hook possible) and direct `openat` syscall (kernel view — would need `/proc/net/tcp` file rewrite or `getdents64` filtering of `/proc/net/`)
  - Determine if the only reliable interception point is a per-read rewrite of the `/proc/net/tcp` content (via `Hooks.cpp` `Fake_open` extension — new `IsNetTcpPath`) or requires intercepting `getdents64` on `/proc/net/`
4. Write Feature 5.14 (`/proc/net/tcp analysis blocking`) with probe-first discipline.
   Expected mechanism: new `IsNetTcpPath` in `Hooks.cpp` + `BuildFilteredNetTcp()` analogous to `BuildFilteredMaps` / `BuildFilteredStatus`, filtering out engine's own socket lines. `NetTcpTest.kt` with 6 falsifiable assertions.
5. Build + install (`.\build-and-install.ps1`).
6. `am instrument` `NetTcpTest`. Expect `OK (6 tests)`.
7. Full suite. Expect `OK (191 tests)` (registered rises to 197: 191 + 6 = 197; executed 191).
8. Canary from fresh process.
9. Commit, mark 5.14 → ✅.

---

## Rules of engagement (unchanged, still authoritative)

- **Target is the engine feature inventory, not ApplicationProxy coverage.**
- **Viber is canary only. Canary must be measured from fresh process.**
- **Every session advances at least one ⚠️/❌/🔧 → ✅.**
- **Verification-first:** verify any prior-session unverified code before writing new code.
- **Rule 5 (behavior match) — partial disclosure for structural ports.**
- **Rule 12: test assertions must be able to fail.** A test that cannot fail is as broken as one that cannot pass.
- **Rule 14: every test-run instruction includes `./gradlew` + `adb install -r -t` on both APKs.** (Use `.\build-and-install.ps1` to batch these three.)
- **Rule 15: engine is native.**
- **Rule 7 (decomp-first) when wire format / symbol / field layout is uncertain.**
- **No `am instrument` output = no session close.**
- **Deferred items stay deferred:** handleJHMethod (S18), tnFIN code-36 byte rewrite (S12), clone-app features, Settings.Global disassembly (Phase F).
- **Full-file replacements only for code files.** Documentation edits use targeted edits within the existing file.
- **The S59 meta-lesson:** install success ≠ interception success.
- **The S60 meta-lesson:** spec-predicted mechanism ≠ the only mechanism that achieves the goal. Verify *whether*, not *how*.
- **The S61 meta-lesson:** the functional goal may already be achieved by the process's real state. Probe before assuming a mechanism is needed.
- **The S62 meta-lesson:** a single surface can be defended by multiple independent walls; each requires its own bypass. Instrument every step and log the state at each hop, or you will chase the wrong wall.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim. Additions from Sessions 33–62:

- `SYS_getpid` / `SYS_getuid` via `syscall()` bypass both inline and GOT hooks. (S33, verified S34.)
- AttributionSource layout differs by construction path. (S32/33, verified S34.)
- `Process.myUid()` returning spoofed 10000 propagates into `ApplicationInfo.uid`. (S32, verified S34.)
- `mSurface.isValid()` gated on `mAppVisible` gated on host task visibility. (S36/37.)
- MIUI ProcessManager threads are background, non-blocking. (S35+.)
- Full ARM64 relocation handles 9 kinds. (S41.)
- `AttachRuntime` adds alignment/mapped/exec guards. (S42.)
- ChainHook stub page layout (32-byte slots). (S43.)
- `visibility("hidden")` on naked-asm-called functions required for `adrp+add`. (S43, S46, S48.)
- Feature 1.8 — per-slot predicates, `nullptr` = unconditional fire. (S45.)
- Feature 1.9 — post-call modifier with shadow trampoline. (S46.)
- Feature 1.6 — BRK encoding is `0xD4200000 | (imm16 << 5)`, NOT `0xD4200000 | imm16`. (S47.)
- Feature 1.6 — SVC mechanism (verified on-device). (S47.)
- Feature 2.7 — Register state inspection mechanism (verified on-device S48). 784-byte frame, per-slot naked stub, dispatcher memcpy into `g_snapshots[slot]`. MRS NZCV returns flags in bits 31:28, not 3:0 (ARM ARM DDI 0487 C5.2.53). x17 clobbered by `InstallInlineHookEx` relay. x16 accurate.
- Feature 2.1 / 2.2 / 2.3 — MemoryOps mechanism (verified on-device S49). MemRead / MemWrite / MemScan validate against `/proc/self/maps`; MemWrite mprotects each page to RW, writes, restores orig (Rule 10 preserved on every path). `MEMOPS_MAX_PAGES = 8`. `__builtin___clear_cache` on data pages is a no-op.
- Feature 2.5 — Maps enumeration mechanism (verified on-device S50).
  - `MemEnumMaps` / `MemEnumMapsFilter` / `MemFindRegionContaining` with `MemMapEntry { uintptr_t start; uintptr_t end; int prot; uint64_t offset; uint32_t dev_major; uint32_t dev_minor; uint64_t inode; char path[256]; }`.
  - `MAPS_MAX_ENTRIES = 8192` — instrumentation process genuinely has >1024 entries.
  - `sscanf` on Bionic drops the trailing pathname even with valid `%255[^\n]` or `%n`. `ParseMapsLine` uses manual `strtoul` walker.
  - `fgets` line buffer must be 2048+.
- Feature 3.4 — ServiceManager lookup interception (verified on-device S51). See §Cache-hit wall above for full mechanism table.
- Feature 3.6 — Signal handling override (verified on-device S52).
  - `sigaction`-based per-signal slot install. `so_dispatch` runs in signal-safe context.
  - Two actions: `SIGOVR_PASS_THROUGH` chains to previous handler; `SIGOVR_SUPPRESS` swallows without chaining.
  - **SIGUSR1 and SIGUSR2 are reserved by ART.** The `Signal Catcher` thread does `sigwait()` on `{SIGQUIT, SIGUSR1, SIGUSR2}`. A `sigaction` handler on these signals is installed successfully but **never invoked**. Use `SIGRTMIN + N` for user-triggered signal tests.
  - **`_NSIG` is a compile-time constant (65 on bionic), `SIGRTMAX` is a function call.** VLA at file scope from `SIGRTMAX` will not compile.
  - **Do not use `std::atomic<T>` with `__atomic_*_n` primitives.** Use plain `int` / `uint64_t` accessed via `__atomic_*_n`.
  - **Uninstall must reset `fire_count`** for re-entrant cleanliness.
- Feature 2.8 — Backtrace from hook site (verified on-device S53).
  - FP-chain walker: `[fp+0]` → next fp, `[fp+8]` → saved LR. ARM64 ABI: 16-byte-aligned fps, descending stack.
  - Guards: PROT_EXEC check for each pc, 16-byte alignment for each fp, `next_fp > cur_fp` monotonicity for cycle detection, bounded at `BT_MAX_FRAMES=32`.
  - `__builtin_frame_address(0)` in `BacktraceCaptureHere` forces frame establishment.
  - `BacktraceFilterOutLib(frames, n, "libhookengine.so")` strips engine frames.
  - Integration with Feature 2.7: at hook site, `x29` / `x30` from `RegInspect` feed directly into `BacktraceCaptureFromContext(x29, x30)`.
- Feature 4.2 — Native ClassLoader chaining (verified on-device S54).
  - `nativeFNLN(guest, host)` in `NativeFnln.cpp` orchestrates the merge in C++: read `guest.pathList.dexElements`, read `host.pathList.dexElements`, dedupe host elements by identity (`IsSameObject`), prepend host-only to guest, write back via `Field.set`.
  - Hidden-api reflection still goes through `HiddenApiBridge` — the merge logic owns the algorithm, the bypass primitive is the same.
  - `habFindFieldInHierarchy` walks the superclass chain: `PathClassLoader` has no `pathList`, but `BaseDexClassLoader` does. The `NoSuchFieldException` for `PathClassLoader.pathList` is expected and handled.
  - `IsSameObject` must be used for identity comparison across local/global refs — raw `==` is not spec-safe.
  - Return codes: 0 success, -1 invalid args, -2 pathList not found, -3 dexElements not found, -4 field get/set methodIDs not found, -5 pathList read failed, -6 dexElements read failed, -7 element class lookup failed, -8 merged array allocation failed, -9 Field.set threw.
- Feature 4.3 — Dynamic DEX loading (verified on-device S55).
  - `DexLoaderCreate(env, dexPath, optimizedDir, parent)` in `DexLoader.cpp` constructs `dalvik/system/DexClassLoader` entirely in C++.
  - Standard ctor signature: `(String dexPath, String optimizedDirectory, String librarySearchPath, ClassLoader parent)`. Pass `nullptr` for `librarySearchPath` when the DEX has no native libs.
  - **ART initiator-binding quirk:** When a class is loaded via a `DexClassLoader`, ART binds `(dex_file, class_def)` to that loader's defining classloader. If a *different* classloader later asks for the same class **through the same dex** (via a chained `Element`), ART refuses with `ClassNotFoundException`.
    **Rule for chained class loading:** chain the loader into the target, then let the target be the first to load the class. Do **not** pre-load via the source loader before chaining.
  - **Common wall:** `DexClassLoader` construction fails with "Attempt to register dex file ... with multiple class loaders" when the dex path is already loaded by another loader whose elements aren't shared with the new loader's parent chain.
  - `optimizedDir` must be a writable directory (typically `context.codeCacheDir` + a unique subdirectory per loader).
- Feature 4.4 — DEX merging at runtime (verified on-device S56).
  - Multi-path `DexClassLoader("pathA:pathB", optDir, null, parent)` accepts `:`-separated path list natively. No new native mechanism — `DexLoaderCreate` from Feature 4.3 handles it. The multi-path form is standard on Linux/Android.
  - The resulting loader's `dexElements` array contains **one element per source path**, in order. Merged loader chains into a guest via 4.2 making classes from all sources resolvable.
  - **ClassLoaderContext classpath size mismatch** message from ART on multi-path construction is informational, not an error. Present in both S55 and S56 logs when `DexClassLoader` is created with a parent whose classpath doesn't match the boot classpath — harmless, doesn't affect class resolution.
  - Same ART initiator-binding rule as 4.3 applies.
  - All 6 `DexMergeTest` assertions in `DexMergeTest.kt` are falsifiable: `dexMergedCreate`, `dexMergedResolvesFromSourceA`, `dexMergedResolvesFromSourceB`, `dexMergedElementCount`, `dexMergedChainWorks`, `dexMergedIndependentInstances`.
- Feature 7.6 — PLT stub resolution (verified on-device S57).
  - `PltResolveForSymbol(lib_substr, symbol)` in `PltResolver.cpp`:
    - Locates the loaded lib via `dl_iterate_phdr`.
    - `mmap`s the on-disk ELF (`.so` from `/proc/maps` path).
    - Walks `.dynsym`, `.dynstr`, `.rela.plt`, and `.plt` or `.plt.sec`.
    - For each `R_AARCH64_JUMP_SLOT` (type 1026), correlates symbol → `.rela.plt` index → PLT entry.
    - **Prefer `.plt.sec` when present** (BTI-enabled layout on API 37+). Each 16-byte `.plt.sec` entry = one `.rela.plt` slot, 0-based index into `.plt.sec`.
    - **Fallback to `.plt`** (no `.plt.sec`) — try base offsets 32 and 48 (linker convention varies); validate by reading the first instruction word — must be `adrp` (mask `0x9F000000 == 0x90000000`) or `bti c` (`0xD503245F`).
  - Returned address is the first byte of the PLT stub. Calling it as `open()` is the definitive test — `PltResolveTest.pltEntryCallableForOpen` does exactly that and requires a valid fd return.
- Feature 3.7 — Native ServiceManager addition (verified on-device S58).
  - **Primary mechanism:** JNI-impl inline hook on `android_os_Parcel_nativeReadStrongBinder` in `libandroid_runtime.so`.
  - **The symbol is NOT exported** — dynsym walk fails. Must scan the .so's mapped memory for the string `"nativeReadStrongBinder\0"` and then find the 8-byte value equal to that string's address (that's the `JNINativeMethod.name` field). fnPtr is at `+16` from there.
  - **Composition with 3.4:** alias rewrite (`name -> "activity"`) is essential — it forces the driver to return a valid `BinderProxy`.
  - **ART fast-JNI wall extends to `ServiceManagerProxy.getService2` (0x3d) and `checkService2` (0x3f).** Same class of wall as S51's outer SM methods.
  - **ArtMethod hook on `Parcel.readStrongBinder()` Java wrapper fails:** caller's JIT code expects the method's original `data_` = quick stub, not a JNI-ABI function. Do not replace `data_` on a Java method's ArtMethod — JIT-ABI managed return (compressed ref in w0) will be truncated to 32 bits and SIGSEGV.
  - **Only the JNI-impl function** (found via `JNINativeMethod` struct scan) is safe to hook — ART's intact JNI trampoline handles compressed-ref conversion on return.
  - **Same-process only.** Cross-process callers still see a `BinderProxy` — the driver has no handle for our synthetic name.
- Feature 5.11 — Debugger detect, property-gated JDWP (verified on-device S60). See §5.11 above.
- Feature 5.12 — Emulator detect blocking (verified on-device S61). See §5.12 above.
- Feature 5.13 — Test-keys detect blocking (verified on-device S62). See §5.13 above. Additional facts:
  - **`Build.TAGS` cannot be written via reflection.** `public static final String` with constant initializer is compile-time constant-folded; `Field.set` throws `IllegalAccessException` even after `setAccessible(true)` and even via `HiddenApiBridge` bypass. This is a JVM language invariant, not a device wall — no Phase F work is deferred. `BuildSpoof.kt` deliberately not written.
  - **`SigningInfo.mSigningDetails` is `api=blocked` on API 37.** Direct `Class.forName("android.content.pm.SigningInfo").getDeclaredField("mSigningDetails")` from the caller thread is DENIED (`using reflection: denied`). Route through `HiddenApiBridge.reflectGetDeclaredField`.
  - **`Class.getDeclaredFields()` on `api=blocked` classes silently returns an empty array from the caller thread.** It does not throw. This is a silent filter — the field exists, it just isn't visible. Route enumeration through `HiddenApiBridge.reflectGetDeclaredFieldsArray`. This is the "0 fields scanned" symptom in probe #3.
  - **MIUI/API 37 renamed AOSP's `SigningDetails.signatures` to `mSignatures`** and added a second `Signature[]` field `mPastSigningCertificates`. Both must be nulled. Field inventory logged by probe #4:
    `[CREATOR:Creator, PAST_CERT_EXISTS:int, TAG:String, UNKNOWN:SigningDetails, mPastSigningCertificates:Signature[], mPublicKeys:ArraySet, mSignatureSchemeMinorVersion:int, mSignatureSchemeVersion:int, mSignatures:Signature[]]`.
  - **Field-name-agnostic null-out is the robust approach.** Enumerate every non-static `Signature[]` field on `SigningDetails` and set to empty array. Survives future MIUI renames. Log the inventory of discovered fields so a future restructure that adds a new way to reach the signature array (method instead of field, etc.) surfaces immediately.
  - **Detection-goal vs signature-surface distinction.** Feature 5.13's functional goal (a test-keys detector sees only release-keys values) is achieved by the property surface + natural `Build.TAGS` on this device. The signature null-out is orthognal — defense-in-depth for a future port to a test-keys-signing device where the sig could corroborate test-keys state. `BuildPropTest.signatureNulledLegacy` and `signatureNulledModern` are regression guards for PmSpoof, not 5.13 gates.

---

## The blunt version

**Four categories complete:** Category 1 (Hook mechanisms, 9/9), Category 2 (Memory primitives, 7/7), Category 3 (IPC / kernel boundary, 7/7), Category 4 (Class loading, 6/6). Sessions 41–62 shipped twenty-four verified primitives, all on-device verified with fail-able tests:

- 41: full ARM64 relocation (9 kinds)
- 42: dynamic attach
- 43: hook chaining
- 44: ArtMethod trace
- 45: conditional hook
- 46: return-value modification
- 47: SVC instruction hooking
- 48: register state inspection
- 49: memory read / write / scan
- 50: full maps enumeration
- 51: ServiceManager lookup interception (transact-level)
- 52: signal handling override
- 53: backtrace from hook site
- 54: native ClassLoader chaining (Rule 15 debt closure)
- 55: dynamic DEX loading
- 56: DEX merging at runtime
- 57: PLT stub resolution
- 58: Native ServiceManager addition (JNI-impl hook on `nativeReadStrongBinder`)
- 59: ADB detect blocking (⚠️ PARTIAL — property surface ✅, Settings walled)
- 60: debugger detect blocking (✅ — property-gated JDWP)
- 61: emulator detect blocking (✅ — real-device cleanliness + defensive kPropSpoofs)
- 62: test-keys detect blocking (✅ — property surface + natural clean state + signature null-out guard)

**191 registered tests, 0 failed at S62 close.** Session 62 took four probe iterations (one compile error, one `api=blocked` leak, one field rename, one silent `declaredFields` filter — four distinct walls in sequence), then one new test class. The PmSpoof signature null-out now clears both the legacy `PackageInfo.signatures` and the modern `SigningInfo.mSigningDetails.mSignatures` / `mPastSigningCertificates` via a name-agnostic enumeration over `SigningDetails.declaredFields` obtained through the HiddenApiBridge bypass. `Build.TAGS` reflection-write is refused by the JVM (public static final + constant init) and doesn't need to be spoofed on this device — the natural value is `release-keys`. S62's meta-lesson: "a single surface can be defended by multiple independent walls; each requires its own bypass. Instrument every step or you will chase the wrong wall."

**Phase D closed.** Phase E (anti-detection completion, S59–S64) is four sessions in; Session 63 opens Feature 5.14 (`/proc/net/tcp` blocking). The remaining Phase E features (5.14, 5.15, 5.16) will follow the same probe-first discipline: measure what's actually leaking on this device before writing any mechanism. Phase F (polish, S65–S75) is where the §5.10 Settings.Global disassembly and §5.11 Debug/VMDebug fast-JNI disassembly land. Session 75 is the target.

**If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.**