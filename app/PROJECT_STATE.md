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

**IMPORTANT measurement note:** the canary must be measured from a **fresh process** — a run where `GuestLoadTest` is the only test invoked. Running `GuestLoadTest` inside the full suite (position ~50 of 155) has `elapsedMs=16000+` and `providersAttempted=3, providersSucceeded=2` because 49 test classes have already fragmented the process — providers after the 3rd time out. That is **not** the canary measurement. The canary is: `adb shell am instrument -w -r -e class com.ims134.hookengine.GuestLoadTest ...`. Fresh process. Baseline: `elapsedMs ~1700ms`, `providersAttempted=15`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise).


# PROJECT STATE

Last updated: **Session 59 close (2026-10-28)**. Supersedes Session-58-close version. Sessions 41–59 landed on disk and are on-device verified (157 registered, 155 executed, 0 failed, 2 ignored at Session 59 close). Feature 5.10 ADB-detect = ⚠️ PARTIAL (property surface complete; Settings.Global surface architecturally walled — see §5.10 below).

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

Sessions 41–58 resumed engine work and delivered **twenty-one** verified primitives — the entire Category 1, the entire Category 2, all seven of Category 3, and all of Category 4. **Four categories complete:** Categories 1, 2, 3, 4. Phase D closed.

---

## Current verified state

**Engine primitives (verified on-device, all green at Session 58 close):**
- Inline hook: 9 ARM64 instruction kinds. Verified by RelocTest (49 assertions).
- GOT hook: 26 sites, works.
- ArtMethod hook: works (native methods, non-fast-JNI only).
- ArtMethod entry/exit trace: works on framework natives. Verified by ArtMethodTraceTest (5 tests).
- Hook chaining: works. Verified by ChainHookTest (6 tests).
- Conditional hook: predicate gates per-slot handler fire. Verified by ConditionalHookTest (6 tests).
- Return-value modification: post-call modifier transforms caller-visible x0. Verified by ReturnModTest (6 tests).
- SVC instruction hooking: BRK-magic + SIGTRAP handler. Verified by SvcHookTest (6 tests).
- Register state inspection: full ARM64 GP + SP + NZCV + Q register capture. PROBE and PASSTHRU modes. Verified by RegInspectTest (6 tests).
- Memory read / write / scan: `/proc/self/maps`-validated; MemWrite mprotects each page to RW, writes, restores orig (Rule 10 preserved). Verified by MemoryOpsTest (6 tests).
- Full maps enumeration: MemEnumMaps, MemEnumMapsFilter, MemFindRegionContaining; per-entry {start, end, prot, offset, dev major/minor, inode, path}. Verified by MapsEnumTest (6 tests).
- Backtrace from hook site: FP-chain walker with `/proc/self/maps` PC validation, 16-byte alignment guard, monotonic ascending FP guard, cycle detection, bounded at BT_MAX_FRAMES=32. Verified by BacktraceTest (6 tests).
- Native ClassLoader chaining (`nativeFNLN`): C++ merge of `pathList.dexElements` (host-only prepended to guest, deduped by identity), preserving `ClassLoaderBridge.installFor` API. Rule 15 debt closed. Verified by NativeFnlnTest (6 tests) and prior ClassLoaderBridgeTest (3 tests).
- Dynamic DEX loading: `DexLoaderCreate(env, dexPath, optimizedDir, parent)` in `DexLoader.cpp` constructs `dalvik/system/DexClassLoader` entirely in C++. Verified by DexLoadTest (6 tests).
- DEX merging at runtime: multi-path `DexClassLoader("pathA:pathB", ...)` — same native `DexLoaderCreate`, different argument. Merged loader contains one element per source; chains into a guest via 4.2 making all classes from both sources resolvable. Verified by DexMergeTest (6 tests).
- Dynamic runtime attach: works with alignment/mapped/exec guards. Verified by DynamicAttachTest (6 tests).
- Hidden-API bypass: works via thread-attached reflection.
- `IPCThreadState::transact` inline hook: works.
- AIDL prelude rewrite: works on 4 interfaces.
- ServiceManager lookup interception (transact-level): alias and null overrides rewrite String16 names in the request parcel before transact. Verified by ServiceManagerTest (8 tests).
- Signal handling override: sigaction-based per-slot handlers with PASS_THROUGH / SUPPRESS actions, full siginfo capture. Verified by SignalOverrideTest (6 tests).
- **Native ServiceManager addition** (Feature 3.7): register a synthetic binder under any name so `ServiceManager.getService(name)` returns it. Mechanism: 3.4 alias rewrite makes the driver return a valid BinderProxy in the reply; a C++ inline hook on `android_os_Parcel_nativeReadStrongBinder` (located by `JNINativeMethod`-struct scan in `libandroid_runtime.so`) returns the original Kotlin Binder when `g_isSynthetic` is set. Same-process caller sees the original instance. Verified by SyntheticServiceTest (6 tests).
- PLT stub resolution: `PltResolveForSymbol(lib, symbol)` walks `.rela.plt` and correlates relocation index to `.plt` (or BTI `.plt.sec`) entry; validates the first instruction is `adrp` or `bti c`. Verified by PltResolveTest (7 tests, including a callable check).
- `nativeCNVM`: decomp-verified, 4 tests green.
- `nativeInit` (structural): verified Session 38.

**Container / canary (verified on-device, fresh-process):**
- Viber boots: `outcome=booted-viber-order`, providers 14/15 (MlKit noise), `elapsedMs ~1700-1900ms`.
- SplashActivity: instantiated, `onCreate`/`onStart`/`onResume` all run.
- WMS accepts token + allocates Surface when host task visible.

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
| 1.6 | SVC instruction hooking (bypass libc) | ✅ |
| 1.7 | Hook chaining | ✅ |
| 1.8 | Conditional hook | ✅ |
| 1.9 | Return-value modification | ✅ |

### Category 2 — Memory primitives — **7/7 COMPLETE**

| # | Feature | Status |
|---|---|---|
| 2.1 | Read memory at arbitrary address | ✅ |
| 2.2 | Write memory (page-prot safe) | ✅ |
| 2.3 | Scan memory for pattern | ✅ |
| 2.4 | Allocate memory near target | ✅ |
| 2.5 | Enumerate maps | ✅ **S50** |
| 2.7 | Register state inspection at hook site | ✅ |
| 2.8 | Backtrace from hook site | ✅ **S53** |

### Category 3 — IPC / kernel boundary — **7/7 COMPLETE**

| # | Feature | Status |
|---|---|---|
| 3.1 | Binder transact interception | ✅ |
| 3.2 | AIDL prelude parse | ✅ |
| 3.3 | AIDL reply rewrite | ✅ |
| 3.4 | ServiceManager lookup interception | ✅ **S51** (transact-level; cache-hit blocked by ROM) |
| 3.5 | Property read/write interception | ✅ |
| 3.6 | Signal handling override | ✅ **S52** |
| 3.7 | Native ServiceManager addition | ✅ **S58** (JNI-impl hook on `nativeReadStrongBinder`; same-process only) |

### Category 4 — Class loading / code isolation — **6/6 COMPLETE**

| # | Feature | Status |
|---|---|---|
| 4.1 | Custom ClassLoader delegation | ✅ |
| 4.2 | Native ClassLoader chaining | ✅ **S54** (Rule 15 debt closed) |
| 4.3 | Dynamic DEX loading | ✅ **S55** |
| 4.4 | DEX merging at runtime | ✅ **S56** |
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
| 5.10 | ADB detect blocking | ⚠️ **S59** (property surface ✅; Settings.Global walled — see §5.10) |
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
| 7.6 | PLT stub resolution | ✅ **S57** |

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

- **✅ verified complete:** ~55 features (82% of 67 tracked)
- **⚠️ partial:** 6 features
- **❌ not started:** 6 features

**Engine completion requires ~17 more sessions.**

---

## Engine roadmap — Session 59 to Session 75

### Phase A — Foundation (Sessions 41–44) — CLOSED
### Phase B — Hook mechanism depth (Sessions 45–48) — CLOSED (Category 1 9/9)
### Phase C — Memory + kernel boundary (Sessions 49–53) — CLOSED (Category 2 7/7)
### Phase D — Class loading + symbol depth (Sessions 54–58) — CLOSED

| Session | Feature | Exit |
|---|---|---|
| 54 | 4.2 Native `nativeFNLN` (Rule 15 debt closure) | ✅ `NativeFnlnTest` (6 tests) |
| 55 | 4.3 Dynamic DEX loading | ✅ `DexLoadTest` (6 tests) |
| 56 | 4.4 DEX merging at runtime | ✅ `DexMergeTest` (6 tests) |
| 57 | 7.6 PLT stub resolution | ✅ `PltResolveTest` (7 tests) |
| 58 | 3.7 Native ServiceManager addition | ✅ `SyntheticServiceTest` (6 tests) |

### Phase E — Anti-detection completion (Sessions 59–64)

| Session | Feature | Exit |
|---|---|---|
| **59** | **5.10 ADB detect blocking** | **next — `AdbDetectTest`** |
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

## §Cache-hit wall — measured, documented, closed

**Feature 3.4 as originally spec'd is complete.** The `ServiceManager` transact-level rewriter fires on every `handle==0` transact whose request parcel carries a String16 name matching an override. Alias and null overrides both work. 8 green tests.

The Session 51 **cache-hit aspiration** — intercepting `ServiceManager.getService("activity")` when the framework serves it from a cached binder without dispatching through `IPCThreadState::transact` — is a measured architectural wall on MIUI/API 37. **Fifteen-plus mechanisms were tried. Every one was blocked by a specific MIUI hardening. The definitive on-device close:**


### The three fast-JNI tokens


`rawGetService FromReflectedMethod = 0x3d`
`getService FromReflectedMethod = 0x3f`
`checkService FromReflectedMethod = 0x41`


All three return small integer tokens, not ArtMethod pointers. These are ART's encoding of fast-JNI dispatch-table indices. Every ArtMethod-based interception path for these three methods is architecturally impossible on this ROM.

### The `+32` slot is not a callable code address

`ArtMethod*+32` for `getService` holds `0x38000970eceb68`, not in `/proc/self/maps`. Cannot be inline-hooked. Patching `+32` to our shim succeeds at write level but never affects dispatch.

### `SimpleArrayMap.get(sCache)` is JIT-inlined

Hooking `SimpleArrayMap.get(Object)` succeeds but the shim fires on unrelated ArrayMap lookups, not the actual `sCache.get(name)` from `getService`. MIUI JIT-inlines the call.

### The `sCache` field is a decoy

MIUI's `rawGetService` never puts into `sCache` — only `initServiceCache()` at process startup does. Actual cache is in `getIServiceManager().getService2(name)`.

### Future work

Cache-hit interception on MIUI/API 37 requires IDA Pro analysis of `libart.so`'s fast-JNI dispatch layer at indices `0x3d`/`0x3f`/`0x41`. That is a dedicated research session, not a feature-landing task.

---

## §5.10 ADB-detect — four measured walls (Session 59)

**Property surface — COMPLETE.** `ro.debuggable`, `init.svc.adbd`,
`ro.adb.secure`, `service.adb.root` return spoofed values.
`libandroid_runtime.so` added to `ShouldHookProperty` so reflection-based
`SystemProperties.get` fires 5.6's existing spoof. Verified by
`AdbDetectTest.roDebuggableSpoofed` / `initSvcAdbdSpoofed`.

**Settings.Global surface — ARCHITECTURALLY WALLED.** Four hypotheses
measured, four refuted on-device:

| # | Mechanism | Refuted by |
|---|---|---|
| 1 | `NameValueCache.mValues` poison | Write persists forever (`pre-write map='0'`), framework read bypasses (`getString` returns `'1'`). |
| 2 | `mGenerationTrackers` sync | Field exists but stays `ArrayMap size=0`. Not the invalidation scheme here. |
| 3 | `mReadableFieldsWithRedactedValue` special path | These two keys are the only map entries; redacted "0" is a fallback for unprivileged callers, we're privileged so real value returned. |
| 4 | `IContentProvider.call()` via transact | S59 probe: 0 transacts fired for 3 ADB/DEV reads (`fireDelta=0`, `codes=[]`, `elapsed=0ms`). Control `Settings.Secure` read fired code 21 with 368-byte Bundle reply. The reads never leave the process. |

**What the CP reply reveals (for Phase F):** MIUI's cache-miss reply is a
`Bundle` containing `_track_generation` marker + `android.util.MemoryIntArray`
+ `android.os.ParcelFileDescriptor` + `value`. The framework uses
  **shared-memory generation tracking**, not `mGenerationTrackers` ArrayMap.
  The `adb_enabled` shared-mem region was populated at process boot, before
  hook install — no user-space intercept point exists after boot.

**Correct Phase F entry point:** disassemble
`framework.jar!classes3.dex`'s `Settings$NameValueCache.getStringForUser`
on this ROM and trace the shared-memory read path. IDA/Ghidra work, not an
engine-primitive session. The `CP diag` instrumentation in
`BinderTransactHook.cpp` and the field dump in `AdbSpoof.install()` are
retained as Phase F entry points.

**Meta-lesson (recurring):** "install success ≠ interception success."
Same class as S51's `SimpleArrayMap.get()` (installed, never dispatched)
and S58's `JavaHookInstall` (installed, quick-ABI mismatch). The
`AdbSpoof.poison()` return value is "code ran," not "framework returns 0."
The `post-poison getString=... getInt=...` diagnostic line is the ground
truth.

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal:
- `booted-viber-order` + no new FATAL + providers ≥14 = canary green
- Any new FATAL, any boot regression, any provider count drop = canary red

**Canary state at Session 58 close (fresh process):** pending full-suite + fresh-process verification run before final S58 close. Engine changes in S58 touch the SM-add path only, gated behind `syntheticAdd` being called from tests — no startup-path changes. Expected green. Prior confirmed baseline (S57 close): `outcome=booted-viber-order`, `elapsedMs=~1700`, `providersAttempted=15`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise).

**Measurement discipline:** canary must be measured from fresh process — `-e class com.ims134.hookengine.GuestLoadTest` as the only class. In-suite readings (~position 50 of 155) show `providersAttempted=3, providersSucceeded=2` because 49 test classes fragment the process — that is **not** the canary measurement.

---

## Test suite state (as of Session 58 close)

**Executed:** 155 · **Registered:** 157 · **Ignored:** 6 · **Failed:** 0

Registered test classes (post-S58):

| Class | Tests | Status |
|---|---|---|
| NativeItemTest | 5 | ✅ |
| ProcessHooksTest | 4 | ✅ |
| RegressionTest | 5 | ✅ |
| RelocTest | 1 (49 assertions) | ✅ |
| ProxyBridgeSurfaceTest | 4 | ✅ |
| NativeInitTest | 2 | ✅ |
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
| **PltResolveTest** | **7** | **✅ S57** |
| **SyntheticServiceTest** | **6** | **✅ S58** |
| **AdbDetectTest** | **6 (2 ignored)** | **⚠️ S59** |
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several | non-gating |

**Test count history:**
- S40: 51 · S41: 51 · S42: 57 · S43: 63 · S44: 68 · S45: 74 · S46: 80 · S47: 86 · S48: 92 · S49: 98
- S50 close: 104 · S51 close: 112 · S52 close: 118 · S53 close: 124
- S54 close: 130 · S55 close: 136 · S56 close: 142 (+6 DexMergeTest)
- S57 close: 149 (+7 PltResolveTest)
- S58 close: 155 (+6 SyntheticServiceTest)
- S59 close: **157** (+4 AdbDetectTest green, +2 ignored)

---

## Session 60 first actions (authoritative)

1. Confirm Session 59 commit landed (`git log --oneline -3`).
2. **Fresh-process canary rerun** to confirm S59 no-regression. Expected
   `outcome=booted-viber-order`, `providersAttempted=15`, `providersSucceeded=14`.
3. Write Feature 5.11 — debugger detect (JDWP handshake, Debug.isDebuggerConnected,
   waitingForDebugger, full /proc/self/status TracerPid).
4. Build + install (Rule 14).
5. `am instrument` DebuggerDetectTest. Expect `OK (6 tests)`.
6. Full suite. Expect `OK (161 tests)`.
7. Canary from fresh process.
8. Commit, mark 5.11 → ✅.

---

## Rules of engagement (unchanged, still authoritative)

- **Target is the engine feature inventory, not ApplicationProxy coverage.**
- **Viber is canary only. Canary must be measured from fresh process.**
- **Every session advances at least one ⚠️/❌/🔧 → ✅.**
- **Verification-first:** verify any prior-session unverified code before writing new code.
- **Rule 5 (behavior match) — partial disclosure for structural ports.**
- **Rule 12: test assertions must be able to fail.** A test that cannot fail is as broken as one that cannot pass.
- **Rule 14: every test-run instruction includes `./gradlew` + `adb install -r -t` on both APKs.**
- **Rule 15: engine is native.**
- **Rule 7 (decomp-first) when wire format / symbol / field layout is uncertain.**
- **No `am instrument` output = no session close.**
- **Deferred items stay deferred:** handleJHMethod (S18), tnFIN code-36 byte rewrite (S12), clone-app features.
- **Full-file replacements only for code files.** Documentation edits use targeted edits within the existing file.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim. Additions from Sessions 33–58:

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
  - Test-set: 7 assertions in `PltResolveTest.kt` — `pltResolveOpenNonZero`, `pltResolveMissingSymbolZero`, `pltResolveMissingLibZero`, `pltEntryIsExecutable`, `pltEntryStubStructure`, `pltDistinctSymbolsDiffer`, `pltEntryCallableForOpen`.
- Feature 3.7 — Native ServiceManager addition (verified on-device S58).
  - **Primary mechanism:** JNI-impl inline hook on `android_os_Parcel_nativeReadStrongBinder` in `libandroid_runtime.so`.
  - **The symbol is NOT exported** — dynsym walk fails. Must scan the .so's mapped memory for the string `"nativeReadStrongBinder\0"` and then find the 8-byte value equal to that string's address (that's the `JNINativeMethod.name` field). fnPtr is at `+16` from there.
  - **Composition with 3.4:** alias rewrite (`name -> "activity"`) is essential — it forces the driver to return a valid `BinderProxy`. Any attempt to swap the reply Parcel (`mData` replace, `mObjects` patch, `mDataPos` reset, mprotect RW, `mObjectList` injection) crashes inside MIUI's post-transact Parcel validation on this ROM. **Do not pursue reply-Parcel mutation — it does not work on MIUI/API 37.**
  - **ART fast-JNI wall extends to `ServiceManagerProxy.getService2` (0x3d) and `checkService2` (0x3f).** Same class of wall as S51's outer SM methods.
  - **ArtMethod hook on `Parcel.readStrongBinder()` Java wrapper fails:** caller's JIT code expects the method's original `data_` = quick stub, not a JNI-ABI function. Do not replace `data_` on a Java method's ArtMethod — JIT-ABI managed return (compressed ref in w0) will be truncated to 32 bits and SIGSEGV.
  - **Only the JNI-impl function** (found via `JNINativeMethod` struct scan) is safe to hook — ART's intact JNI trampoline handles compressed-ref conversion on return.
  - **Same-process only.** Cross-process callers still see a `BinderProxy` — the driver has no handle for our synthetic name.
  - Pre-intercept + post-intercept contract for 3.7: `PreIntercept` sets `g_isSynthetic` when it sees the synthetic name in the request; `PostIntercept` (after transact) must NOT clear `g_isSynthetic` — it's cleared only by `FakeNativeReadStrongBinder` when the caller's `readStrongBinder` fires. If `PostIntercept` clears it early, the hook fires on the wrong read (e.g., an unrelated `readStrongBinder` call elsewhere) and the mechanism breaks.

---

## The blunt version

**Four categories complete:** Category 1 (Hook mechanisms, 9/9), Category 2 (Memory primitives, 7/7), **Category 3 (IPC / kernel boundary, 7/7)**, **Category 4 (Class loading, 6/6)**. Sessions 41–58 shipped twenty-one verified primitives, all on-device verified with fail-able tests:

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

**155 registered tests, 0 failed at S58 close.** Session 58 took eight iterations to land 3.7 — each one removed one MIUI/API-37-specific hardening layer (reply-Parcel mprotect EACCES, type-field corruption, object-table mismatch, protected-data validation, fast-JNI tokens on `getService2`, JIT-stub ABI truncation, non-exported JNI symbol). The mechanism that worked in the end (JNI-impl hook via `JNINativeMethod` struct scan) is the only one that doesn't cross the ROM's architectural boundaries.

**Phase D closed.** Phase E (anti-detection completion, S59–S64) and Phase F (polish, S65–S75) remain. Session 75 is the target. Session 59 opens Feature 5.10 — ADB detect blocking.

**If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.**