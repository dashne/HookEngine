
Three layers close the ptrace-attachment surface:
1. **`/proc/self/syscall`** — readable but reveals only the reading
   thread's *current* syscall. With no ptracer attached (5.9 ptrace-block,
   5.11 property-gated JDWP), the syscall number is `epoll_pwait` or
   another Looper syscall. Indistinguishable from any ordinary Android
   process. Not a leak.
2. **`/proc/self/stack`** — read-denied at kernel level via
   `ptrace_may_access` capability check. `File.readText()` fails with
   EACCES; `Os.open()` succeeds but `Os.read()` fails with EACCES
   (errno=13). Even if kernel loosened, no ptracer means the trace would
   show only kernel symbols — no user code state.
3. **`/proc/self/status`** — `TracerPid: 0` forced by S23
   `BuildFilteredStatus`. No ptrace-attachment signal via status.

**No Hooks.cpp changes. No IsSyscallPath / IsStackPath. No
BuildFilteredSyscall / BuildFilteredStack.**

### The 3 assertions (from `CmdlineStackTest`)

4. `syscallFileWellFormed` — `/proc/self/syscall` readable, 9-field standard format, syscall nr >= 0, sp/pc non-zero. Fails if kernel format changes OR file becomes unreadable.
5. `stackReadDeniedAtKernel` — `/proc/self/stack` read-denied at BOTH the File path (via `File.readText()`) AND the direct Os path (`Os.open()` + `Os.read()`). Both must return EACCES. Fails if a future ROM loosens the `ptrace_may_access` capability check.
6. `tracerPidForcedZero` — `/proc/self/status` `TracerPid: 0`. Fails if S23 `BuildFilteredStatus` regresses OR a debugger is attached.

### Meta-lesson (S64)

**"The functional goal may be achieved by kernel capability check +
absence of state + framework filter, in combination."** The ptrace-attachment
signal is closed by:
- Kernel capability check (stack read denied)
- Absence of state (no ptracer → syscall shows nothing anomalous)
- Framework filter (status TracerPid forced to 0)

Three layers, each from a different tier (kernel / process / Java), each
doing part of the work.

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal:
- `booted-viber-order` + no new FATAL + providers ≥14 = canary green
- Any new FATAL, any boot regression, any provider count drop = canary red

**Canary state at Session 64 close (fresh process):** `outcome=booted-viber-order`, `elapsedMs=1696ms` (S63/S64 identical — zero engine changes between), `providersAttempted=15`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise, unchanged from S61–S63). Verified from a fresh process at S64 close.

**Measurement discipline:** canary must be measured from fresh process — `-e class com.ims134.hookengine.GuestLoadTest` as the only class. In-suite readings (~position 100 of 205) show `providersAttempted=3, providersSucceeded=2` because 100 test classes fragment the process — that is **not** the canary measurement.

---

## Test suite state (as of Session 64 close)

**Executed:** 199 · **Registered:** 205 · **Ignored:** 6 · **Failed:** 0

Registered test classes (post-S64):

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
| BuildPropTest | 6 | ✅ S62 |
| NetTcpTest | 6 | ✅ S63 |
| **CmdlineStackTest** | **6** | **✅ S64** |
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several (incl. DebuggerDetectProbeTest 1, DebuggerDetectProbe2Test 2, EmulatorDetectProbeTest 1, BuildPropProbeTest 1, NetTcpProbeTest 1, **CmdlineStackProbeTest 1**) | non-gating |

**Test count history (registered; the count includes `@Ignore`'d test methods):**
- S40: 51 · S41: 51 · S42: 57 · S43: 63 · S44: 68 · S45: 74 · S46: 80 · S47: 86 · S48: 92 · S49: 98
- S50 close: 104 · S51 close: 112 · S52 close: 118 · S53 close: 124
- S54 close: 130 · S55 close: 136 · S56 close: 142
- S57 close: 149
- S58 close: 155
- S59 close: 168
- S60 close: 177
- S61 close: 183 per handoff / 184 measured in-session
- S62 close: 191
- S63 close: 198
- **S64 close: 205** (+7: CmdlineStackProbeTest 1, CmdlineStackTest 6)

---

## Session 65 first actions (authoritative)

Phase E is CLOSED. Session 65 opens Phase F with Feature 6.8 (signal-safe uninstall).

1. Confirm Session 64 commit landed (`git log --oneline -3`).
2. **Fresh-process canary rerun** to confirm S64 no-regression. Expected
   `outcome=booted-viber-order`, `providersAttempted=15`, `providersSucceeded=14`.
3. **Probe the signal-safe uninstall surface first.** Before writing any code:
  - Read `InlineHooker.cpp` `UninstallInlineHook` — current implementation, known non-signal-safe paths (mutex acquisition, `munmap`, `nanosleep` deferred-free).
  - Read `HookRegistry.h` / `HookRegistry.cpp` — registry locking, `RegistryFree` under mutex.
  - Probe: from a SIGRTMIN+N handler, attempt to call `UninstallInlineHook` on a known target. Does it deadlock, crash, or complete?
  - Probe: what path does `UninstallAllHooks` take when called from signal context?
  - Determine minimum required changes to make the uninstall path async-signal-safe.
  - Expected mechanism candidates:
    - **Deferred uninstall:** signal handler enqueues an "uninstall request" via lock-free SPSC queue; a dedicated thread (spawned at engine init) drains it and performs the actual uninstall. Signal handler returns immediately. This is the classic Frida / Android runtime pattern.
    - **Lock-free registry read:** replace `g_reg_mutex` with an RCU-like scheme so registry reads (needed to find the handle) are lock-free from signal context. The write side stays mutex-protected.
    - **Atomic state transitions:** `HS_INSTALLED` → `HS_REMOVING` via `__atomic_exchange_n` so concurrent install and signal-context uninstall cannot both claim the same slot.
  - Likely **real native code** this session — signal-safe uninstall requires reworking `InlineHooker.cpp` and `HookRegistry.cpp`. This is a genuine C++ deliverable.
4. Write `SignalSafeUninstallTest.kt` (or `SignalSafeUninstallProbeTest.kt` first if the mechanism isn't obvious from code inspection).
5. Build + install (`.\build-and-install.ps1`).
6. `am instrument` gate 1.
7. Full suite gate.
8. Canary gate.
9. Commit, mark 6.8 → ✅.

---

## Rules of engagement (unchanged, still authoritative)

- **Target is the engine feature inventory, not ApplicationProxy coverage.**
- **Viber is canary only. Canary must be measured from fresh process.**
- **Every session advances at least one ⚠️/❌/🔧 → ✅.**
- **Verification-first:** verify any prior-session unverified code before writing new code.
- **Rule 5 (behavior match) — partial disclosure for structural ports.**
- **Rule 12: test assertions must be able to fail.** A test that cannot fail is as broken as one that cannot pass. Corollary: **writing code that can never be reached to filter a file that can never be read is a Rule 12 violation.** Install success ≠ fire success.
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
- **The S63 meta-lesson:** the functional goal may already be achieved by OS-level MAC policy — one layer below the process, one layer below device identity and natural data. Probe the kernel-policy layer before writing memfd filters.
- **The S64 meta-lesson:** the functional goal may be achieved at multiple independent layers of the stack: framework identity spoof, status filter, kernel capability check, absence of state, OS MAC policy, hardware identity, natural data state. Probe the deepest layer first, then work up.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim. Additions from Sessions 33–64:

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
  - `habFindFieldInHierarchy` walks the superclass chain: `PathClassLoader` has no `pathList`, but `BaseDexClassLoader` does.
  - `IsSameObject` must be used for identity comparison across local/global refs.
  - Return codes: 0 success, -1 invalid args, -2 pathList not found, -3 dexElements not found, -4 field get/set methodIDs not found, -5 pathList read failed, -6 dexElements read failed, -7 element class lookup failed, -8 merged array allocation failed, -9 Field.set threw.
- Feature 4.3 — Dynamic DEX loading (verified on-device S55).
  - `DexLoaderCreate(env, dexPath, optimizedDir, parent)` in `DexLoader.cpp` constructs `dalvik/system/DexClassLoader` entirely in C++.
  - **ART initiator-binding quirk:** chain the loader into the target, then let the target be the first to load the class. Do NOT pre-load via the source loader before chaining.
  - **Common wall:** `DexClassLoader` construction fails with "Attempt to register dex file ... with multiple class loaders" when the dex path is already loaded by another loader whose elements aren't shared with the new loader's parent chain.
- Feature 4.4 — DEX merging at runtime (verified on-device S56).
  - Multi-path `DexClassLoader("pathA:pathB", optDir, null, parent)` accepts `:`-separated path list natively.
  - **ClassLoaderContext classpath size mismatch** message from ART is informational, not an error.
- Feature 7.6 — PLT stub resolution (verified on-device S57).
  - `PltResolveForSymbol(lib_substr, symbol)` in `PltResolver.cpp`: mmaps on-disk ELF, walks `.dynsym`, `.dynstr`, `.rela.plt`, `.plt` or `.plt.sec`.
  - **Prefer `.plt.sec` when present** (BTI-enabled layout on API 37+). Each 16-byte entry = one `.rela.plt` slot, 0-based.
  - **Fallback to `.plt`** — try base offsets 32 and 48; validate first instruction word (`adrp` or `bti c`).
- Feature 3.7 — Native ServiceManager addition (verified on-device S58).
  - **Primary mechanism:** JNI-impl inline hook on `android_os_Parcel_nativeReadStrongBinder` in `libandroid_runtime.so`.
  - **The symbol is NOT exported** — scan .so's mapped memory for the string, find JNINativeMethod.name at that pointer, fnPtr at `+16`.
  - **Composition with 3.4:** alias rewrite essential. Reply-Parcel mutation does not work on MIUI/API 37.
  - **Same-process only.**
- Feature 5.11 — Debugger detect, property-gated JDWP (verified on-device S60). See §5.11 above.
- Feature 5.12 — Emulator detect blocking (verified on-device S61). See §5.12 above.
- Feature 5.13 — Test-keys detect blocking (verified on-device S62). See §5.13 above.
- Feature 5.14 — /proc/net/tcp blocking via SELinux MAC policy (verified on-device S63). See §5.14 above. Additional facts:
  - `/proc/net/tcp`, `/proc/net/tcp6`, `/proc/net/udp`, `/proc/net/udp6` are classified `proc_net_tcp_udp`. `/proc/net/unix` is `proc_net`. Both DENY `{ read }` to `untrusted_app` with `permissive=0`.
  - `Os.open()` returns the same `EACCES` as `File.readText()`. Denial at kernel level, not JVM.
  - `NativeTestBridge.readFileRaw` (raw `openat`) returns 0 bytes.
  - **S42 loopback-bind wall regressed** on current ROM build. `ServerSocket().bind()` now succeeds. Phase F candidate.
- Feature 5.15 — /proc/self/cmdline full spoof (verified on-device S64). See §5.15 above. Additional facts:
  - **S19 `setArgV0` produces exactly 1 argv entry.** Kernel/Android NUL-pads the original buffer size. No residual args.
  - **`/proc/self/environ` is 19 standard Android env vars.** No engine identity.
- Feature 5.16 — /proc/self/syscall + stack filter (verified on-device S64). See §5.16 above. Additional facts:
  - **`/proc/self/syscall` format: 9 fields, whitespace-separated.** `<nr> <arg0> <arg1> <arg2> <arg3> <arg4> <arg5> <sp> <pc>`. `sp` and `pc` are hex-prefixed.
  - **`/proc/self/stack` read-denied by kernel `ptrace_may_access` capability check.** `File.readText()` throws `IOException: read failed: EACCES`. `Os.open()` succeeds (file exists, read permission check passes on open), but `Os.read(fd, ...)` throws `ErrnoException EACCES (errno=13)`. This is a **kernel capability check**, not SELinux — no AVC line in logcat.
  - **`/proc/self/status` TracerPid: 0** via S23 `BuildFilteredStatus`.
  - **Path aliasing:** `/proc/self/<file>` works, `/proc/<pid>/<file>` works only for the caller's own pid. `/proc/<uid>/<file>` returns ENOENT — don't confuse pid with uid in path construction.

---

## The blunt version

**Four categories complete:** Category 1 (Hook mechanisms, 9/9), Category 2 (Memory primitives, 7/7), Category 3 (IPC / kernel boundary, 7/7), Category 4 (Class loading, 6/6). **Phase E CLOSED** — 5.11 through 5.16 all ✅, only 5.10 remains ⚠️ PARTIAL (Settings.Global walled). Sessions 41–64 shipped twenty-seven verified primitives, all on-device verified with fail-able tests:

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
- 63: /proc/net/tcp blocking (✅ — OS-level SELinux MAC policy)
- 64: /proc/self/cmdline + syscall/stack filter (✅ — S19 setArgV0 + kernel ptrace_may_access + S23 TracerPid)

**205 registered tests, 0 failed at S64 close.** Session 64 closes the last two Phase E features without native code because the OS already achieves both goals. The probe established:

- **5.15 (cmdline):** S19 `setArgV0` produces a single, clean argv entry. `environ` has no engine identity. Nothing to filter.
- **5.16 (syscall + stack):** `/proc/self/syscall` readable but reveals only Looper state (no ptracer → no leak). `/proc/self/stack` read-denied at kernel level via `ptrace_may_access`. `TracerPid: 0` via S23 filter.

**This is not avoiding native code.** Phase A–D shipped 18 native primitives. S59 and S62 both added real work (property surface + `PmSpoof.nullOutSignatures` 4-wall bypass). S60, S61, S63, S64 all measured that the goal was already achieved at a layer below the engine. **Writing code that can never fire is the Rule 12 violation, not skipping it.**

**The S64 meta-lesson extends the family into its deepest form yet:**

1. **S61:** hardware identity — the device is real.
2. **S62:** data content — Build.TAGS, signature naturally clean.
3. **S63:** kernel MAC policy — SELinux denies app-domain reads of /proc/net/*.
4. **S64:** multiple independent layers in combination — framework identity spoof (setArgV0) + status filter (TracerPid) + kernel capability check (ptrace_may_access) + absence of state (no ptracer).

Each layer is a different kind of "the goal is already achieved without engine code." **The engine's Phase E work is to measure which layer defends each surface and write falsifiable regression guards at that layer.** When a wall breaks, the guard goes red and the next session writes the native code that the wall now requires.

**Phase F opens at Session 65.** Remaining native work: 6.8 (signal-safe uninstall — genuine C++ rework of `InlineHooker` / `HookRegistry`), 8.7 (per-hook profiling — native CPU/memory accounting per handler), 7.1 (unified ELF API — consolidate the 5 scattered dynsym/symtab/hash walkers), 8.2 (per-hook argument logging — extend ChainHook dispatch). And the disassembly work: S51 cache-hit, S60 Debug/VMDebug fast-JNI, §5.10 Settings.Global shared-mem path, S63 loopback-bind wall re-measurement. Session 75 is the target.

**If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.**