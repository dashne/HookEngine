# HOOK ENGINE — COMPLETED FEATURE SPECIFICATION

This document defines what the engine **is** when fully complete. It is the target, not the current state. Current state lives below in "Status." If a feature here is not yet ✅ below, it is on the roadmap.

The engine is complete when every capability in this document is verified on-device, and the canary (Viber boot) remains green throughout.

---

## 1. Hook any function, at any granularity

**Native hooking:**
- Inline hook at any ARM64 instruction — handles all 9 ARM64 instruction kinds including `B.cond`, `CBZ/CBNZ`, `TBZ/TBNZ`, `LDR-literal`
- Inline hook at a runtime-computed address (not just a compile-time symbol)
- Inline hook at a mid-function offset (not just function entry)
- GOT/PLT hook — symbol-level redirect in any loaded shared library
- Hook chaining — multiple handlers on the same target, ordered, all fire in sequence
- Conditional hook — handler fires only when register state at the site matches a predicate
- Return-value modification — modify the caller's return value without changing control flow

**Java hooking:**
- ArtMethod hook (replace) — non-native Java methods, native methods (non-fast-JNI)
- ArtMethod entry trace — pre-call and post-call callbacks on the same method without replacing it
- Argument inspection — read boxed and primitive arguments at hook site
- Return-value inspection and modification

**Direct syscall interception:**
- SVC instruction hooking — intercept syscalls below the libc wrapper layer
- Per-syscall dispatch table — handle each syscall number independently

## 2. Instrument what the target is doing

- Method trace — log every call, argument set, and return value of a chosen class or method
- Call graph capture — record the call tree from a chosen entry point
- Register state inspection — read all ARM64 GP and FP registers at hook site
- Stack backtrace — walk the call stack at hook site (FP-chain plus DWARF unwind for stripped code)
- Per-hook execution counter — how many times, from which threads, at what timestamps
- Per-hook profiling — CPU cost and memory overhead of the handler

## 3. Read and modify process memory

- Read arbitrary memory — any address, any length, safe bounds
- Write arbitrary memory — with page protection save + restore
- Pattern scan — find byte patterns in any memory region with mask support
- Memory enumeration — list all readable/writable/executable regions with metadata
- Allocate memory near a target address — for trampolines and injected code

## 4. Intercept and rewrite IPC

- Binder transaction interception — at `IPCThreadState::transact`
- AIDL prelude parsing — decode interface token, argument list per interface
- AIDL reply rewriting — modify the reply parcel before returning to caller
- ServiceManager lookup interception — hook `getService`, `checkService`, `addService`
- ServiceManager addition — register a synthetic binder under any name
- Property read/write interception — hook `__system_property_get`, `__system_property_find`, `__system_property_read_callback`, `__system_property_read`
- Signal handling override — intercept and rewrite incoming signals

## 5. Control class loading and code injection

- Native ClassLoader chaining — engine-native, not Kotlin proxy (Rule 15 closure)
- Dynamic DEX loading — load a `.dex` file into the process at runtime
- DEX merging — merge two DEX files at runtime, present as one to the target's ClassLoader
- Native library load override — `dlopen` GOT hook, inject or redirect
- Resource table merging — add a second APK's resources via `AssetManager.addAssetPath`

## 6. Hide from detection

**File system:**
- Hide root binaries (`su`, `magisk`, `supersu`, `KernelSU`, `APatch`)
- Hide root filesystem paths (`/data/adb`, `/sbin/.magisk`, `/cache/magisk`, `/debug_ramdisk`)
- Filter `/proc/mounts`, `/proc/self/mounts`, `/proc/self/mountinfo` for overlay-mount and magisk-mount lines
- Filter `/proc/self/maps` — hide engine's own loaded library and symbol lines
- Filter `/proc/self/status` and `/proc/*/task/*/status` — force `TracerPid: 0`
- Filter `/proc/self/syscall` and `/proc/self/stack` — hide ptrace-attachment surface
- Full `/proc/self/cmdline` spoof — beyond `setArgV0`, remaining argv entries

**Debugger surface:**
- ADB detect blocking — `Settings.Global.ADB_ENABLED`, `ro.debuggable`, `ro.adb.secure`
- Debugger detect blocking — JDWP handshake, `Debug.isDebuggerConnected()`, `waitingForDebugger()`
- Emulator detect blocking — `ro.kernel.qemu`, `ro.hardware`, sensor fingerprint
- Test-keys / custom-build detect blocking — build tag rewrite, signature check shielding

**Network:**
- Frida port blocking — `27042`, `27043`, `27052`, `11111`
- IDA port blocking — `23946`
- Loopback interception for known forensics ports
- `/proc/net/tcp` and `/proc/net/tcp6` filtering — hide listening sockets belonging to engine

**Kernel:**
- `ptrace` syscall blocking — prevent target from attaching to other processes
- Anti-ptrace — detect and nullify `PTRACE_TRACEME` from target
- `execve` path filtering — block execution of root binaries from any source

**Environment:**
- Build property spoofing — `ro.debuggable`, `ro.secure`, `ro.build.tags`, `ro.build.type`, `ro.kernel.qemu`, `init.svc.adbd`, `service.adb.root`, `sys.oem_unlock_allowed`
- UID/GID spoofing — `getuid`, `geteuid`, `getgid`, `getegid`

## 7. Coordinate across the process

- Hook registry — every installed hook is registered with target address, replacement, original bytes, page protection, install state
- Per-guest tagging — group hooks by logical scope; uninstall one group without disturbing others
- Atomic install / uninstall — no partial state on failure
- Rollback on partial failure — if any step of a multi-step install fails, all prior steps unwind
- Deferred-free under contention — uninstall waits for any in-flight execution of the trampoline before freeing pages
- Signal-safe uninstall — uninstall is safe from any signal handler context
- Inter-hook conflict detection — refuse to install on an address another hook already owns
- ART version capability gate — refuse to install on unverified ART builds

## 8. Symbol and ELF resolution

- ELF symbol lookup by name — find any exported function in any loaded library
- `.dynsym` walk — dynamic symbol table
- `.symtab` walk — static symbol table (local symbols)
- GNU hash + SysV hash support — both symbol table variants
- Multi-library search — search every loaded library in one call
- PLT stub resolution — locate the PLT entry for a given import

## 9. Diagnose the engine itself

- Full regression test suite — page-protection restore, relay leak detection, deferred-free contention, patch safety, rollback
- Relocation test suite — every instruction kind, boundary cases, fallback paths
- Anti-detection test suite — every filter asserts its specific behavior
- Runtime hook inspection — enumerate all installed hooks from JNI or from within a hook
- Per-hook execution counters — dispatch count, effective-rewrite count
- Per-hook CPU / memory profiling — handler cost accounting

## 10. Preserve host-process stability

- Every hook installation restores the target page's original protection on completion — no lingering RWX pages
- Every trampoline is reachable in ±128MB of its target — no inline hook fails due to branch-range
- Every uninstall restores the exact original bytes at the target — byte-identical
- No hook install leaves a stale registry entry on any failure path
- Every engine entry point guards against a target that has been unmapped or re-protected by the host
- No JNI call in a hook dispatch path may touch a `jobject` on a caller thread with a JIT `BinderProxy.transact` frame above it — all `jobject` operations offload to a fresh attached thread

---

## Feature count

Approximate totals when fully complete:

- **Hook mechanisms:** ~15 distinct capabilities across inline / GOT / ArtMethod / dynamic / chained / conditional / return-modify / trace / SVC
- **Instrumentation:** ~6 capabilities across trace / call-graph / registers / backtrace / counters / profiling
- **Memory primitives:** ~5 capabilities
- **IPC / kernel boundary:** ~7 capabilities across Binder / AIDL / ServiceManager / property / signal
- **Class loading:** ~5 capabilities
- **Anti-detection:** ~25 distinct filters across filesystem / debugger / network / kernel / environment
- **Coordination:** ~9 capabilities across registry / tagging / atomic / rollback / deferred-free / signal-safe / conflict / gate
- **Symbol resolution:** ~6 capabilities
- **Diagnostics:** ~6 capabilities
- **Stability invariants:** ~6 guarantees

**~85 distinct capabilities** spread across 10 categories.

---

## What the engine is NOT

Not claimed by this specification, and out of scope unless separately approved:

- A full dynamic binary translation (DBI) engine (Frida-Stalker-class instruction-level tracing)
- A scripting runtime (Lua, JS, embedded interpreter for live hook writing)
- Cross-process injection into other apps (requires root)
- Multi-Android-version support (single target: API 37, arm64, ART)
- Symbolic execution / taint analysis / constraint solvers
- Kernel-mode hooking / eBPF / kprobes

Those are research-grade instrumentation frameworks, distinct from a hook engine. If needed, they become a separate project with a separate specification.

---

## Canary

Viber boot under the engine is a **regression canary**, not a target. Every session ends with `GuestLoadTest` green (`outcome=booted-viber-order`, providers ≥14, no new FATAL). If the canary breaks, an engine feature broke — fix the engine feature, then continue. Viber UI, Viber functionality, Viber network — not in scope, not pursued, not tracked.

**IMPORTANT measurement note:** the canary must be measured from a **fresh process** — a run where `GuestLoadTest` is the only test invoked. Running `GuestLoadTest` inside the full suite (position 50 of 84) has `elapsedMs=16000+` and `providersSucceeded=3` because 49 test classes have already fragmented the process — `FileProvider` and other providers time out. That is **not** the canary measurement. The canary is: `adb shell am instrument -w -r -e class com.ims134.hookengine.GuestLoadTest ...`. Fresh process. Baseline: `elapsedMs ~1700ms`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise).


# PROJECT STATE

Last updated: **Session 46 close (2026-10-26)**. Supersedes Session-45-close version. Sessions 41–46 landed on disk and are on-device verified (80 tests executed, 0 failed at Session 46 close). Session 47 target: Feature 1.6 — SVC instruction hooking (closes Category 1).

---

## What this project is

**An advanced native hook engine for Android arm64 / ART 37.**

The engine's job is to make a live process auditable and patchable at the code level:

- Hook any function, at any instruction, in any loaded library, native or Java
- Trace method entry/exit, inspect register state, modify return values
- Scan and patch process memory
- Rewrite Binder IPC at the AIDL payload level
- Override syscall-reachable surface (files, /proc, properties, sockets, ptrace)
- Load and instrument additional code at runtime
- Survive under active detection

The target is defined by the **feature inventory** below — not by any particular app booting. When Categories 1–8 in the inventory are all ✅, the engine is done.

## What this project is NOT

- **Not a clone app.** Filesystem virtualization, per-clone identity, notification namespacing, AMS task registration, Activity UI rendering — these are clone-app features. Out of scope unless the operator explicitly adds them as separate work.
- **Not a reference-reproduction project.** `com.excelliance.kxqp.platform.ApplicationProxy` natives are not a goal. Whether `nativeADB` or `nativeJH*` gets ported is a question of whether the *engine* needs the underlying primitive, not whether the reference ships the method.
- **Not a "get Viber to work" project.** Viber is a **regression canary**. Its role is to verify the engine primitives haven't broken under real integration load. Its state — `booted`, `crashed`, `rendered`, `blank` — is diagnostic data, not a milestone.
- **Not a coverage-counter project.** The `X/37` ApplicationProxy method count is retired as a progress metric. It is kept in this document as a snapshot of the canary test, nothing more.

---

## The drift — acknowledged

Sessions 1–14 built the primitive foundation: inline hook, GOT hook, ArtMethod hook, ARM64 relocation (partial), hidden-API bypass, Binder transact hook, Parcel rewriting, `nativeCNVM`. That was engine work.

Sessions 15–40 did **not** deepen the engine. They built a container around it: dual-Application boot, GuestClassLoader, LoadedApk manipulation, `makeApplication` path, Wall A/C2/C2b fixes, token steal, WMS probes, Activity lifecycle. All of that is container work, all of it was labeled "integration depth," all of it consumed 26 sessions.

Coverage growth in that period: **one port** — `nativeInit` at Session 38. Everything else stayed flat at 14/37.

**That is not a resumption of engine work. That is a detour.** The container built in Sessions 15–40 is now the canary. Its continued function is required (regression coverage). Its further development is out of scope.

Sessions 41–46 resumed engine work:
- **S41** verified Feature 1.1 — full ARM64 relocation (9 kinds).
- **S42** shipped Feature 1.5 — dynamic runtime attach by computed address.
- **S43** shipped Feature 1.7 — hook chaining (ordered multi-handler).
- **S44** shipped Feature 1.4 — ArtMethod entry/exit trace.
- **S45** shipped Feature 1.8 — conditional hook (per-handler predicate gating).
- **S46** shipped Feature 1.9 — return-value modification (post-call rewrite).

Session 47 target: Feature 1.6 — SVC instruction hooking, closing Category 1.

---

## Current verified state

**Engine primitives (verified on-device, all green at Session 46 close):**
- Inline hook: works for all 9 ARM64 instruction kinds. Verified by RelocTest (1 test, 49 assertions).
- GOT hook: 26 sites, works.
- ArtMethod hook: works (native methods, non-fast-JNI only).
- ArtMethod entry/exit trace: works on framework natives. Verified by ArtMethodTraceTest (5 tests).
- Hook chaining: works. Verified by ChainHookTest (6 tests).
- Conditional hook: works. Predicate gates handler fire per-slot. Verified by ConditionalHookTest (6 tests).
- Return-value modification: works. Post-call modifier transforms caller-visible x0. Verified by ReturnModTest (6 tests).
- Dynamic runtime attach: works with alignment/mapped/exec guards. Verified by DynamicAttachTest (6 tests).
- Hidden-API bypass: works via thread-attached reflection.
- `IPCThreadState::transact` inline hook: works.
- AIDL prelude rewrite: works on 4 interfaces.
- `nativeCNVM`: decomp-verified, 4 tests green.
- `nativeInit` (structural): verified Session 38.

**Container / canary (verified on-device, fresh-process):**
- Viber boots: `outcome=booted-viber-order`, providers 14/15 (MlKit noise), `elapsedMs ~1700ms`.
- SplashActivity: instantiated, `onCreate`/`onStart`/`onResume` all run.
- WMS accepts token + allocates Surface when host task visible.

**Canary function:** if `GuestLoadTest` returns `outcome != "?"` and no new FATAL, engine primitives still work in a real integration scenario. **Always measure from fresh process** — see Canary section above.

---

## Engine feature inventory — the actual target

Status: ✅ have + verified · ⚠️ partial · ❌ not started · 🔧 written but unverified

### Category 1 — Hook mechanisms

| # | Feature | Status |
|---|---|---|
| 1.1 | Inline hook at any ARM64 instruction | ✅ |
| 1.2 | GOT/PLT symbol hook | ✅ |
| 1.3 | ArtMethod hook (replace) | ✅ |
| 1.4 | ArtMethod entry/exit trace (pre + post, no replace) | ✅ |
| 1.5 | Dynamic runtime attach by computed address | ✅ |
| 1.6 | SVC instruction hooking (bypass libc) | ❌ → **S47 target** |
| 1.7 | Hook chaining (ordered multi-handler) | ✅ |
| 1.8 | Conditional hook (register inspection at site) | ✅ |
| 1.9 | Return-value modification (post-call trampoline) | ✅ |

**Category 1: 8/9 complete.** Only 1.6 remains. Category closes after Session 47.

### Category 2 — Memory primitives

| # | Feature | Status |
|---|---|---|
| 2.1 | Read memory at arbitrary address | ❌ |
| 2.2 | Write memory (page-prot safe) | ⚠️ |
| 2.3 | Scan memory for pattern | ❌ |
| 2.4 | Allocate memory near target | ✅ |
| 2.5 | Enumerate maps | ⚠️ |
| 2.7 | Register state inspection at hook site | ❌ |
| 2.8 | Backtrace from hook site | ❌ |

### Category 3 — IPC / kernel boundary

| # | Feature | Status |
|---|---|---|
| 3.1 | Binder transact interception | ✅ |
| 3.2 | AIDL prelude parse | ✅ |
| 3.3 | AIDL reply rewrite | ✅ |
| 3.4 | ServiceManager lookup interception | ❌ |
| 3.5 | Property read/write interception | ✅ |
| 3.6 | Signal handling override | ❌ |
| 3.7 | Native ServiceManager addition | ❌ |

### Category 4 — Class loading / code isolation

| # | Feature | Status |
|---|---|---|
| 4.1 | Custom ClassLoader delegation | ✅ |
| 4.2 | Native ClassLoader chaining | ⚠️ (Kotlin) |
| 4.3 | Dynamic DEX loading | ❌ |
| 4.4 | DEX merging at runtime | ❌ |
| 4.5 | Resource merging | ✅ |
| 4.6 | Native library load override | ✅ |

### Category 5 — Anti-detection

| # | Feature | Status |
|---|---|---|
| 5.1–5.5 | File/maps/status/mounts filters | ✅ |
| 5.6 | Build property spoofing | ⚠️ (9 props) |
| 5.7 | UID/GID spoofing | ✅ |
| 5.8 | Frida/IDA port blocking | ✅ |
| 5.9 | ptrace blocking | ✅ |
| 5.10 | ADB detect blocking | ❌ |
| 5.11 | Debugger detect (JDWP full) | ⚠️ |
| 5.12 | Emulator detect blocking | ❌ |
| 5.13 | Test-keys detect blocking | ❌ |
| 5.14 | /proc/net/tcp analysis blocking | ❌ |
| 5.15 | /proc/self/cmdline full spoof | ⚠️ |
| 5.16 | /proc/self/syscall + stack filter | ❌ |

### Category 6 — Safety / correctness

| # | Feature | Status |
|---|---|---|
| 6.1 | Hook registry | ✅ |
| 6.2 | Per-guest tags | ❌ (phantom S42 batch — never shipped) |
| 6.3 | Atomic install / uninstall | ✅ |
| 6.4 | Deferred-free under contention | ✅ |
| 6.5 | Rollback on partial failure | ✅ |
| 6.6 | Page protection read + restore | ✅ |
| 6.7 | Inter-hook conflict detection | ✅ |
| 6.8 | Signal-safe uninstall | ⚠️ |
| 6.9 | Capability gate (ART version) | ✅ |

### Category 7 — Symbol / ELF utilities

| # | Feature | Status |
|---|---|---|
| 7.1 | ELF symbol resolution (unified API) | ⚠️ (scattered ad-hoc implementations) |
| 7.2 | .dynsym walk | ✅ |
| 7.3 | .symtab walk | ✅ |
| 7.4 | GNU + SysV hash support | ✅ |
| 7.5 | Multi-lib search | ✅ |
| 7.6 | PLT stub resolution | ❌ |

### Category 8 — Runtime diagnostics

| # | Feature | Status |
|---|---|---|
| 8.1 | Per-hook execution counter | ✅ |
| 8.2 | Per-hook argument logging | ⚠️ |
| 8.3 | Runtime hook inspection | ✅ |
| 8.4 | Regression test suite | ✅ |
| 8.5 | Reloc test suite | ✅ |
| 8.6 | Anti-detection test suite | ✅ |
| 8.7 | Profiling (CPU / memory per hook) | ❌ |

### Totals

- **✅ verified complete:** ~30 features
- **⚠️ partial:** ~8 features
- **❌ not started:** ~23 features

**Engine completion requires ~28 more sessions.**

---

## Engine roadmap — Session 47 to Session 75

### Phase B — Hook mechanism depth (Sessions 45–48)

| Session | Feature | Status / Exit |
|---|---|---|
| 45 | 1.8 Conditional hook | ✅ `ConditionalHookTest` |
| 46 | 1.9 Return-value modification | ✅ `ReturnModTest` |
| **47** | **1.6 SVC instruction hooking** | **next — `SvcHookTest`** |
| 48 | 2.7 Register state inspection | `RegInspectTest` |

After S48: **Category 1 complete. Category 2 (except 2.1–2.3) complete.**

### Phase C — Memory + kernel boundary (Sessions 49–53)

| Session | Feature | Exit |
|---|---|---|
| 49 | 2.1 / 2.2 / 2.3 Memory read / write / scan | `MemoryOpsTest` |
| 50 | 2.5 Full maps enumeration | `MapsEnumTest` |
| 51 | 3.4 ServiceManager lookup interception | `ServiceManagerTest` |
| 52 | 3.6 Signal handling override | `SignalOverrideTest` |
| 53 | 2.8 Backtrace from hook site | `BacktraceTest` |

### Phase D — Class loading + symbol depth (Sessions 54–58)

| Session | Feature | Exit |
|---|---|---|
| 54 | 4.2 Native `nativeFNLN` (Rule 15 debt closure) | `NativeFnlnTest` |
| 55 | 4.3 Dynamic DEX loading | `DexLoadTest` |
| 56 | 4.4 DEX merging at runtime | `DexMergeTest` |
| 57 | 7.6 PLT stub resolution | `PltResolveTest` |
| 58 | 3.7 Native ServiceManager addition | `SmAddTest` |

### Phase E — Anti-detection completion (Sessions 59–64)

| Session | Feature | Exit |
|---|---|---|
| 59 | 5.10 ADB detect blocking | `AdbDetectTest` |
| 60 | 5.11 Debugger detect (JDWP + full status) | `DebuggerDetectTest` |
| 61 | 5.12 Emulator detect blocking | `EmulatorDetectTest` |
| 62 | 5.13 Test-keys / custom build detect | `BuildPropTest` |
| 63 | 5.14 /proc/net/tcp blocking | `NetTcpTest` |
| 64 | 5.15 /proc/self/cmdline + 5.16 syscall/stack filter | `CmdlineStackTest` |

### Phase F — Polish + close (Sessions 65–75)

| Session | Feature | Exit |
|---|---|---|
| 65 | 6.8 Signal-safe uninstall | `SignalSafeUninstallTest` |
| 66 | 8.7 Profiling (CPU / memory per hook) | `HookProfileTest` |
| 67–73 | Address any ⚠️ partials surfaced by prior phases | Depends |
| 74 | Full engine regression run + `PROJECT_STATE.md` final close | All suites + canary green |
| 75 | Reserved | Depends |

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load. Nothing else.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal:
- `booted-viber-order` + no new FATAL + providers ≥14 = canary green
- Any new FATAL, any boot regression, any provider count drop = canary red = an engine primitive broke

**Measurement discipline (added S46):** the canary **must** be measured from a fresh process — a run where `GuestLoadTest` is the only test invoked. Running `GuestLoadTest` inside the full suite (position 50 of 84) has `elapsedMs=16000+` and `providersSucceeded=3` because 49 test classes have already fragmented the process and providers time out. That is **not** the canary measurement — it's a stress-data reading, not a canary.

**Canary state at Session 46 close (fresh process):** green. `outcome=booted-viber-order`, `elapsedMs=1681`, `providersSucceeded=14` / `providersFailed=1` (MlKit noise).

**Canary failure protocol:** canary goes red (fresh process) → identify which engine primitive broke → fix that primitive → canary green → continue.

---

## Test suite state (as of Session 46 close)

**Executed:** 80 · **Registered:** 84 · **Ignored:** 4 · **Failed:** 0

Registered test classes:

| Class | Tests | Status |
|---|---|---|
| NativeItemTest | 5 | ✅ |
| ProcessHooksTest | 4 | ✅ |
| RegressionTest | 5 | ✅ |
| RelocTest | 1 (49 assertions) | ✅ S41 |
| ProxyBridgeSurfaceTest | 4 | ✅ |
| NativeInitTest | 2 | ✅ S39 |
| AbiProbeTest | 2 | ✅ S39 |
| ThreadStatusProbeTest | 1 | ✅ S39 |
| MountsProbeTest | 2 | ✅ S40 |
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
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several | non-gating |

**Test count history:**
- S40 close: 51 executed
- S41 close: 51 executed
- S42 close: 57 executed (+6 DynamicAttachTest)
- S43 close: 63 executed (+6 ChainHookTest)
- S44 close: 68 executed (+5 ArtMethodTraceTest)
- S45 close: 74 executed (+6 ConditionalHookTest)
- S46 close: **80 executed** (+6 ReturnModTest)
- S47 target: 86 executed (+6 SvcHookTest)

---

## Session 47 first actions (authoritative)

1. Confirm Session 46 commit landed (`git log --oneline -3`).
2. Write Feature 1.6 — SVC instruction hooking. This is the hardest hook mechanism in Category 1. See plan below.
3. Build + install (Rule 14).
4. `am instrument` SvcHookTest. Expect `OK (6 tests)`.
5. Full suite. Expect `OK (86 tests)`.
6. **Canary from fresh process** (not in-suite). Expect `outcome=booted-viber-order`, `providersSucceeded ≥ 14`, `elapsedMs ~1700ms`.
7. Commit, mark Feature 1.6 → ✅, **close Category 1 (9/9)**.

### Feature 1.6 planning note

SVC hooking is fundamentally harder than inline/GOT/chaining because SVC is a synchronous exception-generating instruction. Design candidates:

1. **SIGILL-based trap**: replace an SVC with `BRK #imm` or patch the SVC's condition to `svc #0` with a different immediate to trigger a specific signal. Install a `sigaction(SIGILL)` handler that decodes the PC, determines the syscall number (in `x8`) and args, dispatches to a per-syscall handler, then either skips the SVC (return handled value in `x0`) or re-executes it. Requires signal-safe uninstall (Feature 6.8, currently ⚠️) — the SVC hook is what forces that conversation.
2. **Patching at `svc` call sites**: scan for `svc #0` instructions in a target function or library, replace with `bl trampoline`. Reaches every SVC from that library. Not universal — some libc SVCs are in read-only mapped shared memory that we can mprotect, others are in JIT that we can't.
3. **libc syscall() function hook**: already effectively done via GOT. Bypasses SVC level; not what 1.6 specifies.

**Session 47 will start with option 1 (SIGILL trap).** Option 1 is decomp-friendly, applies to every SVC in every library, and aligns with what 1.6 actually requires ("intercept syscalls below the libc wrapper layer"). If SIGILL proves infeasible on this ROM (some ROMs install their own SIGILL handlers), we document and fall back to a variant.

**Prerequisite risk:** Feature 6.8 (signal-safe uninstall) is currently ⚠️. SVC hook uninstall must be signal-safe by construction. Session 47 will either (a) implement a signal-safe uninstall path for the SVC hook specifically, or (b) bump 6.8 from ⚠️ to ✅ as part of Session 47 and re-order the roadmap. The latter is cleaner if we do it. Session 47's deliverable scope will include either option; we'll know which after the first probe.

---

## Rules of engagement (unchanged, still authoritative)

- Target is the engine feature inventory, not ApplicationProxy coverage.
- Viber is canary only. **Canary must be measured from fresh process** — in-suite GuestLoadTest readings are not the canary.
- Every session advances at least one ⚠️/❌/🔧 → ✅.
- Verification-first: if a prior session left unverified code on disk, verify before writing new code.
- Rule 5 (behavior match) — partial disclosure for structural ports: `Rule 5 partial: <reason>` in commit.
- Rule 12: test assertions must be able to fail.
- Rule 14: every test-run instruction includes `./gradlew` + `adb install -r -t` on **both** APKs.
- Rule 15: engine is native. Kotlin/Java in `app/src/main` limited to JNI surface, `DualApplication`, and pre-port Kotlin proxies.
- Rule 7 (decomp-first) when wire format / symbol / field layout is uncertain.
- **No `am instrument` output = no session close.**
- Deferred items stay deferred: `handleJHMethod` (S18), tnFIN code-36 byte rewrite (S12), clone-app features.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim. Additions from Sessions 33–46:

**SYS_getpid / SYS_getuid via `syscall()` bypass both inline and GOT hooks.** (S33, verified S34.)

**AttributionSource layout differs by construction path.** (S32/33, verified S34.)

**`Process.myUid()` returning spoofed 10000 propagates into ApplicationInfo.uid.** (S32, verified S34.)

**`mSurface.isValid()` gated on `mAppVisible` gated on host task visibility.** State precondition, not a fixed-state behavior. (S36/37.)

**MIUI ProcessManager threads are background, non-blocking.** (S35+.)

**Full ARM64 relocation handles 9 kinds.** (S41, verified on-device.)

**`AttachRuntime` adds alignment/mapped/exec guards.** (S42, verified on-device.)

**ChainHook stub page layout (32-byte slots).** (S43, verified on-device.)

**`visibility("hidden")` on `chain_dispatch_helper`** required for adrp+add in the naked asm. (S43, verified on-device.)

**Feature 1.8 adds per-slot predicates, `nullptr` = unconditional fire.** (S45, verified on-device.)

**Feature 1.9 mechanism (verified on-device S46).**
- Per-slot naked-asm stub (macro-generated, 16 slots). Stub saves `x0..x7` + `x30` to an 80-byte frame, `blr`s the shadow trampoline (body runs, x0 = orig_ret), then calls `retmod_dispatch_helper(args, orig_ret, slot)`, restores `x30`, `ret`s with x0 = modified value.
- **Shadow trampoline** built before install via `EmitRelocated` + `EncodeBranch` + `AllocateMemoryNear`, published to `g_retmod_tramps[slot]` BEFORE `InstallInlineHookEx` runs. Eliminates install→populate race.
- `g_retmod_tramps` and `retmod_dispatch_helper` need `visibility("hidden")` for adrp+add (same as ChainHook).
- `AllocateMemoryNear` is C++-linkage (not extern "C") — declared matching in ReturnMod.cpp.
- Detach: `UninstallInlineHook` (100ms deferred-free covers in-flight stubs) → clear `g_retmod_tramps[slot]` → munmap shadow.
- Known limitations: modifier must not throw (naked asm can't unwind); async signals during stub execution may misbehave; max 16 concurrent retmods.

**Test-fixture declaration order in `native-lib.cpp` matters.** (S45 on-disk fix; carried forward in S46.)

**Fast-JNI constraint on ArtMethod hooks.** (S44, verified on-device.)

**JNI symbol name-mangling.** `foo_bar` → `Java_..._foo_1bar`. (S43.)

**Chained assignment of heterogeneous function pointers is an error.** (S44.)

**`findstr` on binary `.so` returns empty.** (S43.)

**Verifying an APK rebuild deployed.** (S43.)

**`ActivityBootProbeTest` retired.** (S41.)

**Canary crash suppressor.** (S41.)

**Canary measurement discipline (S46 — critical).** `GuestLoadTest` writes `guest-boot.txt` in BOTH fresh-process and in-suite invocations, and the file only reflects the LAST write. If the full suite runs first, `guest-boot.txt` shows the in-suite reading (elapsedMs 16000+, providersSucceeded=3, FileProvider TIMEOUT), which is **not** the canary. The canary measurement is only the fresh-process run: `-e class com.ims134.hookengine.GuestLoadTest` as the only class. Baseline: `elapsedMs ~1700ms`, `providersSucceeded=14` / `providersFailed=1` (MlKit noise). Diagnose canary red only against this fresh-process reading.

**Test count reduction is acceptable.** Baseline 80 executed at S46 close; S47 target 86.

**Phantom S42 batch warning.** Hook registry tags + `ElfSymbols.h` never shipped. 6.2 = ❌, 7.1 = ⚠️.

---

## The blunt version

Sessions 41–46 shipped six verified primitives: full ARM64 relocation, dynamic attach, hook chaining, ArtMethod trace, conditional hook, return-value modification. Each backed by a test class that runs on-device and can fail. That is six engines of hook work, not container drift.

Session 47 ships Feature 1.6 (SVC hooking) and closes Category 1 — 9/9. Category 1 has been the foundational hook mechanism category since Session 1. Closing it is a milestone. The SIGILL-trap design is the natural path; if it proves infeasible on this ROM we document and fall back.

No more Viber walls. No more "integration depth." No more coverage counter obsession. Engine only. Canary green.

If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.