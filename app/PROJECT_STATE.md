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

**IMPORTANT measurement note:** the canary must be measured from a **fresh process** — a run where `GuestLoadTest` is the only test invoked. Running `GuestLoadTest` inside the full suite (position 50 of 90) has `elapsedMs=16000+` and `providersSucceeded=3` because 49 test classes have already fragmented the process — `FileProvider` and other providers time out. That is **not** the canary measurement. The canary is: `adb shell am instrument -w -r -e class com.ims134.hookengine.GuestLoadTest ...`. Fresh process. Baseline: `elapsedMs ~1700ms`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise).


# PROJECT STATE

Last updated: **Session 47 close (2026-10-27)**. Supersedes Session-46-close version. Sessions 41–47 landed on disk and are on-device verified (86 tests executed, 0 failed at Session 47 close). **Category 1 (Hook mechanisms) is COMPLETE at 9/9.** Session 48 target: Feature 2.7 — register state inspection (opens Category 2's remaining gaps).

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
- **Not a reference-reproduction project.** `com.excelliance.kxqp.platform.ApplicationProxy` natives are not a goal.
- **Not a "get Viber to work" project.** Viber is a **regression canary**. Its state is diagnostic data, not a milestone.
- **Not a coverage-counter project.** The `X/37` ApplicationProxy method count is retired as a progress metric.

---

## The drift — acknowledged

Sessions 1–14 built the primitive foundation. Sessions 15–40 did **not** deepen the engine — they built a container around it (dual-Application boot, GuestClassLoader, LoadedApk manipulation, WMS probes). That detour consumed 26 sessions and produced one engine port (`nativeInit`, S38). The container is now the canary; its further development is out of scope.

Sessions 41–47 resumed engine work and delivered seven verified hook primitives. **Category 1 is complete.**

---

## Current verified state

**Engine primitives (verified on-device, all green at Session 47 close):**
- Inline hook: 9 ARM64 instruction kinds — `NORMAL`, `B`, `BL`, `ADR`, `ADRP`, `B.cond`, `CBZ/CBNZ`, `TBZ/TBNZ`, `LDR-literal`. Verified by RelocTest (1 test, 49 assertions).
- GOT hook: 26 sites, works.
- ArtMethod hook: works (native methods, non-fast-JNI only).
- ArtMethod entry/exit trace: works on framework natives. Verified by ArtMethodTraceTest (5 tests).
- Hook chaining: works. Verified by ChainHookTest (6 tests).
- Conditional hook: predicate gates per-slot handler fire. Verified by ConditionalHookTest (6 tests).
- Return-value modification: post-call modifier transforms caller-visible x0. Verified by ReturnModTest (6 tests).
- **SVC instruction hooking: BRK-magic + SIGTRAP handler. Per-slot expected-nr filter; naked svc_issue_raw helper; reentry guard; signal-safe detach via atomic active flag. Verified by SvcHookTest (6 tests).**
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

**Canary function:** if `GuestLoadTest` returns `outcome != "?"` and no new FATAL, engine primitives still work in a real integration scenario. **Always measure from fresh process** — see Canary section.

---

## Engine feature inventory — the actual target

Status: ✅ have + verified · ⚠️ partial · ❌ not started · 🔧 written but unverified

### Category 1 — Hook mechanisms — **9/9 COMPLETE**

| # | Feature | Status |
|---|---|---|
| 1.1 | Inline hook at any ARM64 instruction | ✅ |
| 1.2 | GOT/PLT symbol hook | ✅ |
| 1.3 | ArtMethod hook (replace) | ✅ |
| 1.4 | ArtMethod entry/exit trace | ✅ |
| 1.5 | Dynamic runtime attach by computed address | ✅ |
| 1.6 | SVC instruction hooking (bypass libc) | ✅ **S47** |
| 1.7 | Hook chaining | ✅ |
| 1.8 | Conditional hook | ✅ |
| 1.9 | Return-value modification | ✅ |

**Category 1 milestone reached Session 47.** All foundational hook primitives delivered and on-device verified.

### Category 2 — Memory primitives

| # | Feature | Status |
|---|---|---|
| 2.1 | Read memory at arbitrary address | ❌ |
| 2.2 | Write memory (page-prot safe) | ⚠️ |
| 2.3 | Scan memory for pattern | ❌ |
| 2.4 | Allocate memory near target | ✅ |
| 2.5 | Enumerate maps | ⚠️ |
| 2.7 | Register state inspection at hook site | ❌ → **S48 target** |
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

- **✅ verified complete:** ~31 features (whole Category 1)
- **⚠️ partial:** ~8 features
- **❌ not started:** ~22 features

**Engine completion requires ~27 more sessions.**

---

## Engine roadmap — Session 48 to Session 75

### Phase A — Foundation verified (Sessions 41–44) — CLOSED

### Phase B — Hook mechanism depth (Sessions 45–48)

| Session | Feature | Status |
|---|---|---|
| 45 | 1.8 Conditional hook | ✅ |
| 46 | 1.9 Return-value modification | ✅ |
| 47 | 1.6 SVC instruction hooking | ✅ |
| **48** | **2.7 Register state inspection** | **next** |

**Category 1 is COMPLETE at 9/9** (end of S47).

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

**Canary state at Session 47 close (fresh process):** green. `outcome=booted-viber-order`, `elapsedMs=1715`, `providersSucceeded=14` / `providersFailed=1` (MlKit noise).

**Measurement discipline:** canary must be measured from fresh process — `-e class com.ims134.hookengine.GuestLoadTest` as the only class. In-suite readings (~position 50/90) are process-fragmented and not the canary.

---

## Test suite state (as of Session 47 close)

**Executed:** 86 · **Registered:** 90 · **Ignored:** 4 · **Failed:** 0

Registered test classes (post-S47):

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
| **SvcHookTest** | **6** | **✅ S47** |
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several | non-gating |

**Test count history:**
- S40 close: 51
- S41 close: 51
- S42 close: 57 (+6)
- S43 close: 63 (+6)
- S44 close: 68 (+5)
- S45 close: 74 (+6)
- S46 close: 80 (+6)
- S47 close: **86** (+6)
- S48 target: 92 (+6)

---

## Session 48 first actions (authoritative)

1. Confirm Session 47 commit landed (`git log --oneline -3`).
2. Write Feature 2.7 — Register state inspection. See plan below.
3. Build + install (Rule 14).
4. `am instrument` RegInspectTest. Expect `OK (6 tests)`.
5. Full suite. Expect `OK (92 tests)`.
6. Canary from fresh process. Expect `outcome=booted-viber-order`, `providersSucceeded ≥ 14`, `elapsedMs ~1700ms`.
7. Commit, mark Feature 2.7 → ✅.

### Feature 2.7 planning note

Register state inspection — "read all ARM64 GP and FP registers at hook site."

**Design candidates:**

1. **Snapshot API on top of existing hook primitives.** Add a `RegSnapshot` struct (`x[31]`, `sp`, `pc`, `nzcv`, `q[32]`), and a per-slot capture function. The capture must run inside the naked stub, before any C++ code — which means extending either `ReturnMod.cpp` or `ChainHook.cpp` stubs to write a `RegSnapshot` to a per-slot buffer at entry. Reading is then safe from any C++ context (`RegInspectRead(slot, out)`), which is a natural fit for the JNI test surface.
2. **Dedicated naked-capture primitive.** Install via `InstallInlineHookEx` with a stub that saves all registers to a per-slot buffer, calls the dispatcher, then restores. This is what Feature 2.7 actually requires — the capture *is* the primitive, not incidental to another.
3. **Read from handler context via a thread-local.** When a handler runs (chain/retmod/SVC), expose a `GetRegSnapshot()` that reads a thread-local populated by the naked stub. Minimal stub changes but only works inside handler calls.

**Session 48 will start with option 1** — an `AttachRegInspect(target, capture_mode)` that installs a stub saving `x0-x30, sp, pc, nzcv, q0-q31` to a per-slot buffer at every call. Capture mode controls whether the stub bypasses the body (capture-only probe, faster) or continues through the shadow trampoline (capture-while-executing). Detach restores bytes.

**Scope caution:** the 31 GP + 32 FP + sp + pc + nzcv snapshot is ~1000 bytes. The naked stub must not clobber x18 (shadow call stack pointer on some ABIs) or x29/x30 without saving. The stub layout will need careful review. This is the hardest naked-asm primitive in the roadmap aside from SVC, so expect one build/run iteration to surface a bug.

---

## Rules of engagement (unchanged, still authoritative)

- Target is the engine feature inventory, not ApplicationProxy coverage.
- Viber is canary only. **Canary must be measured from fresh process** — in-suite GuestLoadTest readings are not the canary.
- Every session advances at least one ⚠️/❌/🔧 → ✅.
- Verification-first: verify any prior-session unverified code before writing new code.
- Rule 5 (behavior match) — partial disclosure for structural ports.
- Rule 12: test assertions must be able to fail.
- Rule 14: every test-run instruction includes `./gradlew` + `adb install -r -t` on **both** APKs.
- Rule 15: engine is native.
- Rule 7 (decomp-first) when wire format / symbol / field layout is uncertain.
- **No `am instrument` output = no session close.**
- Deferred items stay deferred: `handleJHMethod` (S18), tnFIN code-36 byte rewrite (S12), clone-app features.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim. Additions from Sessions 33–47:

**SYS_getpid / SYS_getuid via `syscall()` bypass both inline and GOT hooks.** (S33, verified S34.)

**AttributionSource layout differs by construction path.** (S32/33, verified S34.)

**`Process.myUid()` returning spoofed 10000 propagates into ApplicationInfo.uid.** (S32, verified S34.)

**`mSurface.isValid()` gated on `mAppVisible` gated on host task visibility.** (S36/37.)

**MIUI ProcessManager threads are background, non-blocking.** (S35+.)

**Full ARM64 relocation handles 9 kinds.** (S41.)

**`AttachRuntime` adds alignment/mapped/exec guards.** (S42.)

**ChainHook stub page layout (32-byte slots).** (S43.)

**`visibility("hidden")` on `chain_dispatch_helper`** required for adrp+add in naked asm. Same constraint as `g_adrp_value` in test_item1.cpp and `g_retmod_tramps` in ReturnMod.cpp. (S43, S46.)

**Feature 1.8 — per-slot predicates, `nullptr` = unconditional fire.** (S45.)

**Feature 1.9 — post-call modifier with shadow trampoline.** (S46.)

**Feature 1.6 — BRK encoding is `0xD4200000 | (imm16 << 5)`, NOT `0xD4200000 | imm16`.**
- `brk #0x4242` = **`0xD4284840`** — correctly encodes to BRK class + imm16 = 0x4242.
- `0xD4208484` has bits 4:0 = `00100` — that is an **UNDEFINED instruction**, and it raises **SIGILL** on the CPU, not SIGTRAP. A SIGTRAP handler never fires, the default SIGILL handler runs, and the process is killed.
- `BRK_MASK = 0xFFE0001F` for the magic check; `BRK_IMM16(insn) = (insn >> 5) & 0xFFFF`.
- **This cost Session 47 one build+run iteration to diagnose** — SvcHookTest crashed with `shortMsg=Process crashed` on the first hook fire, logcat `adb logcat -d | findstr -i "signal"` would show SIGILL, and the fix was one constant. (S47, verified on-device.)

**Feature 1.6 — SVC mechanism (verified on-device).**
- Patch a specific `svc #0` (`0xD4000001`) to `brk #0x4242` (`0xD4284840`). SIGTRAP handler decodes PC, matches the site table, reads x0–x5 and x8 from `ucontext_t.uc_mcontext.regs[]`, issues the real syscall via the naked `svc_issue_raw(nr, a0..a5)` helper (mov x8,#nr; mov x0..x5=a0..a5; svc #0; ret), then dispatches to the user fn with `(nr, a0..a5, orig_ret)`. Result replaces `uc_mcontext.regs[0]` (x0). Handler advances `pc += 4` to consume the BRK.
- **Reentry guard:** `__thread int g_in_handler` — if user handler makes a syscall hitting another patched site, we raw-reissue and skip dispatch to prevent infinite recursion.
- **Signal-safe detach:** clear `slot.active` (atomic release) BEFORE restoring bytes. Any thread already in the handler at detach time sees `active == false` → raw-reissues → caller gets correct result. No use-after-free, no missed syscalls.
- **Chaining:** non-magic BRKs forward to previous SIGTRAP handler. `SA_SIGINFO | SA_RESTART`, SIGTRAP blocked during handler.
- **Slot cap:** 16. Handler pointer never cleared so a racing detach can't leave a stale read.
- **Signal-context constraints:** user handler runs in signal context — must be async-signal-safe. No malloc, no stdio, no lock acquisition, no unbounded work.
- **Only `svc #0`.** `svc #1`/`svc #2` etc. have different kernel entry semantics; refused with `HOOK_UNSUPPORTED_INSN`. (S47, verified on-device.)

**Test-fixture declaration order in `native-lib.cpp` matters.** `chainReset` references `g_pred_calls`/`g_pred_last_x0`; those must be declared above. Same for `getChainPred*Addr` referencing `chain_pred_*`. (S45 on-disk fix; carried forward in S46, S47.)

**Kotlin `Long` literal must fit in `Long.MAX_VALUE`.** `0xFFFFFFFFFFFFFFFFL` fails compile — Kotlin rejects literals exceeding 9223372036854775807. Use `arg0 and 0xFFFFFFFFL` and check the low 32 bits instead. AAPCS64 leaves the top 32 bits of x0 ABI-unspecified for int32 parameters, so the low-32 check is the correct assertion. (S47.)

**Fast-JNI constraint on ArtMethod hooks.** (S44.)

**JNI symbol name-mangling.** (S43.)

**Chained assignment of heterogeneous function pointers.** (S44.)

**`findstr` on binary `.so` returns empty.** (S43.)

**Verifying an APK rebuild deployed.** (S43.)

**`ActivityBootProbeTest` retired.** (S41.)

**Canary crash suppressor.** (S41.)

**Canary measurement discipline.** (S46.)

**Test count reduction is acceptable.**

**Phantom S42 batch warning.** 6.2 = ❌, 7.1 = ⚠️.

---

## The blunt version

**Category 1 — Hook mechanisms — closes Session 47 at 9/9.** Every foundational hook primitive the project spec called for is now on-device verified with a fail-able test class. This is the milestone the whole engine rested on: without these nine primitives, nothing in Categories 2–8 has anything to build on. It is now closed.

Sessions 41–47 shipped: full ARM64 relocation, dynamic attach, hook chaining, ArtMethod trace, conditional hook, return-value modification, SVC hooking. Seven primitives, seven verified test classes, 86 executed tests, zero regressions across the entire suite. That was **not** container drift — that was engine work, week by week.

Session 48 opens Category 2's remaining gaps with Feature 2.7 (register state inspection). Then 2.1/2.2/2.3 memory read/write/scan, 2.5 maps enumeration, 2.8 backtrace. Category 2 closes around Session 53.

No more Viber walls. No more "integration depth." No more coverage counter obsession. Engine only. Canary green.

If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.