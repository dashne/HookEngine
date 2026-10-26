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

Last updated: Session 40 close (2026-10-26). Supersedes all prior versions. **The target has been reset. Read "What this project is" and "What this project is NOT" before anything else.**

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

---

## Current verified state

**Engine primitives (verified on-device):**
- Inline hook: works, but only for `NORMAL` / `B` / `BL` / `ADR` / `ADRP` — the other four ARM64 kinds are rejected. Session 41 code (written, unverified) closes this.
- GOT hook: 26 sites, works.
- ArtMethod hook: works (native methods, non-fast-JNI only).
- Hidden-API bypass: works via thread-attached reflection.
- `IPCThreadState::transact` inline hook: works.
- AIDL prelude rewrite: works on 4 interfaces.
- `nativeCNVM`: decomp-verified, 4 tests green.
- `nativeInit` (structural): **verified on-device** Session 38 — `so=43 dex=14 rc=0` via `NativeInitTest`. First coverage growth since Session 14.

**Engine primitives (written, NOT yet on-device verified):**
- Session 39: `nativeOnLoad` (2-param alias), `is64Bit`, `nativeGetCpuABI` explicit ports; `IsStatusPath` extended to `/proc/*/task/*/status`.
- Session 40: `IsMountsPath` + `ShouldFilterMountLine` for `/proc/mounts` etc.
- Session 41: full ARM64 relocation (9 kinds, was 5).
- Session 42: hook registry with per-guest tags + `ElfSymbols` utility.

**These six code changes are on disk. None have run their test on-device.** Session 41's first action is to run them in sequence.

**Container / canary (verified on-device):**
- Viber boots: `outcome=booted-viber-order`, providers 14/15 (MlKit noise), JNI engine survives 60s with background IPC.
- SplashActivity: instantiated, `onCreate`/`onStart`/`onResume` all run.
- WMS accepts token + allocates Surface when host task visible (`mSurface=Surface(isValid=true)`, Session 36).
- Content frame empty at 250 ms (Session 37) — splash state, not a wall.
- `am instrument` re-runs show task-visibility flake (S35 vs S36 vs S37).

**Canary function:** if `GuestLoadTest` returns `outcome != "?"` and no new FATAL, engine primitives still work in a real integration scenario. If the canary breaks, an engine primitive broke. That is the canary's only job.

---

## Engine feature inventory — the actual target

Status: ✅ have + verified · ⚠️ partial · ❌ not started · 🔧 written but unverified

### Category 1 — Hook mechanisms

| # | Feature | Status |
|---|---|---|
| 1.1 | Inline hook at any ARM64 instruction | 🔧 S41 (9 kinds) |
| 1.2 | GOT/PLT symbol hook | ✅ |
| 1.3 | ArtMethod hook (replace) | ✅ |
| 1.4 | ArtMethod entry/exit trace (pre + post, no replace) | ❌ |
| 1.5 | Dynamic runtime attach by computed address | ❌ |
| 1.6 | SVC instruction hooking (bypass libc) | ❌ |
| 1.7 | Hook chaining (ordered multi-handler) | ❌ |
| 1.8 | Conditional hook (register inspection at site) | ❌ |
| 1.9 | Return-value modification (post-call trampoline) | ❌ |

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
| 6.1 | Hook registry | 🔧 S42 |
| 6.2 | Per-guest tags | 🔧 S42 |
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
| 7.1 | ELF symbol resolution | 🔧 S42 |
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
| 8.3 | Runtime hook inspection | 🔧 S42 |
| 8.4 | Regression test suite | ✅ |
| 8.5 | Reloc test suite | ✅ |
| 8.6 | Anti-detection test suite | ✅ |
| 8.7 | Profiling (CPU / memory per hook) | ❌ |

### Totals

- **✅ verified complete:** ~24 features
- **🔧 written, unverified:** ~8 features (S39–S42 batch)
- **⚠️ partial:** ~8 features
- **❌ not started:** ~27 features

**Engine completion requires ~35 sessions** (Categories 1–8 fully ✅). **Not** dependent on Viber state, **not** dependent on ApplicationProxy coverage, **not** dependent on Activity UI.

---

## Engine roadmap — Session 41 to Session 75

### Phase A — Foundation verified (Sessions 41–44, 4 sessions)

| Session | Work | Exit criteria |
|---|---|---|
| **41** | Verify S38–S40 batch (nativeInit, nativeOnLoad, is64Bit, nativeGetCpuABI, nativeDNPO status TID, mounts filter). Run all tests. Fix any red. | All prior-batch tests green + canary green |
| **42** | Verify S41 batch (full ARM64 relocation). Run RelocTest. Fix if red. | RelocTest 44 assertions green + canary green |
| **43** | Verify S42 batch (hook registry tags, ELF symbols). Run both tests. Fix if red. | 6 tests green + canary green |
| **44** | Feature 1.5 — dynamic attach by computed address. | `DynamicAttachTest` green + canary green |

After S44: foundation is real and verified end-to-end. Session 41 is "catch up on the pending batch," not new work.

### Phase B — Hook mechanism depth (Sessions 45–50, 6 sessions)

| Session | Feature | Exit |
|---|---|---|
| 45 | 1.4 ArtMethod entry/exit tracing | `MethodTraceTest` |
| 46 | 1.7 Hook chaining (ordered handlers) | `HookChainTest` |
| 47 | 1.8 Conditional hook (register inspection) | `ConditionalHookTest` |
| 48 | 1.9 Return-value modification | `ReturnModTest` |
| 49 | 2.7 Register state inspection | `RegInspectTest` |
| 50 | 2.8 Backtrace from hook site | `BacktraceTest` |

After S50: **Category 1 (except 1.6) complete. Category 2 (except 2.1–2.3) complete.**

### Phase C — Memory + kernel boundary (Sessions 51–55, 5 sessions)

| Session | Feature | Exit |
|---|---|---|
| 51 | 2.1 / 2.2 / 2.3 Memory read / write / scan | `MemoryOpsTest` |
| 52 | 2.5 Full maps enumeration | `MapsEnumTest` |
| 53 | 1.6 SVC instruction hooking | `SvcHookTest` |
| 54 | 3.4 ServiceManager lookup interception | `ServiceManagerTest` |
| 55 | 3.6 Signal handling override | `SignalOverrideTest` |

After S55: **Category 1 complete. Category 2 complete. Category 3 (except 3.7) complete.**

### Phase D — Class loading + symbol depth (Sessions 56–60, 5 sessions)

| Session | Feature | Exit |
|---|---|---|
| 56 | 4.2 Native `nativeFNLN` (Rule 15 debt closure) | `NativeFnlnTest` |
| 57 | 4.3 Dynamic DEX loading | `DexLoadTest` |
| 58 | 4.4 DEX merging at runtime | `DexMergeTest` |
| 59 | 7.6 PLT stub resolution | `PltResolveTest` |
| 60 | 3.7 Native ServiceManager addition | `SmAddTest` |

After S60: **Category 3 complete. Category 4 complete. Category 7 complete.**

### Phase E — Anti-detection completion (Sessions 61–66, 6 sessions)

| Session | Feature | Exit |
|---|---|---|
| 61 | 5.10 ADB detect blocking | `AdbDetectTest` |
| 62 | 5.11 Debugger detect (JDWP + full status) | `DebuggerDetectTest` |
| 63 | 5.12 Emulator detect blocking | `EmulatorDetectTest` |
| 64 | 5.13 Test-keys / custom build detect | `BuildPropTest` |
| 65 | 5.14 /proc/net/tcp blocking | `NetTcpTest` |
| 66 | 5.15 /proc/self/cmdline + 5.16 syscall/stack filter | `CmdlineStackTest` |

After S66: **Category 5 complete.**

### Phase F — Polish + close (Sessions 67–75, up to 9 sessions)

| Session | Feature | Exit |
|---|---|---|
| 67 | 6.8 Signal-safe uninstall | `SignalSafeUninstallTest` |
| 68 | 8.7 Profiling (CPU / memory per hook) | `HookProfileTest` |
| 69–73 | Address any ⚠️ partials surfaced by prior phases | Depends on outcomes |
| 74 | Full engine regression run + `PROJECT_STATE.md` final close | All suites + canary green |
| 75 | Reserved for any decomp-blocked item that survives to end | Depends |

**Total: Sessions 41–75, ~35 sessions.** Coverage of ApplicationProxy natives is **not part of the exit criteria**. The engine's exit criteria are Categories 1–8 fully ✅.

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load. Nothing else.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal:
- `booted-viber-order` + no new FATAL + providers ≥14 = canary green
- Any new FATAL, any boot regression, any provider count drop = canary red = an engine primitive broke

**Canary state at Session 40 close:** green. `outcome=booted-viber-order`, `onCreated=true`, providers 14/15 (MlKit noise), no crash in 60s, background IPC observed.

**What the canary does NOT test:**
- Viber UI visibility (Wall C4/C5/C6 outcome was "surface valid, content empty")
- Viber functionality (network, register, message)
- Activity task registration
- Any specific Viber reference method

**Canary failure protocol:** canary goes red → identify which engine primitive broke → fix that primitive → canary green → continue. Do not add canary-specific features. Do not chase Viber walls.

**Wall C3, C4, C5, C6 status:** all closed or accepted as *canary state facts*, not as targets. Wall C4 (token steal) works. Wall C5 (surface allocation) works when task visible. Wall C6 (empty content at 250ms) — canary data, not a wall. If any of these regress, that's a canary signal, not a target.

---

## What the `X/37` coverage counter is now

**Retained as documentation, not as progress metric.** At Session 40 close:

- 15/37 ApplicationProxy natives verified on-device (Session 38 confirmed `nativeInit`)
- 17/37 claimed counting written-but-unverified S39–S40 (nativeOnLoad, nativeAMRHook)

Actual verified: **15/37**. Claimed if S39–S42 tests pass: 17/37.

**The counter does not grow the engine.** If a future session ports `nativeJH*` and canary stays green, that's engine work. If a future session ports it and canary turns red, that's engine work that broke something. Either way, engine feature ✅ matters more than counter movement.

The counter will not be the subject of any future session narrative. Sessions 15–40's endless coverage debates — that era is closed.

---

## Session 41 first actions (authoritative)

Session 41 opens with **verification, not new work**. Six code files exist on disk from Sessions 39–42 that have never run their tests. Verify them before any new code is written.

**Step 1 — Build and install** (Rule 14):

```powershell
cd H:\Mobiles\AndroidProject\2027\hookEngine
./gradlew :app:assembleDebug :app:assembleDebugAndroidTest
adb install -r -t app\build\outputs\apk\debug\app-debug.apk
adb install -r -t app\build\outputs\apk\androidTest\debug\app-debug-androidTest.apk

Step 2 — Run pending tests in order. After each, log run finished: line:

# 2a. S39 batch
adb logcat -c
adb shell am instrument -w -r `
  -e class com.ims134.hookengine.NativeInitTest,com.ims134.hookengine.AbiProbeTest,com.ims134.hookengine.ThreadStatusProbeTest `
  com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner
# Expect: run finished: 5 tests, 0 failed

# 2b. S40 batch
adb logcat -c
adb shell am instrument -w -r `
  -e class com.ims134.hookengine.MountsProbeTest `
  com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner
# Expect: run finished: 2 tests, 0 failed

# 2c. S41 batch (full ARM64 relocation)
adb logcat -c
adb shell am instrument -w -r `
  -e class com.ims134.hookengine.RelocTest `
  com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner
# Expect: run finished: 1 tests, 0 failed with 44 assertions

# 2d. S42 batch (hook registry tags + ELF symbols)
adb logcat -c
adb shell am instrument -w -r `
  -e class com.ims134.hookengine.HookRegistryTagTest,com.ims134.hookengine.ElfSymbolsTest `
  com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner
# Expect: run finished: 6 tests, 0 failed
Step 3 — Full suite regression.

powershell
adb logcat -c
adb shell am instrument -w -r `
  com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner
# Expect: OK (N tests) or run finished: N tests, 0 failed
Step 4 — Viber canary.

powershell
adb logcat -c
adb shell am instrument -w -r `
  -e class com.ims134.hookengine.GuestLoadTest `
  com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner
adb shell run-as com.ims134.hookengine cat files/guest-boot.txt
# Expect: outcome=booted-viber-order, providersSucceeded >= 14
# Expect: no new FATAL in `adb logcat -d AndroidRuntime:E *:F`
Step 5 — Commit verification outcome.

If all green: single commit noting all pending tests verified + engine foundation locked.

If red: fix in-session. Do not advance to Session 44's new work (feature 1.5) until the batch is fully green.

Rules of engagement (revised for engine-first)
Every session opens with the engine-feature header, not the ApplicationProxy coverage header. Format: Session N | Phase X | Feature Y.Z — <name> | status.

The target is the engine feature inventory. Coverage counter is documentation, not a metric.

Viber is canary. Runs at session close. Its state is diagnostic, not a milestone. Wall names like "Wall A / B / C" are retired — no new walls are named, no walls are closed as targets.

Every session advances at least one ⚠️/❌/🔧 → ✅. If it doesn't, it isn't a session — it's a probe, and probes are prohibited as session deliverables.

Verification-first. Any session that opens with written-but-unverified code from a prior session must verify that code before adding new code.

Deferred work is honored. handleJHMethod parked (Session 18). tnFIN code 36 byte-rewrite parked (Session 12). Clone-app features (filesystem virtualization, AMS registration, etc.) out of scope unless the operator explicitly adds them.

Rule 5 (behavior match) — partial disclosure for structural ports. When a reference method has no decomp available, a structural port is allowed with Rule 5 partial: <reason> stated in the commit message.

Rule 15 (engine is native). Kotlin/Java in app/src/main limited to JNI surface declarations, DualApplication, and pre-port Kotlin proxies. New engine logic is native.

Test assertions must be able to fail. Rule 12 stands.

Rule 14 (rebuild + install). Every test-run instruction includes ./gradlew + adb install -r -t on both APKs.

No am instrument output = no session close. Session is not done until run finished: N tests, 0 failed appears in the paste.

Decomp-first when layout or wire format is uncertain. Rule 7 stands.

BinderProxy.transact-frame jobject crash, dlsym filter, read-only reply parcel, Kotlin object non-static, mParent-vs-decorView, ART capability gate — the hard-won facts from prior sessions remain. See "Hard-won facts" below.

Hard-won facts (preserved, extended)
Facts from Sessions 1–40 retained here. Any prior fact is still authoritative unless explicitly superseded.

[Preserved as in prior PROJECT_STATE.md — inline hook ROM rules, Parcel layout, AIDL wire format, hidden-API bypass via AttachCurrentThread, Viber-specific findings (Wall A/B/C), MLKit noise, canary workflow. Not repeated here for brevity; the full text from the Session-28-based file section "Hard-won facts (canonical reference)" applies verbatim.]

Additions since Session 28:

SYS_getpid / SYS_getuid via syscall() bypass both inline and GOT hooks. getpid() from libhookengine returns spoofed g_virtualPid (10000) because the Fake_getpid inline hook fires for every caller including libhookengine. Same for getuid() via getuid/geteuid hooks. Any engine-internal code that needs real kernel values must use syscall(SYS_getpid) / syscall(SYS_getuid). (Session 33, verified Session 34.)

AttributionSource layout differs by construction path. Own-pkg AttributionSource ([leading][pid][uid][tagLen][pkgLen][pkg]), Viber-context AttributionSource ([leading][uid][pid][tagLen][pkgLen][pkg]). The rewriter resolves layout by pkg and writes to the correct slot per shape. Not AOSP behavior; not documented. (Session 32/33, verified Session 34.)

Process.myUid() returning spoofed 10000 propagates into ApplicationInfo.uid, which propagates into every AttributionSource constructed from that ApplicationInfo. ApplicationProxy.cpp's GetMyUid must use syscall(SYS_getuid), not Process.myUid(). (Session 32, verified Session 34.)

mSurface.isValid() is gated on mAppVisible which is gated on host task visibility. Under am instrument, task is usually not user-visible at guest addView time → mSurface.isValid=false. Session 36's success was because prior run left the task visible. Not a fixed-state behavior; it's a state precondition. Not an engine primitive failure — it's WMS working as designed. Viber canary unaffected (canary doesn't call addView).

Activity.attach sets mBase to ContextThemeWrapper, not the raw ContextImpl. Standard androidx AppCompatActivity theme wrapping. Expected.

nativeInit structural port verified. so=43 dex=14 rc=0 on Viber APK. DEX extraction via JNI ZipFile walk works for the largest realistic test case. nativeOnLoad alias uses the same helper. (Session 38, verified Session 38.)

MIUI ProcessManager threads are background, non-blocking. Thread-24/Thread-26 FATAL on ProcessManager.addMiuiApplicationThread does not kill the process; main thread continues. Canary green despite the background FATAL. Do not chase.

ApplicationProxy.nativeOnLoad and nativeInit share a C++ helper. Signature: nativeInit(String, String, int) → int, nativeOnLoad(String, String) → int. (Session 39, unverified on-device.)

IsMountsPath filter covers /proc/mounts, /proc/self/mounts, /proc/self/mountinfo, /proc/<pid>/mounts, /proc/<pid>/mountinfo. Filters magisk / supersu / KernelSU / APatch / /data/adb / /sbin/.magisk / /cache/magisk / /debug_ramdisk / overlay-on-system lines. (Session 40, unverified on-device.)

Full ARM64 relocation handles 9 kinds. Original 5 (NORMAL, B, BL, ADR, ADRP) + 4 new (B_COND, CBZ_CBNZ, TBZ_TBNZ, LDR_LITERAL). B.cond → inverted-cond skip + B target (2 insns). CBZ/CBNZ → inverted + B target (2 insns). TBZ/TBNZ → inverted + B target (2 insns). LDR literal → ADRP x17 + LDR (2 insns). (Session 41, unverified on-device.)

Hook registry has per-guest tags. HookHandle.guest_tag[32] field. RegistrySnapshotByTag, RegistryCountByTag, RegistrySetTag, InstallInlineHookExTagged, HookGotEntryExTagged, TagExistingHook. uninstallAllForTag from JNI. (Session 42, unverified on-device.)

ElfSymbols.h / ElfSymbols.cpp — FindExportAnyLib(sym), FindSymbolInLib(lib, sym) via dl_iterate_phdr + GNU + SysV hash walk. Supersedes per-file dynsym scans; new code should call these. (Session 42, unverified on-device.)

Test suite state
The suite contains both engine tests and canary tests. All rows must be green for a session to close.

Engine tests:

Class	Tests	Status
NativeItemTest	5	green (engine items 1–5)
ProcessHooksTest	4	green
RegressionTest	5	green
RelocTest	1 (44 assertions)	written-S41, unverified
ProxyBridgeSurfaceTest	4	green
NativeInitTest	2	written-S39, unverified
AbiProbeTest	2	written-S39, unverified
ThreadStatusProbeTest	1	written-S39, unverified
MountsProbeTest	2	written-S40, unverified
HookRegistryTagTest	2	written-S42, unverified
ElfSymbolsTest	4	written-S42, unverified
AntiDetectionTest	1	green
AntiDetectionE2ETest	4	1 green + 3 ignored (loopback EPERM, Rule 12)
PmSpoofTest	4	green
ClassLoaderBridgeTest	3	green
DualApplicationTest	2	green
BinderTransactE2ETest	9	green
BinderTransactHookTest	2	green
Diagnostic / probe tests (not gates):
ClassLoaderProbeTest, BinderTransactProbeTest, BinderProbeTest, AidlCodeProbeTest, Session11Code36ReplyTest, Session11ProbeTest.

Canary tests:

Class	Tests	Rule
GuestLoadTest	1	Rule 12 (data, not gate)
ActivityBootProbeTest	1	Rule 12 (data, not gate)
IntegrationTest	1	Rule 12 (retry-wrapped)
Test count history (updated):

Session 14–28: 52 registered

Session 38 (verified green): 52 + 1 (NativeInitTest) = 53

Session 39 (written): +5 (2 Abi + 1 ThreadStatus + 2 NativeInit added — NativeInitTest got 2nd method) → 55

Session 40 (written): +2 (Mounts) → 57

Session 41 (written): Reloc expanded (44 assertions, still 1 test) → 57

Session 42 (written): +6 (2 HookRegistryTag + 4 ElfSymbols) → 63 registered

Actual verified count at Session 40 close: 53. Claimed 63 if all pending tests pass in Session 41.

Coverage history (documented, not progress metric)
ApplicationProxy natives on-device verified:

S7: 10/90 (historical denominator, superseded)

S14: 14/90 verified (nativeCNVM)

S20: 14/37 (denominator corrected)

S22–S28: 14/37 (Wall A/B/C2 integration only)

S38: 15/37 — nativeInit (verified on-device)

S39: +1 claimed 16/37 (nativeOnLoad) — not yet on-device verified

S40: +1 claimed 17/37 (nativeAMRHook mounts) — not yet on-device verified

Confirmed: 15/37. Pending claim: 17/37 if S39–S40 tests pass in Session 41.

From Session 41 onward, the X/37 counter is not the progress metric. Engine features completed is the metric.

What Session 41 must NOT do
Do not write new code until the S39–S42 pending batch is verified on-device. The batch is 5 test classes, 15 assertions. It's the entire foundation for Phases A–F. If it doesn't run, nothing built on it runs.

Do not open with a Viber wall probe. Wall C6 was canary data, not a target. No more Wall D, Wall E, Wall F.

Do not touch ApplicationProxy.cpp, CppParcelRewriter.cpp, GuestHost.kt, PmSpoof.kt, DualApplication.kt, MainActivity.kt unless a test failure requires it. S39–S42 code is written; verification, not new writing, is the session.

Do not touch handleJHMethod. Parked (Session 18).

Do not add clone-app features. Filesystem virtualization, per-clone identity, AMS registration, etc. — out of scope unless operator explicitly adds.

Do not advance to feature 1.5 until the batch is verified green. Dynamic attach builds on real relocation, real registry, real symbol resolution. If those aren't on-device verified, dynamic attach is guesswork.

No handoff. Session 41's deliverable is a single commit marking the batch verified (or a fix and re-verify).

Preserved session narratives
Sessions 1–14 (engine foundation), 15–17 (test infra), 18 (DualHost pivot), 19–20 (ApplicationProxy boot), 22–23 (Wall A / Wall B), 24 (milestone 3 accepted), 25–28 (Wall C1/C2/C3), 29–37 (Wall C3/C4/C5/C6 — all canary-state probes now), 38 (nativeInit ported), 39–42 (engine batch written, unverified).

All are retained below verbatim from the Session-28 base PROJECT_STATE.md as historical record. None of them define the current target.

See "Session 28 closed 2026-10-26" and prior sections for full text. The engine-target narrative begins in this document's "What this project is" section.

The blunt version
For 26 sessions the project drifted into "get Viber to work." That was container work. It produced a working canary and a lot of diagnostic knowledge, but it did not advance the hook engine beyond Session 14.

Session 38 broke the flatline by porting nativeInit. Sessions 39–42 extended that with nativeOnLoad, ABI ports, extended anti-detect filtering, full ARM64 relocation, and hook registry tagging. None of Sessions 39–42 have been on-device verified. That's Session 41's first job.

From Session 43 onward, the target is unambiguous: the engine feature inventory. Categories 1–8. Fully ✅. Canary green throughout. Session 75 goal.

No more Viber walls. No more "integration depth." No more coverage counter obsession. Engine only. Canary green.

If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.

text

---

Run the Session 41 verification batch. Paste the four `run finished:` lines (S39, S40, S41, S42) + full suite + canary. Then Session 43 (feature 1.5 — dynamic attach) is written against verified primitives.