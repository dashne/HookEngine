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


# PROJECT STATE

Last updated: **Session 45 close (2026-10-26)**. Supersedes Session-40-close version. Sessions 41–45 have landed on disk and are on-device verified (74 tests executed, 0 failed at Session 45 close). This document reflects that state.

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

Sessions 41–45 resumed engine work:
- **S41** verified Feature 1.1 — full ARM64 relocation (9 kinds).
- **S42** shipped Feature 1.5 — dynamic runtime attach by computed address.
- **S43** shipped Feature 1.7 — hook chaining (ordered multi-handler).
- **S44** shipped Feature 1.4 — ArtMethod entry/exit trace.
- **S45** shipped Feature 1.8 — conditional hook (per-handler predicate gating).

Session 46 target: Feature 1.9 — return-value modification, closing Category 1 (except 1.6).

---

## Current verified state

**Engine primitives (verified on-device, all green at Session 45 close):**
- Inline hook: works for all 9 ARM64 instruction kinds — `NORMAL`, `B`, `BL`, `ADR`, `ADRP`, `B.cond`, `CBZ/CBNZ`, `TBZ/TBNZ`, `LDR-literal`. Verified by RelocTest (1 test, 49 assertions).
- GOT hook: 26 sites, works.
- ArtMethod hook: works (native methods, non-fast-JNI only).
- ArtMethod entry/exit trace: works on framework natives (`Binder.getCallingUid`, `Binder.getCallingPid`). Verified by ArtMethodTraceTest (5 tests).
- Hook chaining: works. Dispatcher runs N handlers in registration order; ordered, all fire. Verified by ChainHookTest (6 tests).
- Conditional hook: works. Predicate gates whether the paired handler fires; false → handler skipped, original still returns real value. Per-handler granularity across different handlers on same target. Verified by ConditionalHookTest (6 tests).
- Dynamic runtime attach: works. `AttachRuntime` adds alignment/mapped/exec guards on top of the raw path. Verified by DynamicAttachTest (6 tests).
- Hidden-API bypass: works via thread-attached reflection.
- `IPCThreadState::transact` inline hook: works.
- AIDL prelude rewrite: works on 4 interfaces.
- `nativeCNVM`: decomp-verified, 4 tests green.
- `nativeInit` (structural): verified on-device Session 38 — `so=43 dex=14 rc=0` via `NativeInitTest`.

**Container / canary (verified on-device):**
- Viber boots: `outcome=booted-viber-order`, providers 14/15 (MlKit noise), JNI engine survives 60s with background IPC.
- SplashActivity: instantiated, `onCreate`/`onStart`/`onResume` all run.
- WMS accepts token + allocates Surface when host task visible (`mSurface=Surface(isValid=true)`, Session 36).
- Content frame empty at 250 ms (Session 37) — splash state, not a wall.

**Canary function:** if `GuestLoadTest` returns `outcome != "?"` and no new FATAL, engine primitives still work in a real integration scenario. If the canary breaks, an engine primitive broke. That is the canary's only job.

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
| 1.6 | SVC instruction hooking (bypass libc) | ❌ |
| 1.7 | Hook chaining (ordered multi-handler) | ✅ |
| 1.8 | Conditional hook (register inspection at site) | ✅ |
| 1.9 | Return-value modification (post-call trampoline) | ❌ → **S46 target** |

**Category 1: 7/9 complete.** 1.6, 1.9 remaining. Category closes after Session 46 (1.9) once 1.6 also lands.

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
| 6.2 | Per-guest tags | ❌ (phantom S42 batch — never shipped; see Hard-won facts) |
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
| 7.1 | ELF symbol resolution (unified API) | ⚠️ (scattered ad-hoc implementations: `elf_hooker.cpp`, `HiddenApi.cpp`, `BinderProbe.cpp`, `CppParcelRewriter.cpp`; unified `FindExportAnyLib` never shipped) |
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
| 8.3 | Runtime hook inspection | ✅ (via `RegistrySnapshot` in `HookRegistry.h` / `HookRegistry.cpp`) |
| 8.4 | Regression test suite | ✅ |
| 8.5 | Reloc test suite | ✅ |
| 8.6 | Anti-detection test suite | ✅ |
| 8.7 | Profiling (CPU / memory per hook) | ❌ |

### Totals

- **✅ verified complete:** ~29 features
- **⚠️ partial:** ~8 features
- **❌ not started:** ~24 features

**Engine completion requires ~30 more sessions** (Categories 1–8 fully ✅). **Not** dependent on Viber state, **not** dependent on ApplicationProxy coverage, **not** dependent on Activity UI.

---

## Engine roadmap — Session 46 to Session 75

### Phase A — Foundation verified (Sessions 41–44) — **CLOSED**

| Session | Feature | Status |
|---|---|---|
| 41 | 1.1 Inline hook 9 kinds | ✅ |
| 42 | 1.5 Dynamic attach | ✅ |
| 43 | 1.7 Hook chaining | ✅ |
| 44 | 1.4 ArtMethod trace | ✅ |

### Phase B — Hook mechanism depth (Sessions 45–48)

| Session | Feature | Status / Exit |
|---|---|---|
| 45 | 1.8 Conditional hook | ✅ `ConditionalHookTest` (6 tests) |
| **46** | **1.9 Return-value modification** | **next — `ReturnModTest`** |
| 47 | 2.7 Register state inspection | `RegInspectTest` |
| 48 | 2.8 Backtrace from hook site | `BacktraceTest` |

After S48: **Category 1 (except 1.6) complete. Category 2 (except 2.1–2.3) complete.**

### Phase C — Memory + kernel boundary (Sessions 49–53)

| Session | Feature | Exit |
|---|---|---|
| 49 | 1.6 SVC instruction hooking | `SvcHookTest` |
| 50 | 2.1 / 2.2 / 2.3 Memory read / write / scan | `MemoryOpsTest` |
| 51 | 2.5 Full maps enumeration | `MapsEnumTest` |
| 52 | 3.4 ServiceManager lookup interception | `ServiceManagerTest` |
| 53 | 3.6 Signal handling override | `SignalOverrideTest` |

After S53: **Category 1 complete. Category 2 complete. Category 3 (except 3.7) complete.**

### Phase D — Class loading + symbol depth (Sessions 54–58)

| Session | Feature | Exit |
|---|---|---|
| 54 | 4.2 Native `nativeFNLN` (Rule 15 debt closure) | `NativeFnlnTest` |
| 55 | 4.3 Dynamic DEX loading | `DexLoadTest` |
| 56 | 4.4 DEX merging at runtime | `DexMergeTest` |
| 57 | 7.6 PLT stub resolution | `PltResolveTest` |
| 58 | 3.7 Native ServiceManager addition | `SmAddTest` |

After S58: **Category 3 complete. Category 4 complete. Category 7 complete.**

### Phase E — Anti-detection completion (Sessions 59–64)

| Session | Feature | Exit |
|---|---|---|
| 59 | 5.10 ADB detect blocking | `AdbDetectTest` |
| 60 | 5.11 Debugger detect (JDWP + full status) | `DebuggerDetectTest` |
| 61 | 5.12 Emulator detect blocking | `EmulatorDetectTest` |
| 62 | 5.13 Test-keys / custom build detect | `BuildPropTest` |
| 63 | 5.14 /proc/net/tcp blocking | `NetTcpTest` |
| 64 | 5.15 /proc/self/cmdline + 5.16 syscall/stack filter | `CmdlineStackTest` |

After S64: **Category 5 complete.**

### Phase F — Polish + close (Sessions 65–75)

| Session | Feature | Exit |
|---|---|---|
| 65 | 6.8 Signal-safe uninstall | `SignalSafeUninstallTest` |
| 66 | 8.7 Profiling (CPU / memory per hook) | `HookProfileTest` |
| 67–73 | Address any ⚠️ partials surfaced by prior phases | Depends on outcomes |
| 74 | Full engine regression run + `PROJECT_STATE.md` final close | All suites + canary green |
| 75 | Reserved for any decomp-blocked item that survives to end | Depends |

**Total: Sessions 46–75, ~30 sessions.** Coverage of ApplicationProxy natives is **not part of the exit criteria**. The engine's exit criteria are Categories 1–8 fully ✅.

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load. Nothing else.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal:
- `booted-viber-order` + no new FATAL + providers ≥14 = canary green
- Any new FATAL, any boot regression, any provider count drop = canary red = an engine primitive broke

**Canary state at Session 45 close:** green. `outcome=booted-viber-order`, `onCreated=true`, providers 14/15 (MlKit noise), no crash in 60s, background IPC observed.

**What the canary does NOT test:**
- Viber UI visibility (Wall C4/C5/C6 outcome was "surface valid, content empty")
- Viber functionality (network, register, message)
- Activity task registration
- Any specific Viber reference method

**Canary failure protocol:** canary goes red → identify which engine primitive broke → fix that primitive → canary green → continue. Do not add canary-specific features. Do not chase Viber walls.

**Wall C3, C4, C5, C6 status:** all closed or accepted as *canary state facts*, not as targets. Wall names retired — no new Wall D/E/F.

---

## Test suite state (as of Session 45 close)

**Executed:** 74 · **Registered:** 78 · **Ignored:** 4 (1 × `ActivityBootProbeTest` + 3 × `AntiDetectionE2ETest` connect tests) · **Failed:** 0

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
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several | non-gating |

**Test count history:**
- S40 close: 51 executed
- S41 close: 51 executed (rewrote internals — no new class)
- S42 close: 57 executed (+6 DynamicAttachTest)
- S43 close: 63 executed (+6 ChainHookTest)
- S44 close: 68 executed (+5 ArtMethodTraceTest)
- S45 close: **74 executed** (+6 ConditionalHookTest)
- S46 target: 80 executed (+6 ReturnModTest)

---

## Session 46 first actions (authoritative)

1. **Step 0** — confirm Session 45 commit landed (`git log --oneline -5` should show the S45 commit at top).
2. **Step 1** — write Feature 1.9 (return-value modification). New `ReturnMod.h` / `ReturnMod.cpp` + `ReturnModTest.kt`. Surface: `AttachRetMod(target, modifier_fn, out_trampoline)` where modifier is `uint64_t (*)(const uint64_t* x0_x7, uint64_t orig_ret)`. Trampoline must be able to route through a post-call site that captures x0 (return value), calls the modifier, and returns the (possibly modified) x0.
3. **Step 2** — build + install (Rule 14).
4. **Step 3** — `am instrument` ReturnModTest. Expect `OK (6 tests)`.
5. **Step 4** — full suite. Expect `OK (80 tests)`.
6. **Step 5** — canary. Expect `outcome=booted-viber-order`, providers ≥14, no new FATAL.
7. **Step 6** — commit, mark Feature 1.9 → ✅, close Category 1 (except 1.6).

---

## Rules of engagement (unchanged, still authoritative)

- Target is the engine feature inventory, not ApplicationProxy coverage.
- Viber is canary only. Its state is diagnostic, not a milestone. No new Viber walls.
- Every session advances at least one ⚠️/❌/🔧 → ✅.
- Verification-first: if a prior session left unverified code on disk, verify before writing new code.
- Rule 5 (behavior match) — partial disclosure for structural ports: `Rule 5 partial: <reason>` in commit.
- Rule 12: test assertions must be able to fail.
- Rule 14: every test-run instruction includes `./gradlew` + `adb install -r -t` on **both** APKs.
- Rule 15: engine is native. Kotlin/Java in `app/src/main` limited to JNI surface, `DualApplication`, and pre-port Kotlin proxies.
- Rule 7 (decomp-first) when wire format / symbol / field layout is uncertain.
- **No `am instrument` output = no session close.** Session is not done until `run finished: N tests, 0 failed` appears in the paste.
- Deferred items stay deferred: `handleJHMethod` (S18), tnFIN code-36 byte rewrite (S12), clone-app features.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim (inline hook ROM rules, Parcel layout, AIDL wire format, hidden-API bypass via AttachCurrentThread, Viber Wall A/B/C findings, MLKit noise, canary workflow). Not repeated here for brevity.

Additions from Sessions 33–45:

**SYS_getpid / SYS_getuid via `syscall()` bypass both inline and GOT hooks.** `getpid()` from libhookengine returns spoofed `g_virtualPid` (10000) because the `Fake_getpid` inline hook fires for every caller including libhookengine. Same for `getuid()` via getuid/geteuid hooks. Any engine-internal code that needs real kernel values must use `syscall(SYS_getpid)` / `syscall(SYS_getuid)`. (Session 33, verified Session 34.)

**AttributionSource layout differs by construction path.** Own-pkg AttributionSource (`[leading][pid][uid][tagLen][pkgLen][pkg]`), Viber-context AttributionSource (`[leading][uid][pid][tagLen][pkgLen][pkg]`). The rewriter resolves layout by pkg and writes to the correct slot per shape. Not AOSP behavior; not documented. (Session 32/33, verified Session 34.)

**`Process.myUid()` returning spoofed 10000 propagates into ApplicationInfo.uid**, which propagates into every AttributionSource constructed from that ApplicationInfo. `ApplicationProxy.cpp`'s `GetMyUid` must use `syscall(SYS_getuid)`, not `Process.myUid()`. (Session 32, verified Session 34.)

**`mSurface.isValid()` is gated on `mAppVisible`** which is gated on host task visibility. Under `am instrument`, task is usually not user-visible at guest `addView` time → `mSurface.isValid=false`. Session 36's success was because prior run left the task visible. Not a fixed-state behavior; it's a state precondition. Viber canary unaffected (canary doesn't call addView).

**`Activity.attach` sets `mBase` to `ContextThemeWrapper`, not the raw `ContextImpl`.** Standard androidx AppCompatActivity theme wrapping. Expected.

**MIUI ProcessManager threads are background, non-blocking.** Thread-24/Thread-26 FATAL on `ProcessManager.addMiuiApplicationThread` does not kill the process; main thread continues. Canary green despite the background FATAL. Do not chase.

**Full ARM64 relocation handles 9 kinds.** Original 5 (`NORMAL`, `B`, `BL`, `ADR`, `ADRP`) + 4 new (`B_COND`, `CBZ_CBNZ`, `TBZ_TBNZ`, `LDR_LITERAL`). B.cond → inverted-cond skip + B target (2 insns). CBZ/CBNZ → inverted + B target (2 insns). TBZ/TBNZ → inverted + B target (2 insns). LDR literal → `stp x16,x17` + `adrp x16` + `ldr rt` + `ldp x16,x17` (4 insns), rejects rt==16/17. (Session 41, verified on-device.)

**`AttachRuntime` adds validation the raw path never had.** Refuses misaligned (addr & 3 != 0), unmapped, non-executable target page, non-executable replacement. All refusals return `HOOK_TARGET_INVALID`. "Computed address" is enforced structurally — the target is obtained *only* as a jlong from a JNI bridge; Kotlin never sees the symbol name. (Session 42, verified on-device.)

**ChainHook stub page layout (32-byte slots).** `+0 ldr x16,[pc,#0x10]` / `+4 ldr x9,[pc,#0x14]` / `+8 br x16` / `+12 nop` / `+16 dispatcher(8B)` / `+24 target_id(8B)`. The dispatcher (`chain_dispatch_naked`) saves x0-x9, x19-x30, q0-q7 to a 320-byte frame, calls `chain_dispatch_helper`, reads the trampoline back from `[sp,#304]`, restores, and `br x16`. Handlers run outside the chain mutex — prevents re-entrant deadlock if a handler calls into another chained target. (Session 43, verified on-device.)

**`visibility("hidden")` on `chain_dispatch_helper`** is required for the adrp+add in the naked asm — the PIC linker refuses `R_AARCH64_ADR_PREL_PG_HI21` against a preemptible (exported) symbol. Same pattern as `g_adrp_value` in `test_item1.cpp`. (Session 43, verified on-device.)

**Feature 1.8 adds per-slot predicates, `nullptr` = unconditional fire.** `ChainSlot.predicates[MAX_HANDLERS_PER_CHAIN]` is 1:1 paired with `handlers[]`. `ChainAttach` delegates to `ChainAttachCond(target, nullptr, handler, outTrampoline)` so the pre-1.8 API is unchanged. `ChainDetach` shifts both arrays together to preserve pairing. Dispatcher order per slot: `if (local_handlers[i] && (!local_preds[i] || local_preds[i](xregs))) local_handlers[i](xregs);`. Skipping a handler does **not** skip the orig call — the chain still falls through to the trampoline, and the caller always observes the real return value. (Session 45, verified on-device.)

**Test-fixture declaration order in `native-lib.cpp` matters.** `chainReset` references `g_pred_calls` / `g_pred_last_x0`; those atomics must be declared **above** `chainReset`. Same for the fixture functions — `getChainPredAcceptAllAddr` etc. reference `chain_pred_accept_all` and need it declared above. The build failure from Session 45 (undeclared identifiers at line 995) was the atomics being placed below `chainReset`. Fix: declare the entire Feature 1.8 fixture block (`extern "C" HookResult ChainAttachCond(...)` + atomics + predicate functions) *before* `chain_handler_1`, and only the JNI bridges after `chainReset`. (Session 45, on-disk fix.)

**Fast-JNI constraint on ArtMethod hooks (biggest surprise of S44).** `env->FromReflectedMethod(method)` returns a **fast-JNI dispatch table index** (small int < 0x1000) for app-defined native methods (external fun in Kotlin object, resolved via dlsym) — not a real `ArtMethod*`. `JavaHookInstall` correctly refuses these (validates `data_ >= 0x1000`). Framework natives like `Binder.getCallingUid` are compiled eagerly to a fast stub in `libart.so` — `FromReflectedMethod` returns a real pointer and the hook works. **Implication:** if you need to trace an app-defined native method, ArtMethod hook won't work; use call-site patching, SVC-level hook, or hook `ArtMethod::Invoke` itself. **Test fixtures for ArtMethodTrace must use framework natives** (`Binder.getCallingUid`, `Binder.getCallingPid`) — not custom `external fun` natives. (Session 44, verified on-device.)

**JNI symbol name-mangling.** Method name `foo_bar` in Kotlin maps to JNI C symbol `Java_..._foo_1bar` — not `Java_..._foo_bar`. The `_` becomes `_1`. Any JNI function whose Java method name contains `_` needs the mangled name in its extern. This bit us on `chainLastX0_1` → `chainLastX0_11`. (Session 43.)

**Chained assignment of heterogeneous function pointers is an error.** `s.onEntry = s.onExit = nullptr;` — error if `onEntry` and `onExit` are different fn-pointer types. Split into two lines. Applies to any struct with heterogeneous function-pointer members. (Session 44.)

**`findstr` on binary `.so` returns empty by default.** `findstr` silently skips files with embedded nulls. To verify a symbol exists in a `.so` on disk, use `llvm-nm -D <file> | findstr <symbol>` (NDK ships `llvm-nm.exe`). To verify on device, `adb shell "run-as <pkg> strings <path>" | findstr <symbol>` — the `|` is **outside** the quotes, so the pipe is Windows-side and `findstr` runs on host. (Session 43.)

**Verifying an APK rebuild actually deployed.** After `adb install`, the on-device `.so` must be re-verified: `adb shell "run-as com.ims134.hookengine strings /data/app/com.ims134.hookengine-*/lib/arm64/libhookengine.so" | findstr <new_symbol>`. If empty → install didn't take, or build didn't recompile. `./gradlew :app:clean` is mandatory when a previous compile failed.

**`ActivityBootProbeTest` cost / retirement.** Runs 3× process-crash retries per suite invocation (MIUI `InterceptorProxy.addMiuiApplicationThread` + Viber `registerReceiver` AMS rejections on background threads). ~60-75s of pure noise. Retired with `@Ignore` (Session 41); its only Rule 12-weak assertion (`outcomes.isNotEmpty()`) is documented as the retirement reason. (Session 41.)

**Canary crash suppressor.** `GuestLoadTest` installs a targeted `Thread.setDefaultUncaughtExceptionHandler` in `@Before` that swallows **only** `SecurityException` whose message contains `com.viber.voip` AND matches one of: `addMiuiApplicationThread`, `registerReceiverWithFeature`, `Unknown calling package name`, `is not running in process`. All other throwables propagate → default handler → process kill. Rule 12 preserved. Diagnostics logged at `DualCore.Guest:W`. (Session 41.)

**Test count reduction is acceptable.** Session 44 reduced ArtMethodTraceTest from 6 tests to 5 (removed one that was structurally identical to another). Test count discipline is subordinate to Rule 12 (each test must be able to fail meaningfully). Baseline 74 executed at Session 45 close; Session 46 target 80.

**Phantom S42 batch warning.** The Session 42 handoff claimed to have shipped hook registry tags + `ElfSymbols.h`/`ElfSymbols.cpp`. **None of that exists on disk.** `HookRegistry.h` has no `guest_tag` field. There is no `ElfSymbols.h` / `ElfSymbols.cpp`. Those were never written. Status column reflects reality: 6.2 = ❌, 7.1 = ⚠️. If a future handoff claims a batch is on disk, verify by grepping for the named symbol before trusting it.

---

## The blunt version

The engine is real and it is growing again. Sessions 41–45 shipped five verified primitives — full ARM64 relocation, dynamic attach, hook chaining, ArtMethod trace, conditional hook — all backed by test classes that run on-device and can fail. That was not container work. That was engine work.

Session 46 ships Feature 1.9 (return-value modification) and closes Category 1 (except 1.6). From there, Categories 2–8 all have named sessions with named exit tests. The path to Session 75 is unambiguous.

No more Viber walls. No more "integration depth." No more coverage counter obsession. Engine only. Canary green.

If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.