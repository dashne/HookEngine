
Three layers:
1. **syscall** — readable but reveals only Looper state; no ptracer
2. **stack** — kernel ptrace_may_access denies read
3. **status** — TracerPid forced to 0 by S23

**No Hooks.cpp changes.**

### The 3 assertions (from `CmdlineStackTest`)

4. `syscallFileWellFormed` — 9 fields, sp/pc non-zero
5. `stackReadDeniedAtKernel` — EACCES on both File and Os.read paths
6. `tracerPidForcedZero` — TracerPid: 0

**Meta-lesson (S64):** "the functional goal may be achieved at multiple
independent layers of the stack."

---

## §6.8 Signal-safe uninstall — deferred SPSC queue + worker thread (Session 65)

**Functional goal:** uninstall is safe from any signal handler context.

**The probe (S65) audited `UninstallInlineHook` and confirmed three
async-signal-safety violations in the current path:**

1. `std::lock_guard<mutex>(g_hook_mutex)` — pthread_mutex_lock, not on the
   POSIX async-signal-safe list. Self-deadlock if signal fires on the
   thread holding the lock.
2. `FindHandle` / `RegistryFree` — second mutex (`g_reg_mutex`).
3. `nanosleep(100ms)` — not sig-safe, blocks the handler for the full
   deferred-free grace period.

**Deadlock classes:**
- **Class A (self):** signal fires on thread holding `g_hook_mutex`,
  handler tries to acquire → deadlock.
- **Class B (cross):** signal fires on thread A while thread B holds
  `g_hook_mutex` → A blocks 100ms+, transitive AB-BA possible if B itself
  is interrupted.

**Mechanism — deferred uninstall via lock-free SPSC ring buffer + worker
thread.** The classic Frida / Cydia Substrate / Xposed pattern.

- **Signal handler** (`fixture_deferred_uninstall` in `SignalOverrideJni.cpp`):
  calls `SignalSafeUninstallFromHandler(target)` → spinlock CAS + memcpy +
  `write(eventfd)` → returns in ~100us.
- **Worker thread** (spawned by `SignalSafeUninstallArm`):
  `poll(eventfd)` → drain queue → call `UninstallInlineHook(target)` from
  normal thread context. All existing mutex/nanosleep/munmap calls are
  safe here.
- **Zero changes to `UninstallInlineHook`** — it stays normal-context.

**Signal-handler stack usage: < 256 bytes** (MINSIGSTKSZ is 8192 = 32×).
**Signal-handler runtime: ~100us** (S65 log: `raise returned in 97us`).

**Three test failures across three subsystems, three fixes:**

| # | Failure | Subsystem | Fix |
|---|---|---|---|
| 1 | `deferredUninstallFromSignalHandler` — `completed expected 1 but was 0` | Observation timing | `SignalSafeUninstallWaitFor` now waits for **both** `FindHandle(target)==null` **AND** `(done+fail) >= enq`. The old condition returned during the worker's 100ms deferred-free grace period, before `g_sig_completed` incremented. |
| 2 | `statsMatchQueueOperations` / `multipleSignalsSequential` — `attach t2 returned 6` (HOOK_ALLOC_FAILED) | Memory allocator | `AllocateMemoryNear` rewritten: page-by-page outward scan bounded by ±120MB. The old fixed-offset list (1024 pages + 119 MB-strides) couldn't find free pages when two targets were 3KB apart — t1's relay+tramp consumed t2's candidate pages. Fast case 1-10 iterations; worst case ~30K. |
| 3 | `armDisarmRoundTrip` / others — stats accumulated across Arm/Disarm cycles | Stats reset | `SignalSafeUninstallArm` resets enq/done/fail/drop counters on fresh arm. |

**The 6 assertions in `SignalSafeUninstallTest` (all Rule-12 falsifiable):**

1. `armDisarmRoundTrip` — Arm/Disarm idempotency (return 0, second call returns 0)
2. `deferredUninstallFromSignalHandler` — core: install hook → arm → install handler → raise signal → WaitFor → verify hook uninstalled → verify stats `enq=1 done=1 fail=0 drop=0`. If the deferred design were broken (e.g., handler tried to call UninstallInlineHook directly from signal context), the process would deadlock or crash — the test would time out, not pass.
3. `multipleSignalsSequential` — 3 signals in sequence; verify `enq=3 done=3`
4. `statsMatchQueueOperations` — direct enqueue bypasses signal delivery, isolates queue mechanics
5. `unarmedSignalIsNoOp` — FromHandler returns -3, hook still fires, WaitFor times out
6. `signalHandlerDoesNotDeadlockUnderContention` — contention thread + signal raise + WaitFor under 2s. If the handler blocked on g_hook_mutex, process would hang (WaitFor times out). S65 log: `raise returned in 97us`, `WaitFor returned 0`.

**Native surface (S65 additions):**

- `SignalSafeUninstallArm()` — spawn worker + create eventfd, reset counters. Must be called from normal thread context before any signal-handler enqueue.
- `SignalSafeUninstallDisarm()` — stop worker, close fd, log final counters. Idempotent.
- `SignalSafeUninstallFromHandler(void* target)` — async-signal-safe enqueue. Returns 0 on success, -1 queue full, -2 null target, -3 not armed.
- `SignalSafeUninstallWaitFor(void* target, int timeoutMs)` — poll until both reg-clear and stats-settled.
- `SignalSafeUninstallStats(long out[4])` — {enqueued, completed, failed, dropped}.
- `fixture_deferred_uninstall` — new signal handler (handlerId == 2) in `SignalOverrideJni.cpp`; routes `sigovrInstall(signo, 2, target)` to the deferred handler with target as userCtx.

**Design constants:**
- `SIG_UNINSTALL_QUEUE_N = 64` — SPSC ring buffer size
- Worker poll timeout: 500ms — balances shutdown responsiveness vs. wasted wakeups
- WaitFor poll interval: 5ms — bounded latency, no busy-spin

**Meta-lesson (S65, opens Phase F class):** "Non-trivial native features
reveal bugs one subsystem at a time. The SPSC queue was correct; the memory
allocator assumed targets were far apart; the stats observation assumed
the worker completed before FindHandle cleared. Three fixes to three
subsystems, all exposed by three distinct test failures. Do not assume a
feature is one coherent block — the log reveals which subsystem's
invariant is being violated."

This is the first genuine native-deliverable session since S58, and it
required 3 orthogonal fixes to pass. It's the S62 class extended one layer
deeper: not just "multiple walls on one surface" but "multiple subsystems
in one feature."

**Test-isolation note:** `SignalSafeUninstallTest.@Before` disarms any
prior arm (idempotent), resets sigovr, calls `uninstallAll`. `@After`
does the same in reverse. Signal used is `SIGRTMIN+20` to avoid S52's
`SIGRTMIN+5/6` and any ART-reserved signals. Signal handler installed
per-test (not via attachBaseContext) since arming and disarming are
test-scoped.

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal:
- `booted-viber-order` + no new FATAL + providers ≥14 = canary green
- Any new FATAL, any boot regression, any provider count drop = canary red

**Canary state at Session 65 close (fresh process):** `outcome=booted-viber-order`, `elapsedMs ~1696ms`, `providersAttempted=15`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise, unchanged from S61–S64). S65's changes (`InlineHooker.cpp` uninstall path + allocation) don't affect the boot harness — expected identical to S64.

**Measurement discipline:** canary must be measured from fresh process — `-e class com.ims134.hookengine.GuestLoadTest` as the only class.

---

## Test suite state (as of Session 65 close)

**Executed:** 206 · **Registered:** 212 · **Ignored:** 6 · **Failed:** 0

Registered test classes (post-S65):

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
| CmdlineStackTest | 6 | ✅ S64 |
| **SignalSafeUninstallTest** | **6** | **✅ S65** |
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several (incl. DebuggerDetectProbeTest 1, DebuggerDetectProbe2Test 2, EmulatorDetectProbeTest 1, BuildPropProbeTest 1, NetTcpProbeTest 1, CmdlineStackProbeTest 1, **SignalSafeUninstallProbeTest 1**) | non-gating |

**Test count history (registered; the count includes `@Ignore`'d test methods):**
- S40: 51 · S41: 51 · S42: 57 · S43: 63 · S44: 68 · S45: 74 · S46: 80 · S47: 86 · S48: 92 · S49: 98
- S50 close: 104 · S51 close: 112 · S52 close: 118 · S53 close: 124
- S54 close: 130 · S55 close: 136 · S56 close: 142
- S57 close: 149
- S58 close: 155
- S59 close: 168
- S60 close: 177
- S61 close: 183 / 184 measured
- S62 close: 191
- S63 close: 198
- S64 close: 205
- **S65 close: 212** (+7: SignalSafeUninstallProbeTest 1, SignalSafeUninstallTest 6)

---

## Session 66 first actions (authoritative)

Phase E closed at S64. Phase F opened at S65. Session 66 targets Feature 8.7 (per-hook profiling).

1. Confirm Session 65 commit landed (`git log --oneline -3`).
2. **Fresh-process canary rerun** to confirm S65 no-regression. Expected
   `outcome=booted-viber-order`, `providersAttempted=15`, `providersSucceeded=14`.
3. **Probe the per-hook profiling surface first.** Before writing any code:
    - Enumerate every hook site currently registered (ChainHook slots, SvcHook slots, RetMod slots, RegInspect slots, InlineHooker registry, ArtMethodTrace slots).
    - For each: what is the current per-call overhead in the handler path (CPU) and the per-hook persistent allocation (memory)?
    - For each: what is the cost of instrumenting that overhead? (e.g., `clock_gettime(CLOCK_MONOTONIC_RAW)` per dispatch = ~30-50ns; alternative: `CNTVCT_EL0` register read = ~1-3ns.)
    - Determine whether per-hook profiling is feasible without slowing the handler path significantly. If `clock_gettime` in every handler is too expensive, consider sampling (e.g., every Nth dispatch) or a separate-profile-only mode.
    - Design: `HookProfile` struct with `{u64 total_ns, u64 call_count, u64 min_ns, u64 max_ns, u32 mem_bytes}`. Recording happens inside the handler via `rdtsc`-equivalent (`mrs x0, cntvct_el0`).
    - Likely candidates to instrument: ChainHook dispatcher (`chain_dispatch_helper`), ReturnMod dispatcher (`retmod_dispatch_helper`), InlineHooker relay (requires modifying `InstallInlineHookEx` relay), SvcHook SIGTRAP handler.
    - Simplest viable: add profiling to ChainHook and ReturnMod first (they already have C++ dispatch helpers), verify approach works, then extend to InlineHooker.
4. Write `HookProfileProbeTest.kt` if the surface isn't obvious from code inspection (which it mostly is — this is a "measure existing cost" probe).
5. Build + install (`.\build-and-install.ps1`).
6. `am instrument` gate 1.
7. Full suite gate.
8. Canary gate.
9. Commit, mark 8.7 → ✅.

**Expected mechanism candidates for 8.7:**
- **CNTVCT_EL0-based timing** — per-dispatch timing via the ARM64 virtual counter, reads in ~1-3ns on modern cores. Zero syscall overhead. Wraps `mrs x0, cntvct_el0` around the user handler call in each dispatcher.
- **Per-hook persistent storage** — extend `HookHandle` (in `HookRegistry.h`) with a `HookProfile` sub-struct, or add a parallel array.
- **Memory accounting** — for InlineHooker, relay+tramp pages are already tracked (`relay_size`, `tramp_size`); sum them per guest tag. For ChainHook, stub page is fixed (32 bytes); per-slot overhead is the handler array pointers (~128 bytes).
- **JNI surface** — `NativeTestBridge.hookProfileStats(target) -> long[4]` = {call_count, total_ns, min_ns, max_ns}, plus `hookProfileMemBytes(target) -> int`.

**Likely scope:** Medium. Touches `ChainHook.cpp`, `ReturnMod.cpp`, `HookRegistry.h/.cpp`, and adds a new `HookProfile.cpp` (or extends `HookRegistry.cpp`). ~300 lines of new C++, ~150 lines of test. Not as deep as S65's signal-safe work, but not a no-op session either.

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
- **The S65 meta-lesson:** non-trivial native features reveal bugs one subsystem at a time. The SPSC queue was correct; the memory allocator assumed targets were far apart; the stats observation assumed the worker completed before FindHandle cleared. Three fixes to three subsystems, all exposed by three distinct test failures. Do not assume a feature is one coherent block — the log reveals which subsystem's invariant is being violated.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim. Additions from Sessions 33–65:

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
- Feature 4.2 — Native ClassLoader chaining (verified on-device S54).
    - `nativeFNLN(guest, host)` in `NativeFnln.cpp` orchestrates the merge in C++.
    - Hidden-api reflection still goes through `HiddenApiBridge`.
    - `habFindFieldInHierarchy` walks the superclass chain: `PathClassLoader` has no `pathList`, but `BaseDexClassLoader` does.
    - `IsSameObject` must be used for identity comparison across local/global refs.
    - Return codes: 0 success, -1 invalid args, -2 pathList not found, -3 dexElements not found, -4 field get/set methodIDs not found, -5 pathList read failed, -6 dexElements read failed, -7 element class lookup failed, -8 merged array allocation failed, -9 Field.set threw.
- Feature 4.3 — Dynamic DEX loading (verified on-device S55).
    - `DexLoaderCreate(env, dexPath, optimizedDir, parent)` in `DexLoader.cpp`.
    - **ART initiator-binding quirk:** chain the loader into the target, then let the target be the first to load the class.
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
    - **S42 loopback-bind wall regressed** on current ROM build. `ServerSocket().bind()` now succeeds. Phase F candidate (S67).
- Feature 5.15 — /proc/self/cmdline full spoof (verified on-device S64). See §5.15 above.
    - **S19 `setArgV0` produces exactly 1 argv entry.** Kernel/Android NUL-pads the original buffer size. No residual args.
    - **`/proc/self/environ` is 19 standard Android env vars.** No engine identity.
- Feature 5.16 — /proc/self/syscall + stack filter (verified on-device S64). See §5.16 above.
    - **`/proc/self/syscall` format: 9 fields, whitespace-separated.** `<nr> <arg0> <arg1> <arg2> <arg3> <arg4> <arg5> <sp> <pc>`. `sp` and `pc` are hex-prefixed.
    - **`/proc/self/stack` read-denied by kernel `ptrace_may_access` capability check.** `File.readText()` throws `IOException: read failed: EACCES`. `Os.open()` succeeds, but `Os.read(fd, ...)` throws `ErrnoException EACCES (errno=13)`. This is a **kernel capability check**, not SELinux — no AVC line.
    - **`/proc/self/status` TracerPid: 0** via S23 `BuildFilteredStatus`.
    - **Path aliasing:** `/proc/self/<file>` works, `/proc/<pid>/<file>` works only for the caller's own pid.
- Feature 6.8 — Signal-safe deferred uninstall (verified on-device S65). See §6.8 above. Additional facts:
    - **`UninstallInlineHook` has three async-signal-safety violations:** two mutex acquisitions + nanosleep(100ms). The deferred pattern bypasses all three by offloading to a worker thread.
    - **SPSC ring buffer with spinlock-protected producer is sufficient.** The signal handler is single-threaded (one signal at a time per thread), so the "MPSC" concern doesn't apply; a spinlock CAS around the producer is safe and adds ~20ns worst case.
    - **`eventfd(EFD_NONBLOCK | EFD_CLOEXEC)` is the wakeup primitive.** `write(fd, &u64_one, 8)` from signal context is on the POSIX async-signal-safe list. Alternative `pipe()` would also work but eventfd avoids the extra file descriptor overhead.
    - **`AllocateMemoryNear`'s fixed-offset scan (1024 pages + 119 MB-strides) fails when two targets are within ~4KB of each other.** The first install consumes the free pages near both targets; the second install's fixed offsets all land on occupied pages. Fix: page-by-page outward scan. Fast case is 1-10 iterations (the offset just past the target's page). Worst case is ~30K iterations (mmap with MAP_FIXED_NOREPLACE fails fast — ~1us per EEXIST).
    - **`SignalSafeUninstallWaitFor` must wait for BOTH conditions:** registry-clear AND stats-settled. The registry clears inside `UninstallInlineHook`'s mutex; the stats increment happens after `UninstallInlineHook` returns — and UninstallInlineHook includes the 100ms deferred-free nanosleep. If WaitFor returns on registry-clear only, a caller reading stats immediately sees `completed=0`.
    - **`SignalSafeUninstallArm` must reset counters on fresh arm.** Tests run in sequence in the same process; without reset, stats accumulate and assertions on `enqueued==1` fail on the 2nd test.
    - **`SIGRTMIN+20` avoids S52's `SIGRTMIN+5/6` and any ART-reserved signals.** `SIGRTMIN` is a function call on bionic (__libc_current_sigrtmin), not a constant.
    - **Signal handler stack usage is < 256 bytes.** CAS acquire + memcpy(8) + write(8). Even MINSIGSTKSZ (8192) is 32× more than needed.
    - **Signal handler runtime is ~100us on this device.** The S65 log shows `raise returned in 97us`. This is the full path: sigqueue → kernel dispatch → so_dispatch → fixture_deferred_uninstall → SigUninstallEnqueue + eventfd write → return.

---

## The blunt version

**Four categories complete:** Category 1 (Hook mechanisms, 9/9), Category 2 (Memory primitives, 7/7), Category 3 (IPC / kernel boundary, 7/7), Category 4 (Class loading, 6/6). **Phase E CLOSED** at S64 — 5.11–5.16 all ✅, only 5.10 remains ⚠️ PARTIAL. **Phase F opened at S65** — 6.8 signal-safe uninstall ✅. Sessions 41–65 shipped twenty-eight verified primitives, all on-device verified with fail-able tests:

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
- 65: signal-safe uninstall (✅ — deferred SPSC queue + worker thread; `AllocateMemoryNear` page-scan rewrite)

**212 registered tests, 0 failed at S65 close.** Session 65 opened Phase F with a **genuine native deliverable** — the first since S58. The probe audited `UninstallInlineHook` and confirmed three async-signal-safety violations (two mutex acquisitions + nanosleep). The deferred pattern (Frida / Cydia Substrate / Xposed standard) fixed all three: signal handler enqueues into an SPSC ring buffer and returns in ~100us; worker thread drains and calls `UninstallInlineHook` from normal thread context.

**Three test failures across three subsystems, three fixes.** The SPSC queue was correct. The memory allocator assumed targets were far apart (fixed 3KB → `HOOK_ALLOC_FAILED` when two fixtures in the same .so). The stats observation assumed the worker completed before `FindHandle` cleared (race with the 100ms grace). Three fixes to three subsystems — allocator (page-scan), observation timing (WaitFor double-condition), stats reset (Arm clears counters) — all exposed by three distinct test failures.

**S65 extends the meta-lesson family into Phase F:** S59/S60/S61/S62 were about "the goal may already be achieved by an existing layer." S65 is about "a feature that looks like one block is actually three subsystems with three invariants." Both are the same discipline: **do not trust a naive reading of the code; the log reveals what's actually happening.**

**Phase F is 1/11 sessions in.** Remaining native work: 8.7 (per-hook profiling), 7.1 (unified ELF API), 8.2 (per-hook argument logging), plus the S67 loopback-bind wall re-measurement and Phase F disassembly items (S51 cache-hit, S60 Debug/VMDebug fast-JNI, §5.10 Settings.Global shared-mem path). Session 75 is the target.

**If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.**