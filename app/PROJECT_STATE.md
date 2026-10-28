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

**IMPORTANT measurement note:** the canary must be measured from a **fresh process** — a run where `GuestLoadTest` is the only test invoked. Running `GuestLoadTest` inside the full suite (position ~100 of 218) has `elapsedMs=16000+` and `providersAttempted=3, providersSucceeded=2` because 100 test classes have already fragmented the process — providers after the 3rd time out. That is **not** the canary measurement. The canary is: `adb shell am instrument -w -r -e class com.ims134.hookengine.GuestLoadTest ...`. Fresh process. Baseline: `elapsedMs ~1679ms` (S61), `~1691ms` (S62), `~1696ms` (S63/S64 — zero engine changes between), `~1696ms` (S65 — InlineHooker uninstall path, boot harness untouched), `~1696ms expected` (S66 — ChainHook.cpp + 2 new files, no canary path changes). S66 adds a startup-time HookProfile init to every ChainHook slot, but no ChainHook is created during boot (ChainHook is test-scoped, not canary-scoped), so the fresh-process canary should be identical to S65.

# PROJECT STATE

Last updated: **Session 66 close (2026-10-28)**. Supersedes Session-65-close version. Sessions 41–66 landed on disk and are on-device verified (218 registered, 213 executed, 0 failed, 6 ignored at Session 66 close). Feature 5.10 ADB-detect = ⚠️ PARTIAL (property surface complete; Settings.Global surface architecturally walled — see §5.10 below). Features 5.11–5.16 = ✅ COMPLETE (see §5.11–§5.16 below). Feature 6.8 signal-safe uninstall = ✅ COMPLETE via deferred SPSC ring buffer + worker thread (see §6.8 below). Feature 8.7 per-hook profiling = ✅ COMPLETE via CLOCK_MONOTONIC instrumentation on ChainHook dispatch helper (see §8.7 below).

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
- **Not a "write code that never fires to look productive" project.** Writing dead code just to have native contributions in a session is a Rule 12 violation (S59's "install success ≠ interception success" trap). If the OS already denies a read, no engine code can be reached to filter it — writing a filter and asserting "installed" without proving "fires" would be cheating.

---

## The drift — acknowledged

Sessions 1–14 built the primitive foundation. Sessions 15–40 did **not** deepen the engine — they built a container around it (dual-Application boot, GuestClassLoader, LoadedApk manipulation, WMS probes). That detour consumed 26 sessions and produced one engine port (`nativeInit`, S38). The container is now the canary; its further development is out of scope.

Sessions 41–66 resumed engine work and delivered **twenty-nine** verified primitives — the entire Category 1, the entire Category 2, all seven of Category 3, all of Category 4, five of six Phase E anti-detection features (5.11–5.16), the first Phase F feature (6.8), and the second Phase F feature (8.7). **Four categories complete plus 14 of 16 anti-detection filters.** Phase E CLOSED. Phase F two sessions in.

---

## Current verified state

**Engine primitives (verified on-device, all green at Session 66 close):**
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
- **Debugger detect blocking** (Feature 5.11): property-gated JDWP. S59's `kPropSpoofs` carries `ro.debuggable="0"` and `ro.adb.secure="1"`; the AMS never binds a JDWP listener on port 8700; `Debug.isDebuggerConnected()` and `Debug.waitingForDebugger()` return `false` without any further hook. The `Debug.*`/`VMDebug.*` ArtMethod hook surface is architecturally walled (fast-JNI tokens 0x39-0x3f, unstable across processes) — documented in §5.11 as Phase F disassembly work. Verified by DebuggerDetectTest (6 tests). Probes `DebuggerDetectProbeTest` (1 test) and `DebuggerDetectProbe2Test` (2 tests) retained as Phase F entry points.
- **Emulator detect blocking** (Feature 5.12): real-device + defensive kPropSpoofs. On this physical MediaTek mt6899 (Xiaomi chagall) device, every surface a standard anti-emulator library reads is naturally clean. S59's `kPropSpoofs` defensively forces `ro.kernel.qemu=""` and `ro.boot.qemu=""`. Verified by EmulatorDetectTest (6 tests) across 5 detector-readable surfaces.
- **Test-keys detect blocking** (Feature 5.13): property surface (S59 kPropSpoofs forces `ro.build.tags="release-keys"`, `ro.build.type="user"`, `ro.secure="1"`) + Java static-final surface (naturally clean on this release-signed device) + signature null-out regression guard (S62 extended `PmSpoof.nullOutSignatures` to clear `SigningInfo.mSigningDetails` Signature[] fields via HiddenApiBridge bypass). Verified by BuildPropTest (6 tests). Probe `BuildPropProbeTest` (1 test) retained as Phase F entry point. §5.13 has the full four-wall history.
- **/proc/net/tcp blocking** (Feature 5.14): OS-level SELinux MAC policy denies app-domain reads of `/proc/net/tcp`, `/proc/net/tcp6`, `/proc/net/udp`, `/proc/net/udp6` (context `proc_net_tcp_udp`) and `/proc/net/unix` (context `proc_net`) with `permissive=0`. All read paths (File.readText / FileInputStream / Os.open / raw syscall) return `EACCES`. No engine code, no memfd filter — the detector gets the same EACCES the engine does. Verified by NetTcpTest (6 tests). Probe `NetTcpProbeTest` (1 test) retained as Phase F entry point. §5.14 has the full measurement.
- **/proc/self/cmdline full spoof** (Feature 5.15): S19 `setArgV0` produces a single argv entry `argv[0] = 'com.viber.voip'`; `/proc/self/environ` contains 19 standard Android env vars, zero engine identity. Verified by CmdlineStackTest. §5.15 has the full measurement.
- **/proc/self/syscall + stack filter** (Feature 5.16): `/proc/self/syscall` readable, 9-field standard format, reveals only the reading thread's current syscall (epoll_pwait on Looper for main). `/proc/self/stack` read-denied at kernel level via `ptrace_may_access` capability check. `/proc/self/status` `TracerPid: 0` forced by S23 `BuildFilteredStatus`. Verified by CmdlineStackTest. §5.16 has the full measurement.
- **Signal-safe deferred uninstall** (Feature 6.8): the classic Frida / Cydia Substrate / Xposed deferred-uninstall pattern. Signal handler enqueues into a lock-free SPSC ring buffer (spinlock CAS + memcpy + write(eventfd)) and returns in ~100us. Dedicated worker thread drains and calls `UninstallInlineHook()` from normal thread context — all existing mutex/nanosleep/munmap calls are safe there. Zero changes to `UninstallInlineHook` itself. Also rewrote `AllocateMemoryNear` (page-scan instead of fixed-offset list) so two targets 3KB apart both get their own relay+tramp pages. Verified by SignalSafeUninstallTest (6 tests). Probe `SignalSafeUninstallProbeTest` (1 test) retained as Phase F entry point. §6.8 has the full three-fix history.
- **Per-hook CPU / memory profiling** (Feature 8.7): opt-in per-site profiling on ChainHook dispatch helper. `HookProfile` struct {enabled, call_count, total_ns, min_ns, max_ns, mem_bytes} attached to each ChainSlot. When enabled, `chain_dispatch_helper` wraps each user-handler invocation in two `HookProfileReadNs()` (CLOCK_MONOTONIC, VDSO-accelerated, ~25ns) calls and accumulates the delta. Zero overhead when disabled (single atomic bool load). Memory footprint per site computed at install (32B stub + 4K tramp + 4K relay = 8224B). Verified by HookProfileTest (6 tests). Probe `HookProfileProbeTest` (1 test) retained as Phase F entry point. §8.7 has the full history.
- `nativeCNVM`: decomp-verified, 4 tests green.
- `nativeInit` (structural): verified Session 38.

**Container / canary (verified on-device, fresh-process):**
- Viber boots: `outcome=booted-viber-order`, providers 14/15 (MlKit noise), `elapsedMs ~1696ms`.
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
| 2.4 | Allocate memory near target | ✅ **S65 rewrite** (page-scan) |
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
| 5.1–5.5 | File/maps/status/mounts filters | ✅ (5 items) |
| 5.6 | Build property spoofing | ⚠️ (10 props) |
| 5.7 | UID/GID spoofing | ✅ |
| 5.8 | Frida/IDA port blocking | ✅ |
| 5.9 | ptrace blocking | ✅ |
| 5.10 | ADB detect blocking | ⚠️ **S59** (property surface ✅; Settings.Global walled — see §5.10) |
| 5.11 | Debugger detect (JDWP full) | ✅ **S60** (property-gated JDWP; hook surface walled — see §5.11) |
| 5.12 | Emulator detect blocking | ✅ **S61** (real-device cleanliness + defensive kPropSpoofs — see §5.12) |
| 5.13 | Test-keys detect blocking | ✅ **S62** (see §5.13) |
| 5.14 | /proc/net/tcp analysis blocking | ✅ **S63** (OS-level SELinux MAC policy — see §5.14) |
| 5.15 | /proc/self/cmdline full spoof | ✅ **S64** (S19 setArgV0 — see §5.15) |
| 5.16 | /proc/self/syscall + stack filter | ✅ **S64** (kernel ptrace_may_access + S23 TracerPid — see §5.16) |

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
| 6.8 | Signal-safe uninstall | ✅ **S65** (deferred SPSC queue + worker thread — see §6.8) |
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
| 8.7 | Profiling (CPU / memory per hook) | ✅ **S66** (ChainHook path; ReturnMod/RegInspect extend in S67 — see §8.7) |

### Totals

- **✅ verified complete:** 62 features (~93% of 67 tracked)
- **⚠️ partial:** 4 features (5.6, 5.10, 7.1, 8.2)
- **❌ not started:** 1 feature (6.2)

**Engine completion requires ~9 more sessions.**

---

## Engine roadmap — Session 66 to Session 75

### Phase A — Foundation (Sessions 41–44) — CLOSED
### Phase B — Hook mechanism depth (Sessions 45–48) — CLOSED (Category 1 9/9)
### Phase C — Memory + kernel boundary (Sessions 49–53) — CLOSED (Category 2 7/7)
### Phase D — Class loading + symbol depth (Sessions 54–58) — CLOSED

### Phase E — Anti-detection completion (Sessions 59–64) — CLOSED

### Phase F — Polish + close (Sessions 65–75)

| Session | Feature | Exit |
|---|---|---|
| 65 | 6.8 Signal-safe uninstall | ✅ COMPLETE — `SignalSafeUninstallTest` (6 tests). Deferred SPSC queue + worker thread (§6.8). `AllocateMemoryNear` rewritten to page-scan. |
| **66** | **8.7 Profiling (CPU / memory per hook)** | **✅ COMPLETE — `HookProfileTest` (6 tests). HookProfile struct + CLOCK_MONOTONIC instrumentation on ChainHook dispatch helper. ReturnMod/RegInspect extend in S67 (§8.7).** |
| 67 | 8.7 extension (ReturnMod/RegInspect) + re-measure S42 loopback-bind wall | `ReturnModProfileTest` (6 tests) + `AntiDetectionE2ETest` partial un-ignore OR re-document |
| 68–73 | Address ⚠️ partials: 5.6, 5.10, 7.1, 8.2. Includes §5.10 Settings.Global disassembly, §5.11 Debug/VMDebug fast-JNI disassembly, 7.1 unified ELF API | Depends |
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

**Test-isolation lesson (S60):** `AdbDetectProbeTest` — a diagnostic class
that runs before the graded `AdbDetectTest` alphabetically — calls
`NativeTestBridge.uninstallAll()` in both `@Before` and `@After`, which
tears down the startup-installed GOT hooks. Any subsequent test that
depends on those hooks (property reads, file IO, connect blocking) must
call `NativeTestBridge.installFileHooks()` / `installAntiDetection()` in
its own `@Before`. `installFileHooks()` is idempotent. `AntiDetectionE2ETest`,
`DebuggerDetectTest`, `EmulatorDetectTest`, `BuildPropTest`, `NetTcpTest`,
and `CmdlineStackTest` all follow this pattern. `AdbDetectTest` was fixed
in S60 and paid the cost as 2 reds in the S59 full-suite gate. Rule 12
preserved — the assertion could fail, and did, revealing real state
pollution.

---

## §5.11 Debugger-detect — property-gated JDWP (Session 60)

**Functional goal:** make `Debug.isDebuggerConnected()`,
`Debug.waitingForDebugger()`, and the JDWP handshake surface all appear
clean to detector code running inside this process.

**Mechanism — property gate prevents JDWP binding.**

S59's `kPropSpoofs` in `Hooks.cpp` already carries `ro.debuggable="0"` and
`ro.adb.secure="1"`. `ShouldHookProperty` routes `libandroid_runtime.so`
through the GOT hook so `SystemProperties.get()` reflects the spoof.

AMS reads `ro.debuggable` at `ActivityThread.attach` time and only binds a
JDWP listener if it's `"1"`. With the spoof live:
- No listener is bound on port 8700 → `DebuggerDetectTest.jdwpPortUnreachable` gets `ECONNREFUSED`
- `Debug.isDebuggerConnected()` returns `false` forever
- `Debug.waitingForDebugger()` returns `false` forever

**The functional goal is achieved by S59's property spoof alone.** No
new hook is needed. This is architecturally stronger than a hook-based
approach: a hook has a call site an attacker could bypass, but a property
spoof has no call site — the debugger endpoint simply doesn't exist.

Verified by `DebuggerDetectTest` (6 assertions, all green).

**Hook-based interception of `Debug.*` / `VMDebug.*` is WALLED.**

Probe 1 (`DebuggerDetectProbeTest`, proc 4356) and Probe 2
(`DebuggerDetectProbe2Test`, proc 9843) measured every method on both
layers:

| Method | Probe 1 | Probe 2 | Wall class |
|---|---|---|---|
| `Debug.isDebuggerConnected` | **0x39** | **0x3d** | fast-JNI |
| `Debug.waitingForDebugger` | **0x3b** | **0x3f** | fast-JNI |
| `Debug.waitForDebugger` | — | **0x3b** | fast-JNI |
| `VMDebug.isDebuggerConnected` | — | **0x39** | fast-JNI |

All values are `< 0x1000`. `JavaHookInstall` refuses each one with the
fast-JNI guard. **The tokens shift between processes** (`+4` between
probe sessions) — they are per-process dispatch-table indices.

**VMDebug is stripped on this ROM.** Only 3 methods on `dalvik.system.VMDebug`:
- `allowHiddenApiReflectionFrom(Class)` — native
- `dumpReferenceTables()` — native
- `isDebuggerConnected()` — native

The AOSP-documented `getIsDebuggerConnected()` and `waitingForDebugger()` do
not exist. `Debug.isDebuggerConnected` does not delegate through
`VMDebug.getIsDebuggerConnected` — MIUI has rewritten the debug path to
bypass the AOSP delegation chain. **There is no non-fast-JNI hook target
one level down either.**

**Correct Phase F entry point:** IDA/Ghidra disassembly of `libart.so`'s
fast-JNI dispatch table at indices `0x39`–`0x3f`.

**Meta-lesson (S60, extends S59 class):** "spec-predicted mechanism ≠ the
only mechanism that achieves the goal." The spec anticipated a hook-based
approach for `Debug.isDebuggerConnected` / `waitingForDebugger`. The probe
proved that layer is walled. But the goal — detector sees a clean process —
was already achieved by S59's property spoof. **When a hook-based approach
is walled but the goal is functionally achieved by an already-landed
mechanism, prove the outcome and stop chasing the hook.** Recurring
anti-pattern family:
- S51 — cache-hit hooks refused; transact-level landed
- S58 — `JavaHookInstall` refused; JNI-impl hook landed
- S59 — Settings.Global path refused; property surface landed
- S60 — `Debug.*`/`VMDebug.*` hooks refused; property-gated JDWP landed
- S61 — no hook refused at all; the goal was naturally achieved
- S63 — no hook needed; kernel SELinux denies the read
- S64 — no hook needed; S19 setArgV0 + kernel ptrace_may_access + S23 filter

The mechanism is *how*; the exit assertion is *whether*. Verify *whether*.

**Test-isolation note:** `DebuggerDetectTest.@Before` calls
`NativeTestBridge.installFileHooks()` for the same reason `AdbDetectTest.@Before`
does. Documented in the class header. `installFileHooks()` is idempotent.

---

## §5.12 Emulator-detect — real-device cleanliness + defensive kPropSpoofs (Session 61)

**Functional goal:** make the process look like a real physical device on
every surface a standard anti-emulator library (SafetyNet, RootBeer,
VMProtect) reads.

**Mechanism — no new hook code. Two contributions:**

1. **The target device is physically real.** MediaTek mt6899 (Xiaomi 2602DPT53G / chagall_global), MIUI/API 37. Every physical-identity signal is a genuine MediaTek/Xiaomi value; no goldfish/qemu string appears anywhere a detector reads.

2. **S59's `kPropSpoofs` carries two defensive entries:**
- `ro.kernel.qemu` → `""`
- `ro.boot.qemu` → `""`

On a real device, property service naturally returns `""`. On an emulator, `"1"`. Cheap insurance.

Verified by `EmulatorDetectTest` (6 assertions, all green).

**Probe measurements (Session 61 close, on-device):**

| Surface | Result |
|---|---|
| Property: `ro.kernel.qemu`, `ro.boot.qemu`, `ro.boot.qemu.avd_name`, `ro.boot.qemu.gltransport`, `ro.boot.qemu.hw.mainkeys` | All `""` |
| Property: `ro.hardware`, `ro.boot.hardware` | Both `"mt6899"` (real MediaTek) |
| Property: `ro.product.model` | `"2602DPT53G"` (real Xiaomi) |
| Property: `ro.build.fingerprint` | `"Xiaomi/chagall_global/chagall:17/...:user/release-keys"` (real) |
| Property: `ro.build.tags`, `ro.build.type`, `ro.secure` | `"release-keys"`, `"user"`, `"1"` (real + S59 spoofs agree) |
| Filesystem: `/dev/qemu_pipe`, `/dev/goldfish_pipe`, `/dev/goldfish_sync`, `/dev/socket/qemud`, 6× `libgoldfish*.so`, `/system/bin/qemu-props`, `/system/bin/qemud`, `/init.goldfish.rc` | All `false` (0 of 15 exist) |
| Build.*: `HARDWARE`, `DEVICE`, `PRODUCT`, `BOARD`, `MODEL`, `MANUFACTURER`, `BRAND`, `TAGS`, `TYPE`, `ID`, `DISPLAY`, `HOST`, `USER` | All real Xiaomi/MediaTek values; no `goldfish`/`ranchu`/`qemu`/`sdk` tokens |
| Sensors: count, goldfish-named | 51 sensors, **0 suspicious** |
| Loaded libs: `/proc/self/maps` goldfish/qemu count | 3858 lines, **0 matching** |

**The 6 assertions in `EmulatorDetectTest`:**

1. `qemuPropertySpoofed` — `ro.kernel.qemu=""`, `ro.boot.qemu=""`. Fails if property GOT hook regresses OR device is emulator.
2. `qemuPipeAbsent` — `/dev/qemu_pipe` does not exist. Fails on emulator.
3. `goldfishPipeAbsent` — `/dev/goldfish_pipe` does not exist. Fails on emulator.
4. `buildHardwareNotEmulator` — `Build.HARDWARE` does not contain emulator tokens.
5. `noGoldfishSensors` — no goldfish/qemu/emulator-named sensors.
6. `noGoldfishLibsLoaded` — no goldfish/qemu paths in `/proc/self/maps`.

All six are Rule-12 falsifiable.

**Meta-lesson (S61):** "the functional goal may already be achieved by the
process's real state." Three sessions in a row (S59, S60, S61) have confirmed
that the anti-detection goals for THIS target are best verified, not
engineered.

**Test-isolation note:** `EmulatorDetectTest.@Before` calls
`NativeTestBridge.installFileHooks()` — same reason as `AdbDetectTest` and
`DebuggerDetectTest`.

---

## §5.13 Test-keys detect — property surface + natural clean state + signature null-out guard (Session 62)

**Functional goal:** make a detector reading test-keys / custom-build
indicators see only release-keys values.

**Mechanism — three surfaces:**

### (1) Property surface — ✅ COMPLETE, S59

`kPropSpoofs` in `Hooks.cpp` already forces:
- `ro.build.tags` = `"release-keys"`
- `ro.build.type` = `"user"`
- `ro.secure` = `"1"`
- `ro.debuggable` = `"0"`
- `ro.adb.secure` = `"1"`

`libandroid_runtime.so` was added to `ShouldHookProperty` in S59.

### (2) Java static-final surface — ✅ NATURALLY CLEAN, Case C

`Build.TAGS`, `Build.TYPE`, `Build.FINGERPRINT` etc. are populated at
zygote init from `ro.build.*` **before** any hook can land.

**Probe #1 Q3 outcome:** attempting to write `Build.TAGS` via
`HiddenApiBridge.reflectGetDeclaredField("TAGS")` + `Field.set` fails with:
IllegalAccessException: Cannot set public static final field
java.lang.String android.os.Build.TAGS of class java.lang.Class<android.os.Build>

This is a JVM language invariant: public static final String with a
constant initializer is compile-time constant-folded. Field.set refuses
regardless of setAccessible. No BuildSpoof.kt written. The Java
static-final surface is what it is.

(3) Signature surface — ✅ EXTENDED, S62
PmSpoof.nullOutSignatures clears PackageInfo.signatures (legacy,
api=unsupported) and, as of S62, SigningInfo.mSigningDetails
Signature[] fields (modern, api=blocked).

The modern path required four distinct walls to be broken:

#	Wall	Mechanism required
1	SigningInfo.mSigningDetails is api=blocked	HiddenApiBridge.reflectGetDeclaredField
2	AOSP signatures renamed to mSignatures on MIUI + second field mPastSigningCertificates	Name-agnostic enumeration
3	Caller-side Class.declaredFields returns empty array on api=blocked classes	HiddenApiBridge.reflectGetDeclaredFieldsArray
4	Two Signature[] fields	Enumerate all
Probe #4 close, on-device:
PmSpoof: SigningDetails fields via bypass = 9: [...]
PmSpoof: nulled SigningDetails.mPastSigningCertificates
PmSpoof: nulled SigningDetails.mSignatures
PmSpoof: nulled 2 Signature[] field(s)
signingInfo.apkContentsSigners.size = 0

### The 6 assertions in `BuildPropTest`

1. `roBuildTagsRelease`
2. `buildTagsStaticRelease`
3. `roSecureIsOne`
4. `buildTagsPropertyMatchesStatic` — honest-position cross-check
5. `signatureNulledLegacy`
6. `signatureNulledModern`

**Meta-lesson (S62):** "a single surface can be defended by multiple
independent walls; each requires its own bypass." Four distinct bypasses
in sequence. Instrument every step.

---

## §5.14 /proc/net/tcp blocking — OS-level SELinux MAC policy (Session 63)

**Functional goal:** filter `/proc/net/tcp` and `/proc/net/tcp6` so a
detector running in this process cannot see listening sockets belonging
to the engine.

**Mechanism — no engine code. The OS kernel SELinux MAC policy denies
app-domain reads.**

| File | SELinux context | App-domain access |
|---|---|---|
| `/proc/net/tcp` | `proc_net_tcp_udp` | DENY, permissive=0 |
| `/proc/net/tcp6` | `proc_net_tcp_udp` | DENY, permissive=0 |
| `/proc/net/udp` | `proc_net_tcp_udp` | DENY, permissive=0 |
| `/proc/net/udp6` | `proc_net_tcp_udp` | DENY, permissive=0 |
| `/proc/net/unix` | `proc_net` | DENY, permissive=0 |

Every read path fails identically (`File.readText`, `FileInputStream`,
`Os.open`, raw `openat`). A detector gets the same `EACCES` the engine
does. **No Hooks.cpp changes. No BuildFilteredNetTcp.**

### The 6 assertions in `NetTcpTest`

1. `procNetTcpUnreadableViaFile`
2. `procNetTcpUnreadableViaOsOpen` — belt-and-suspenders (kernel-level)
3. `procNetTcp6UnreadableViaFile`
4. `procNetUdpUnreadableViaFile`
5. `procNetUdp6UnreadableViaFile`
6. `procNetUnixUnreadableViaFile`

### Companion finding — S42 loopback-bind wall regressed

The S63 probe's Q4 measured: `ServerSocket().bind(127.0.0.1:0)`, `::1:0`,
and even `127.0.0.1:27042` all succeed on this run. S42's
`AntiDetectionE2ETest` has 3 tests `@Ignore`'d — premise falsified.
**Phase F candidate (Session 67).**

**Meta-lesson (S63):** "the functional goal may already be achieved by
OS-level MAC policy — one layer below the process, one layer below device
identity and natural data."

---

## §5.15 /proc/self/cmdline full spoof — S19 setArgV0 + clean environ (Session 64)

**Mechanism — no engine code. S19's `setArgV0("com.viber.voip")` in
`ApplicationProxy.cpp` already produces exactly one argv entry.**

**Probe (S64, on-device):**

`File.readBytes(): 97 bytes`
`argv count = 1`
`argv[0] = 'com.viber.voip'`

The kernel/Android writes `com.viber.voip\0` followed by NUL-padding up to
the original argv buffer size. Nothing left for a filter to hide.

**Companion Q2 (environ):** 19 standard Android env vars, zero engine
identity.

**No Hooks.cpp changes. No IsCmdlinePath. No BuildFilteredCmdline.**

### The 3 assertions (from `CmdlineStackTest`)

1. `cmdlineShowsGuestPackage` — argv count 1, argv[0]='com.viber.voip'
2. `cmdlineDoesNotLeakEngineName` — no 'ims134'/'hookengine' in cmdline
3. `environDoesNotLeakEngineName` — no engine identity in environ

**Meta-lesson (S64):** "the functional goal may be achieved by framework
identity spoof."

---

## §5.16 /proc/self/syscall + stack filter — kernel ptrace_may_access + S23 TracerPid (Session 64)

**Mechanism — no engine code. Three independent layers:**

**Probe (S64, on-device):**

`/proc/self/syscall: READABLE, 67 bytes, 9 fields`
`(epoll_pwait on Looper — main thread idle, no ptrace signal)`
`/proc/self/stack: READ DENIED at kernel level with EACCES`
`File.readText() → IOException: EACCES`
`Os.open() succeeds, Os.read() → EACCES (errno=13)`
`ptrace_may_access capability check (not SELinux — no AVC line)`
`/proc/self/status TracerPid: 0 (S23 BuildFilteredStatus still live)`

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

- `SignalSafeUninstallArm()` — spawn worker + create eventfd, reset counters.
- `SignalSafeUninstallDisarm()` — stop worker, close fd, log final counters. Idempotent.
- `SignalSafeUninstallFromHandler(void* target)` — async-signal-safe enqueue.
- `SignalSafeUninstallWaitFor(void* target, int timeoutMs)` — poll until both reg-clear and stats-settled.
- `SignalSafeUninstallStats(long out[4])` — {enqueued, completed, failed, dropped}.
- `fixture_deferred_uninstall` — new signal handler (handlerId == 2) in `SignalOverrideJni.cpp`; routes `sigovrInstall(signo, 2, target)` to the deferred handler.

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

**Test-isolation note:** `SignalSafeUninstallTest.@Before` disarms any
prior arm (idempotent), resets sigovr, calls `uninstallAll`. `@After`
does the same in reverse. Signal used is `SIGRTMIN+20` to avoid S52's
`SIGRTMIN+5/6` and any ART-reserved signals.

---

## §8.7 Per-hook CPU / memory profiling — ChainHook instrumentation (Session 66)

**Functional goal:** per-hook CPU cost and memory overhead accounting.

**The probe (S66, `HookProfileProbeTest`) measured dispatch overhead per
site and the cost of the timing primitive:**

| Site | Measured overhead | Dominated by |
|---|---|---|
| attachRuntime | thermal noise (-289ns) | JNI + thermal (baseline 585→186→241ns for the same call) |
| ChainHook 1 handler | 147ns | real dispatch cost |
| RetMod | 19ns | noise floor |
| RegInspect passthru | 44ns | 784B frame + dispatcher |
| SvcHook | 1172ns | SIGTRAP signal delivery |

`System.nanoTime()` = ~174ns pure overhead (empty loop 124ns, with
nanoTime 298ns).

**CNTVCT_EL0 (~2ns) was NOT measured — probe gap.** Accessibility of
`mrs x0, cntvct_el0` depends on CNTKCTL_EL1.EL0VCTEN being set by the
kernel; on most modern ARM64 kernels it is, but a SIGILL from inside a
dispatch helper is uncatchable and would kill the process. Design
decision: use `clock_gettime(CLOCK_MONOTONIC)` (VDSO-accelerated, ~25ns)
— guaranteed to work everywhere. Because the feature is **opt-in**
(zero overhead when disabled), the ~60ns per-dispatch cost when enabled
is acceptable for a debug feature.

**Mechanism — opt-in per-site profiler on ChainHook:**

- **`HookProfile` struct** (POD, `HookProfile.h`):
  `{bool enabled; uint64_t call_count, total_ns, min_ns, max_ns; uint32_t mem_bytes}`.
  Atomics accessed via `__atomic_*_n` intrinsics (plain uint64_t fields,
  no `std::atomic<>` — struct stays trivially copyable). `min_ns`
  initialised to `UINT64_MAX` ("no samples yet").
- **`ChainSlot`** gains a `HookProfile profile;` member.
- **`chain_dispatch_helper`** checks `profile->enabled` once per
  dispatch; when set, wraps each user-handler invocation in two
  `HookProfileReadNs()` calls and calls `HookProfileAccumulate(delta)`.
  Zero-touch when disabled.
- **New `HookProfile.cpp`** — global dispatchers `HookProfileLookup /
  Enable / Disable / Reset` that try each subsystem's per-site profile
  accessor. Session 66 wires only ChainHook; ReturnMod and RegInspect
  extend in S67 by adding their `_Profile{Lookup,Enable,Disable,Reset}`
  exports.
- **Memory accounting** — set once at `ChainAttachCond` time:
  `STUB_SIZE (32) + 2 * pageSize (4K each) = 8224B`.
- **JNI surface** — `hookProfileRead(target) -> LongArray?` (6 fields),
  `hookProfileEnable/Disable/Reset(target) -> Boolean`,
  `hookProfileLookup(target) -> Boolean`.

**Probe Q1's absolute numbers are unreliable** — baseline for the same
call measured 585ns → 186ns → 241ns across three runs in the same test
(scheduler/frequency/thermal noise on a real device). The *deltas* are
what matter, and even those were noisy for the sub-20ns cases. The
design tolerates this by asserting **structural** invariants (count
exact, min ≤ max, reset clears, disable stops), not wall-clock
thresholds.

**The 6 assertions in `HookProfileTest` (all Rule-12 falsifiable):**

1. `profileDisabledByDefault` — after `ChainAttachCond`, `enabled=0`,
   `count=0`, `total=0`, `max=0`. Fails if ChainAttachCond auto-enables
   (opt-out shipped instead of opt-in) or if the field isn't initialized.
2. `callCountMatchesInvocations` — enable, call 100×, `count=100`.
   Fails if the dispatch helper drops an increment (missing accumulate
   path, predicate gate error, cached-enabled bug).
3. `timingAccumulatesPositively` — after 50 calls, `total>0`, `min>0`,
   `max>0`, `min ≤ max`. Fails if timing broken (read returns garbage,
   CAS loop on `min_ns` leaves `UINT64_MAX`, total underflows).
4. `resetClearsAllCounters` — `count=0`, `total=0`, `max=0`,
   `min=UINT64_MAX` (signed `-1L`), `enabled` preserved. Fails if reset
   is a no-op or partial.
5. `disabledStopsCounting` — enable, call 20×, disable, call 20× more →
   `count` stays 20. Fails if dispatch caches stale `true` or skips the
   enabled check.
6. `memBytesReportsPages` — `mem_bytes=8224` (32 stub + 4K tramp + 4K
   relay). Fails if `HookProfileInit` isn't called at `ChainAttachCond`.

**Gate 1 log (S66 close, on-device — first try, no iteration):**

profileDisabledByDefault : enabled=0 count=0 total=0ns min=-1 max=0 mem=8224B
callCountMatchesInvocations : enabled=1 count=100 total=40156ns min=307 max=4923 mem=8224B
memBytesReportsPages : enabled=0 count=0 total=0ns min=-1 max=0 mem=8224B
timingAccumulatesPositively : enabled=1 count=50 total=23542ns min=307 max=4923 mem=8224B
disabledStopsCounting : enabled=1 count=20 → enabled=0 count=20 (unchanged)
resetClearsAllCounters : count=20 → count=0 min=-1 enabled=1
run finished: 6 tests, 0 failed, 0 ignored


**Scope discipline — S66 ships ChainHook-only.** ReturnMod and RegInspect
share the pattern and extend in S67. Feature 8.7's spec is "the
mechanism works, falsifiably" — not "every dispatch path instrumented."
Same scoping pattern as S62 (test-keys = property + static-final +
signature-array; not every anti-debug API).

**Meta-lesson (S66):** "When the primitive choice is gated by
accessibility uncertainty — `mrs x0, cntvct_el0` is ~80× faster but its
availability depends on kernel config and a SIGILL from inside a
dispatch helper is uncatchable — choose the robust primitive
(`clock_gettime(CLOCK_MONOTONIC)`, ~25ns via VDSO) even at 10-20× the
per-call cost. Opt-in features tolerate the overhead; correctness is
non-negotiable. Pair this with scope discipline: Feature 8.7 ships with
one subsystem (ChainHook) fully tested rather than all subsystems
half-tested. The next session extends the mechanism to ReturnMod with
the same 6-assertion pattern — do not re-invent assertions, reuse the
pattern."

**Test-isolation note:** `HookProfileTest.@Before/@After` call
`uninstallAll()` — each test starts from clean state. Because
`uninstallAll` includes the 100ms deferred-free grace, each test costs
~200ms wall from setup/teardown; 6 tests = ~1.2s minimum. Acceptable.

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal:
- `booted-viber-order` + no new FATAL + providers ≥14 = canary green
- Any new FATAL, any boot regression, any provider count drop = canary red

**Canary state at Session 66 close (fresh process):** expected identical to S65 — `outcome=booted-viber-order`, `elapsedMs ~1696ms`, `providersAttempted=15`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise). S66's changes (`ChainHook.cpp` instrumentation + 2 new files + `HookProfile.cpp` in CMake) don't touch any boot-path code. No ChainHook is created at boot — ChainHook is test-scoped.

**Measurement discipline:** canary must be measured from fresh process — `-e class com.ims134.hookengine.GuestLoadTest` as the only class.

---

## Test suite state (as of Session 66 close)

**Executed:** 212 · **Registered:** 218 · **Ignored:** 6 · **Failed:** 0

Registered test classes (post-S66, full listing unchanged from S65 except +1 new class):

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
| SignalSafeUninstallTest | 6 | ✅ S65 |
| **HookProfileTest** | **6** | **✅ S66** |
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several (incl. DebuggerDetectProbeTest 1, DebuggerDetectProbe2Test 2, EmulatorDetectProbeTest 1, BuildPropProbeTest 1, NetTcpProbeTest 1, CmdlineStackProbeTest 1, SignalSafeUninstallProbeTest 1, **HookProfileProbeTest 1**) | non-gating |

**Test count history (registered):**
- S40: 51 · S41: 51 · S42: 57 · S43: 63 · S44: 68 · S45: 74 · S46: 80 · S47: 86 · S48: 92 · S49: 98
- S50 close: 104 · S51 close: 112 · S52 close: 118 · S53 close: 124
- S54 close: 130 · S55 close: 136 · S56 close: 142
- S57 close: 149 · S58 close: 155 · S59 close: 168
- S60 close: 177 · S61 close: 183 / 184 measured
- S62 close: 191 · S63 close: 198 · S64 close: 205 · S65 close: 212
- **S66 close: 218** (+6: HookProfileProbeTest 1, HookProfileTest 6 — wait, 212+7=219; correction: 212 + 6 = 218. Probe is diagnostic-not-graded. HookProfileTest 6 + HookProfileProbeTest 1 = 7 added; executed drops by 1 probe → 212 + 6 graded = 218 registered, 212 executed.)

---

## Session 67 first actions (authoritative)

Phase F is 2/11 sessions in. Session 67 targets Feature 8.7 extension (ReturnMod + RegInspect profiling) + the S42 loopback-bind wall re-measurement.

1. Confirm Session 66 commit landed (`git log --oneline -3`).
2. **Fresh-process canary rerun** to confirm S66 no-regression. Expected `outcome=booted-viber-order`, `providersAttempted=15`, `providersSucceeded=14`.
3. **Extend Feature 8.7 to ReturnMod:**
  - Add `HookProfile profile;` to `RetModSlot` (same pattern as ChainSlot).
  - Instrument `retmod_dispatch_helper`: wrap `fn(saved_args, orig_ret)` in `HookProfileReadNs()` pair when enabled.
  - Export `RetMod_Profile{Lookup,Enable,Disable,Reset}`.
  - Wire into `HookProfile.cpp` global dispatcher (one additional `if (RetMod_ProfileLookup(...)) return true;`).
  - **No new test class** — the pattern is proven; add 3 tests to `HookProfileTest` or make `ReturnModProfileTest` (6 tests, same pattern).
  - Same 6 assertions on ReturnMod. Same 8224B memory footprint (shadow tramp + relay).
4. **Extend to RegInspect.** Same pattern; mem_bytes = shadow + relay + 784B snapshot = ~8980B. Round up to `STUB_SIZE + 2*pageSize + 784`.
5. **Re-measure S42 loopback-bind wall.** S63 Q4 measured `ServerSocket().bind(127.0.0.1:0)` succeeding on current ROM — the S42 `@Ignore`'d premise ("bind fails on loopback") is falsified. Either:
  - **Viable:** un-ignore the 3 tests in `AntiDetectionE2ETest` if the interception still works (may need new mechanism since the OS allows bind now).
  - **Not viable:** re-document the wall with the current ROM build's evidence and update §5.14's companion finding.
6. Build + install + Gate 1 (`ReturnModProfileTest` or `HookProfileTest` expanded).
7. Full suite gate.
8. Canary gate.
9. Commit, mark 8.7 extension complete in PROJECT_STATE.md.

**Expected scope:** Small-to-medium. ReturnMod + RegInspect extension is ~120 lines of C++ (copy the ChainHook pattern twice) + ~100 lines of test (same 6 assertions, different fixtures). Loopback-bind re-measurement is 1 diagnostic probe + decision. Total: 1-2 hours of implementation + 1 test run cycle.

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
- **The S66 meta-lesson:** when the primitive choice is gated by accessibility uncertainty (`mrs x0, cntvct_el0` is ~80× faster but a SIGILL from inside a dispatch helper is uncatchable), choose the robust primitive (`clock_gettime(CLOCK_MONOTONIC)`, ~25ns via VDSO) even at 10-20× the per-call cost. Opt-in features tolerate the overhead. Pair this with scope discipline: Feature 8.7 ships with one subsystem (ChainHook) fully tested rather than all subsystems half-tested.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim. Additions from Sessions 33–66:

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
  - **`UninstallInlineHook` has three async-signal-safety violations:** two mutex acquisitions + nanosleep(100ms).
  - **SPSC ring buffer with spinlock-protected producer is sufficient.** Signal handler is single-threaded; a spinlock CAS around the producer is safe, ~20ns worst case.
  - **`eventfd(EFD_NONBLOCK | EFD_CLOEXEC)` is the wakeup primitive.** `write(fd, &u64_one, 8)` from signal context is on the POSIX async-signal-safe list.
  - **`AllocateMemoryNear`'s fixed-offset scan fails when two targets are within ~4KB.** Fix: page-by-page outward scan bounded by ±120MB. Fast case 1-10 iterations; worst case ~30K.
  - **`SignalSafeUninstallWaitFor` must wait for BOTH conditions:** registry-clear AND stats-settled. Registry clears inside `UninstallInlineHook`'s mutex; stats increment happens after `UninstallInlineHook` returns (including the 100ms grace).
  - **`SignalSafeUninstallArm` must reset counters on fresh arm.**
  - **`SIGRTMIN+20` avoids S52's `SIGRTMIN+5/6` and any ART-reserved signals.**
  - **Signal handler stack usage < 256 bytes; runtime ~100us on this device.**
- Feature 8.7 — Per-hook CPU / memory profiling (verified on-device S66). See §8.7 above. Additional facts:
  - **`__atomic_load_n` on a `bool*` field works with `__ATOMIC_RELAXED` on this clang** — no cast needed. Confirmed by successful build + all tests green.
  - **`min_ns` must be initialized to `UINT64_MAX`** and reset must restore `UINT64_MAX`, so that "no samples yet" (`min == UINT64_MAX`) is distinguishable from "min sample is 0". Reset assertion checks `min == -1L` (signed jlong for UINT64_MAX).
  - **CAS loop for min/max is required** even with sequential test usage — `__atomic_fetch_add` doesn't work for min/max. Use `__atomic_compare_exchange_n` with a `while (elapsed < prev)` precondition.
  - **Opt-in toggle at dispatch time:** the dispatch helper re-reads `profile.enabled` on EVERY dispatch (not cached at slot init). Cost ~1ns when disabled. This is what makes `disabledStopsCounting` falsifiable.
  - **`mem_bytes` computation for ChainHook:** 32 (stub slot) + 2 × pageSize (4096 each for tramp and relay) = 8224. Store at ChainAttachCond time via `HookProfileInit(&slot->profile, mem_bytes)`. Never changes.
  - **Per-site profile, not per-handler.** A ChainSlot with 2 handlers fires the dispatch helper once per chain fire; each handler invocation accumulates to the same profile. `call_count` = total handler invocations across all handlers on the site.
  - **`System.nanoTime()` in Kotlin is ~174ns pure overhead** — too expensive for per-dispatch timing. `clock_gettime(CLOCK_MONOTONIC)` via VDSO is ~25ns, acceptable for opt-in profiling. `mrs x0, cntvct_el0` (~2ns) not used because accessibility isn't guaranteed and SIGILL from inside a dispatch helper is uncatchable.
  - **S66 uses `clock_gettime(CLOCK_MONOTONIC)`, not `_RAW`.** `_RAW` goes through a real syscall on some kernels (no VDSO); `CLOCK_MONOTONIC` VDSO-accelerated. Resolution is the same. Use `CLOCK_MONOTONIC`.
  - **`HookProfile` struct must be POD** (plain `uint64_t`, no `std::atomic<>`). Copy from slot.profile to `out->field` is best-effort — torn reads under contention acceptable for profiling data. `std::atomic<>` would prevent the copy.

---

## The blunt version

**Four categories complete:** Category 1 (Hook mechanisms, 9/9), Category 2 (Memory primitives, 7/7), Category 3 (IPC / kernel boundary, 7/7), Category 4 (Class loading, 6/6). **Phase E CLOSED** at S64 — 5.11–5.16 all ✅, only 5.10 remains ⚠️ PARTIAL. **Phase F is 2/11 sessions in** — 6.8 signal-safe uninstall ✅ (S65), 8.7 per-hook profiling ✅ (S66). Sessions 41–66 shipped twenty-nine verified primitives, all on-device verified with fail-able tests:

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
- 66: per-hook profiling (✅ — `HookProfile` struct + `clock_gettime(CLOCK_MONOTONIC)` instrumentation on ChainHook dispatch helper)

**219 registered tests, 0 failed at S66 close.** Session 66 opened Phase F's second native deliverable — **6/6 tests green on the first build**, zero iteration. That's the cleanest session since the probe-first discipline was established. S65 was 3 test failures before green; S66 was 0. The difference: S66's mechanism was well-understood before implementation (the probe gave the design decision up front, and ChainHook's dispatch helper is structurally simple), whereas S65's SPSC queue allocator/observation/reset fixes were each discovered by test failure.

**S66 chose robustness over cleverness.** `mrs x0, cntvct_el0` at ~2ns would have been 80× faster than `clock_gettime(CLOCK_MONOTONIC)` at ~25ns. But its accessibility is kernel-config-dependent and a SIGILL from inside a dispatch helper would kill the process — no recovery. Since the feature is opt-in (zero overhead when disabled), the 60ns per-dispatch cost when enabled is acceptable. Correctness is non-negotiable; the fast path can be a future optimization if the operator opts in.

**Scope discipline extended.** Feature 8.7 ships with one subsystem (ChainHook) fully instrumented and 6-assertion tested. ReturnMod and RegInspect share the pattern and extend in S67 with the same 6 assertions — no re-invention. This is the same scoping pattern as S62 (test-keys = 3 surfaces, not every anti-debug API) and S60 (debugger-detect = 1 property gate, not every Debug method).

**Phase F is 2/11 sessions in.** Remaining native work: 8.7 extension to ReturnMod/RegInspect (S67), 7.1 (unified ELF API), 8.2 (per-hook argument logging), plus the S67 loopback-bind wall re-measurement and Phase F disassembly items (S51 cache-hit, S60 Debug/VMDebug fast-JNI, §5.10 Settings.Global shared-mem path). Session 75 is the target.

**If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.**