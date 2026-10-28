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
- Per-hook argument logging — last N argument vectors seen at each hook site

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
- Build property spoofing — full 15-property table: `ro.debuggable`, `ro.secure`, `ro.build.tags`, `ro.build.type`, `ro.kernel.qemu`, `ro.boot.qemu`, `init.svc.adbd`, `service.adb.root`, `sys.oem_unlock_allowed`, `ro.adb.secure`, plus the S70 bootloader-integrity tell set (`ro.boot.verifiedbootstate`, `ro.boot.flash.locked`, `ro.boot.veritymode`, `ro.boot.warranty_bit`, `ro.warranty_bit`)
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
- Per-hook argument logging — capture last N arg vectors per site

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
- **Instrumentation:** ~7 capabilities across trace / call-graph / registers / backtrace / counters / profiling / argument logging
- **Memory primitives:** ~5 capabilities
- **IPC / kernel boundary:** ~7 capabilities across Binder / AIDL / ServiceManager / property / signal
- **Class loading:** ~5 capabilities
- **Anti-detection:** ~25 distinct filters across filesystem / debugger / network / kernel / environment
- **Coordination:** ~9 capabilities across registry / tagging / atomic / rollback / deferred-free / signal-safe / conflict / gate
- **Symbol resolution:** ~6 capabilities
- **Diagnostics:** ~7 capabilities
- **Stability invariants:** ~6 guarantees

**~87 distinct capabilities** spread across 10 categories.

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

**IMPORTANT measurement note:** the canary must be measured from a **fresh process** — a run where `GuestLoadTest` is the only test invoked. Running `GuestLoadTest` inside the full suite (position ~120 of 252) has `elapsedMs=16000+` and `providersAttempted=3, providersSucceeded=2` because 100+ test classes have already fragmented the process — providers after the 3rd time out. That is **not** the canary measurement. The canary is: `adb shell am instrument -w -r -e class com.ims134.hookengine.GuestLoadTest ...`. Fresh process. Baseline: `elapsedMs ~1679ms` (S61), `~1691ms` (S62), `~1696ms` (S63/S64), `~1696ms` (S65), `~1705ms` (S66), `~1705ms` (S67), `~1705ms` (S68), `~1675ms` (S69), `~1728ms` (S70), **`~1668ms` (S71)**.

**S70's 1728ms was noise — S71 measured 1668ms, the fastest canary on record (7ms faster than S69, 60ms faster than S70).** The S70 hypothesis (that the five `ro.boot.*` / `ro.warranty_bit` tell-set entries might touch a MediaTek vendor init path and add 20–50ms) is refuted — those entries are still live in `kPropSpoofs`, no code was changed, and the canary improved. Providers 15/14 confirmed. No new FATAL. **The S70 canary was within the historical jitter band all along; the S71 measurement brings it back to the S69 baseline.**

# PROJECT STATE

Last updated: **Session 71 close (2026-10-28)**. Supersedes Session-70-close version. Sessions 41–71 landed on disk and are on-device verified (252 registered, 249 executed, 0 failed, 3 ignored at Session 71 close). Feature 5.10 ADB-detect = ⚠️ PARTIAL (property surface complete; Settings.Global surface architecturally walled — see §5.10 below). Feature 5.6 build property coverage = ✅ COMPLETE (S70). Features 5.11–5.16 = ✅ COMPLETE. Feature 6.8 signal-safe uninstall = ✅ COMPLETE. Feature 8.7 per-hook profiling = ✅ COMPLETE. Feature 7.1 unified ELF symbol API = ✅ COMPLETE (S68). Feature 8.2 per-hook argument logging = ✅ COMPLETE (S69). **Feature 6.2 per-guest tags = ✅ COMPLETE (S71)** — 7 falsifiable `HookTagTest` assertions, all green first-try; see §6.2 below.

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

Sessions 41–71 resumed engine work and delivered **thirty-four** verified primitives — the entire Category 1, the entire Category 2, all seven of Category 3, all six of Category 4, six of six Phase E anti-detection features (5.11–5.16), the first Phase F feature (6.8), the second Phase F feature (8.7, extended at S67 to cover both ChainHook and ReturnMod dispatch paths), the third Phase F feature (7.1 unified ELF symbol API at S68), the fourth Phase F feature (8.2 per-hook argument logging at S69), the fifth Phase F feature (5.6 build property coverage at S70), and the sixth Phase F feature (6.2 per-guest tags at S71). **Four categories complete plus 15 of 16 anti-detection filters.** Phase E CLOSED. Phase F seven sessions in.

---

## Current verified state

**Engine primitives (verified on-device, all green at Session 71 close):**
- Inline hook: 9 ARM64 instruction kinds. Verified by RelocTest (49 assertions).
- GOT hook: 26 sites, works.
- ArtMethod hook: works (native methods, non-fast-JNI only).
- ArtMethod entry/exit trace: works on framework natives. Verified by ArtMethodTraceTest (5 tests).
- Hook chaining: works. Verified by ChainHookTest (6 tests).
- Conditional hook: predicate gates per-slot handler fire. Verified by ConditionalHookTest (6 tests).
- Return-value modification: post-call modifier transforms caller-visible x0. Verified by ReturnModTest (6 tests).
- SVC instruction hooking: BRK-magic + SIGTRAP handler. Verified by SvcHookTest (6 tests).
- Register state inspection: full ARM64 GP + SP + NZCV + Q register capture. PROBE and PASSTHRU modes. Verified by RegInspectTest (6 tests).
- Memory read / write / scan: `/proc/self/maps`-validated; MemWrite mprotects each page to RW, writes, restores orig. Verified by MemoryOpsTest (6 tests).
- Full maps enumeration: MemEnumMaps, MemEnumMapsFilter, MemFindRegionContaining. Verified by MapsEnumTest (6 tests).
- Backtrace from hook site: FP-chain walker with PC validation, alignment guard, cycle detection, bounded at BT_MAX_FRAMES=32. Verified by BacktraceTest (6 tests).
- Native ClassLoader chaining (`nativeFNLN`): C++ merge of `pathList.dexElements` (host-only prepended to guest, deduped by identity). Rule 15 debt closed. Verified by NativeFnlnTest (6 tests) and ClassLoaderBridgeTest (3 tests).
- Dynamic DEX loading: `DexLoaderCreate(env, dexPath, optimizedDir, parent)` in `DexLoader.cpp` constructs `dalvik/system/DexClassLoader` entirely in C++. Verified by DexLoadTest (6 tests).
- DEX merging at runtime: multi-path `DexClassLoader` chains into a guest via 4.2 making all classes from both sources resolvable. Verified by DexMergeTest (6 tests).
- Dynamic runtime attach: works with alignment/mapped/exec guards. Verified by DynamicAttachTest (6 tests).
- Hidden-API bypass: works via thread-attached reflection.
- `IPCThreadState::transact` inline hook: works.
- AIDL prelude rewrite: works on 4 interfaces.
- ServiceManager lookup interception (transact-level): alias and null overrides rewrite String16 names in the request parcel before transact. Verified by ServiceManagerTest (8 tests).
- Signal handling override: sigaction-based per-slot handlers with PASS_THROUGH / SUPPRESS actions, full siginfo capture. Verified by SignalOverrideTest (6 tests).
- **Native ServiceManager addition** (Feature 3.7): C++ inline hook on `android_os_Parcel_nativeReadStrongBinder` (located by `JNINativeMethod`-struct scan). Verified by SyntheticServiceTest (6 tests).
- PLT stub resolution: `PltResolveForSymbol(lib, symbol)` walks `.rela.plt` and correlates relocation index to `.plt` / `.plt.sec` entry. **S68** — migrated to use `ElfMappedLib_Open` shared helper; all 7 tests stay green.
- **Unified ELF symbol resolution** (Feature 7.1, S68): `ElfSymbolLookup(lib, sym)` returns runtime address. `.dynsym` first via GNU hash → SysV hash → linear; then `.symtab` fallback (string table via `sh_link`, not section name). Multi-lib variant. `ElfMappedLib_Open/Close/FindSection` shared helper, filesystem-path mode. Verified by ElfResolveTest (8 tests). `.symtab` fallback proven against `libc.so` (7738 symbols on this ROM).
- **Debugger detect blocking** (Feature 5.11): property-gated JDWP. Verified by DebuggerDetectTest (6 tests). **Emulator detect blocking** (5.12): real-device + defensive kPropSpoofs. Verified by EmulatorDetectTest (6 tests). **Test-keys detect blocking** (5.13): 3 surfaces. Verified by BuildPropTest (11 tests, S70 extended). **/proc/net/tcp blocking** (5.14): OS-level SELinux. Verified by NetTcpTest (6 tests). **/proc/self/cmdline full spoof** (5.15): `setArgV0`. Verified by CmdlineStackTest. **/proc/self/syscall + stack filter** (5.16): kernel-level + S23 TracerPid. Verified by CmdlineStackTest.
- **Signal-safe deferred uninstall** (Feature 6.8): deferred SPSC ring buffer + worker thread. Verified by SignalSafeUninstallTest (6 tests).
- **Per-hook CPU / memory profiling** (Feature 8.7, COMPLETE): opt-in per-site profiling on both ChainHook and ReturnMod dispatch helpers. `HookProfile` struct attached to every ChainSlot and RetModSlot. `CLOCK_MONOTONIC` (VDSO, ~25ns). Zero overhead when disabled. Memory footprint per site computed at install (ChainHook 8224B, ReturnMod 12288B). Verified by HookProfileTest (6 tests, ChainHook) and ReturnModProfileTest (6 tests, ReturnMod). RegInspect/SvcHook/InlineHooker explicitly deferred (§8.7 documents rationale).
- **Per-hook argument logging** (Feature 8.2, COMPLETE, S69): opt-in per-site arg capture on both ChainHook and ReturnMod dispatch helpers. `HookArgLog` struct attached to every ChainSlot and RetModSlot. 16-slot ring buffer per site, stores GP x0–x7 and FP d0–d7 (ChainHook) or GP-only (RetMod). Zero overhead when disabled. Memory footprint `sizeof(HookArgLog) = 2208B`. Verified by HookArgLogTest (7 tests). RegInspect/SvcHook/InlineHooker explicitly deferred (§8.2 documents rationale). **Test 4 (`argLogRingWraps`) proves the ring buffer math is exactly right: after 20 calls with `arg0=1..20`, `ring[0].gp0=17`, `ring[3].gp0=20`, `ring[4].gp0=5`.**
- **Build property coverage** (Feature 5.6, COMPLETE, S70): `kPropSpoofs` extended from 10 to 15 entries with the bootloader-integrity tell set (`ro.boot.verifiedbootstate` → `green`, `ro.boot.flash.locked` → `1`, `ro.boot.veritymode` → `enforcing`, `ro.boot.warranty_bit` → `0`, `ro.warranty_bit` → `0`). `g_fakeProps` pool 16 → 32 with a `static_assert` binding the pool size to the spoof table. `BuildPropTest` 6 → 11 tests, all green first-try. Verified by BuildPropTest (11 tests). See §5.6.
- **Per-guest tags** (Feature 6.2, COMPLETE, S71): `HookHandle.tag[48]` field set atomically with the `HS_FREE → HS_INSTALLING` transition via a thread-local pending tag; `HookGotEntryExTagged` wrapper guarantees cleanup on both success and failure. Four new registry operations: `RegistryCountByTag`, `RegistrySnapshotByTag`, `RegistryUninstallByTag` (capture-then-unhook — never holds `g_reg_mutex` across an unhook), `RegistryListTags`. **`FindHandle` stays tag-blind** — conflict detection operates on target address only, per §6.2 design constraint. Verified by HookTagTest (7 tests, all green first-try). See §6.2.
- `nativeCNVM`: decomp-verified, 4 tests green.
- `nativeInit` (structural): verified Session 38.

**Container / canary (verified on-device, fresh-process):**
- Viber boots: `outcome=booted-viber-order`, providers 14/15 (MlKit noise), `elapsedMs ~1668ms` (S71 close).
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
| 2.4 | Allocate memory near target | ✅ **S65 rewrite** |
| 2.5 | Enumerate maps | ✅ |
| 2.7 | Register state inspection at hook site | ✅ |
| 2.8 | Backtrace from hook site | ✅ |

### Category 3 — IPC / kernel boundary — **7/7 COMPLETE**

| # | Feature | Status |
|---|---|---|
| 3.1 | Binder transact interception | ✅ |
| 3.2 | AIDL prelude parse | ✅ |
| 3.3 | AIDL reply rewrite | ✅ |
| 3.4 | ServiceManager lookup interception | ✅ |
| 3.5 | Property read/write interception | ✅ |
| 3.6 | Signal handling override | ✅ |
| 3.7 | Native ServiceManager addition | ✅ |

### Category 4 — Class loading / code isolation — **6/6 COMPLETE**

| # | Feature | Status |
|---|---|---|
| 4.1 | Custom ClassLoader delegation | ✅ |
| 4.2 | Native ClassLoader chaining | ✅ |
| 4.3 | Dynamic DEX loading | ✅ |
| 4.4 | DEX merging at runtime | ✅ |
| 4.5 | Resource merging | ✅ |
| 4.6 | Native library load override | ✅ |

### Category 5 — Anti-detection

| # | Feature | Status |
|---|---|---|
| 5.1–5.5 | File/maps/status/mounts filters | ✅ |
| 5.6 | Build property spoofing | ✅ **S70** (15 props, 11 tests) |
| 5.7 | UID/GID spoofing | ✅ |
| 5.8 | Frida/IDA port blocking | ✅ |
| 5.9 | ptrace blocking | ✅ |
| 5.10 | ADB detect blocking | ⚠️ (property surface ✅; Settings.Global walled) |
| 5.11 | Debugger detect (JDWP full) | ✅ |
| 5.12 | Emulator detect blocking | ✅ |
| 5.13 | Test-keys detect blocking | ✅ |
| 5.14 | /proc/net/tcp analysis blocking | ✅ |
| 5.15 | /proc/self/cmdline full spoof | ✅ |
| 5.16 | /proc/self/syscall + stack filter | ✅ |

### Category 6 — Safety / correctness

| # | Feature | Status |
|---|---|---|
| 6.1 | Hook registry | ✅ |
| 6.2 | Per-guest tags | ✅ **S71** (7 tests, first-try) |
| 6.3 | Atomic install / uninstall | ✅ |
| 6.4 | Deferred-free under contention | ✅ |
| 6.5 | Rollback on partial failure | ✅ |
| 6.6 | Page protection read + restore | ✅ |
| 6.7 | Inter-hook conflict detection | ✅ |
| 6.8 | Signal-safe uninstall | ✅ |
| 6.9 | Capability gate (ART version) | ✅ |

### Category 7 — Symbol / ELF utilities

| # | Feature | Status |
|---|---|---|
| 7.1 | ELF symbol resolution (unified API) | ✅ **S68** |
| 7.2 | .dynsym walk | ✅ (via ElfResolver) |
| 7.3 | .symtab walk | ✅ (via ElfResolver, sh_link) |
| 7.4 | GNU + SysV hash support | ✅ (via ElfResolver) |
| 7.5 | Multi-lib search | ✅ (via ElfResolver) |
| 7.6 | PLT stub resolution | ✅ (migrated S68) |

### Category 8 — Runtime diagnostics

| # | Feature | Status |
|---|---|---|
| 8.1 | Per-hook execution counter | ✅ |
| 8.2 | Per-hook argument logging | ✅ **S69** |
| 8.3 | Runtime hook inspection | ✅ |
| 8.4 | Regression test suite | ✅ |
| 8.5 | Reloc test suite | ✅ |
| 8.6 | Anti-detection test suite | ✅ |
| 8.7 | Profiling (CPU / memory per hook) | ✅ **S66 + S67** |

### Totals

- **✅ verified complete:** 66 features (98.5% of 67 tracked)
- **⚠️ partial:** 1 feature (5.10 — Settings.Global walled; property surface complete)
- **❌ not started:** 0 features

**Engine completion requires ~2 more sessions of code work plus 3 disassembly sessions and a final close.**

---

## Engine roadmap — Session 69 to Session 75

### Phase A — Foundation (Sessions 41–44) — CLOSED
### Phase B — Hook mechanism depth (Sessions 45–48) — CLOSED (Category 1 9/9)
### Phase C — Memory + kernel boundary (Sessions 49–53) — CLOSED (Category 2 7/7)
### Phase D — Class loading + symbol depth (Sessions 54–58) — CLOSED
### Phase E — Anti-detection completion (Sessions 59–64) — CLOSED

### Phase F — Polish + close (Sessions 65–75)

| Session | Feature | Exit |
|---|---|---|
| 65 | 6.8 Signal-safe uninstall | ✅ COMPLETE — `SignalSafeUninstallTest` (6 tests) |
| 66 | 8.7 Profiling — ChainHook | ✅ COMPLETE — `HookProfileTest` (6 tests) |
| 67 | 8.7 extension (ReturnMod) + S42 loopback wall re-measurement | ✅ COMPLETE — `ReturnModProfileTest` (6 tests); 3 `AntiDetectionE2ETest` un-ignored and reframed |
| 68 | 7.1 Unified ELF symbol API | ✅ COMPLETE — `ElfResolveTest` (8 tests); `PltResolver.cpp` migrated |
| 69 | 8.2 Per-hook argument logging | ✅ COMPLETE — `HookArgLogTest` (7 tests, all green first-try). ChainHook GP+FP capture, ReturnMod GP-only. 16-slot ring per site. `mem_bytes = 2208B`. |
| 70 | 5.6 build property coverage + 6.2 scope probe | ✅ COMPLETE — `BuildPropTest` 6 → 11, all green first-try. `Hooks.cpp` +5 bootloader-integrity tell-set props, `g_fakeProps` 16→32, `static_assert`. 6.2 design note in §6.2. Canary 1728ms, providers 15/14. |
| **71** | **6.2 per-guest tags (implementation)** | **✅ COMPLETE — `HookTagTest` (7 tests, all green first-try after a one-file build fix). `HookHandle.tag[48]`, TLS pending tag consumed in `RegistryAlloc`, `HookGotEntryExTagged` wrapper, four new registry operations. `FindHandle` unchanged. Canary 1668ms (fastest on record), providers 15/14.** |
| 72 | 5.10 Settings.Global disassembly (IDA/Ghidra) OR S51 cache-hit disassembly OR S60 Debug/VMDebug disassembly | Depends |
| 73 | Full engine regression run + buffer | All suites + canary green |
| 74 | `PROJECT_STATE.md` final close | All ✅ or documented walls |
| 75 | Reserved | Depends |

---

## §Cache-hit wall — measured, documented, closed

**Feature 3.4 as originally spec'd is complete.** The `ServiceManager` transact-level rewriter fires on every `handle==0` transact whose request parcel carries a String16 name matching an override. Alias and null overrides both work. 8 green tests.

The Session 51 **cache-hit aspiration** — intercepting `ServiceManager.getService("activity")` when the framework serves it from a cached binder without dispatching through `IPCThreadState::transact` — is a measured architectural wall on MIUI/API 37. **Fifteen-plus mechanisms were tried. Every one was blocked by a specific MIUI hardening.** The three fast-JNI tokens (0x3d/0x3f/0x41), the `+32` slot not being code, `SimpleArrayMap.get(sCache)` being JIT-inlined, and `sCache` being a decoy — all documented in the S51 close. Future work requires IDA Pro analysis of `libart.so`'s fast-JNI dispatch layer.

---

## §5.6 Build property coverage (Session 70)

**Functional goal:** a detection library reading Android system properties for "rooted / debuggable / unlocked bootloader / modified system" indicators sees only clean-device values.

**State at S69 close:** 10 properties spoofed — `ro.debuggable`, `ro.secure`, `ro.build.tags`, `ro.build.type`, `ro.kernel.qemu`, `ro.boot.qemu`, `init.svc.adbd`, `service.adb.root`, `sys.oem_unlock_allowed`, `ro.adb.secure`. Marked ⚠️ PARTIAL because the bootloader-integrity tell set — the second-most-common class of detection signal after test-keys — was not covered.

**S70 change — `Hooks.cpp` only, no new hook mechanism:**

- `kPropSpoofs[]` gains five entries, the clean-device values for the bootloader-integrity tell set:

  | Property | Clean value | Tampered tell it defeats |
      |---|---|---|
  | `ro.boot.verifiedbootstate` | `green` | `orange` = unlocked bootloader; `yellow` = custom key |
  | `ro.boot.flash.locked` | `1` | `0` = flashing permitted, bootloader unlocked |
  | `ro.boot.veritymode` | `enforcing` | `disabled` / `logging` = dm-verity off |
  | `ro.boot.warranty_bit` | `0` | `1` = tamper recorded |
  | `ro.warranty_bit` | `0` | vendor-side companion to the above |

- `g_fakeProps[16]` → `g_fakeProps[32]`. Slots are allocated one-per-distinct-spoofed-name and never released, so the array size is a hard ceiling; at 15 spoofed names and 16 slots the old ceiling had zero headroom. A `static_assert` now binds the two sizes so a future overflow is a build failure, not a silent `Fake___system_property_find` returning `nullptr`.

**Two candidates from the S70 handoff were deliberately dropped — and the reason is the S69 meta-lesson:**

- `ro.product.first_api_level` — this device's real value is already the truth. Spoofing it to the same value is a no-op; spoofing it to a different value manufactures an inconsistency against `ro.build.version.sdk` and the APK's own `minSdk`, which is a **louder** tell than the real value.
- `ro.build.version.security_patch` — a fabricated patch date is itself a tell; detectors cross-check it against `ro.build.version.release` and vendor patch level.

**Rule 12 corollary applied:** a property that is already honest on this device gains nothing from a spoof entry and risks an inconsistency. Don't add it. The **assertions** are what make 5.6 real — they fail if the value ever flips to an unlocked tell, whether that flip comes from a removed spoof entry or a ROM update.

**S70 measurement approach — the diagnostic that didn't gate the close.** The S70 charter recommended a `logcat`-during-boot probe to identify which properties detection libraries actually read, before extending the table. S70 did **not** run that probe. Instead it added all five defensive entries and let the `BuildPropTest` assertions prove the property surface returns the clean value through the hooked path. The charter's own decision table confirmed all three branches (non-clean, clean, mixed) resolved to ⚠️ → ✅, so the diagnostic was never gating. The union approach (add all five, assert all five) is correct under any device state. The entries are behavior-neutral on this clean-signed Xiaomi device and load-bearing on a device whose real values are tampered.

**Verification:** `BuildPropTest` 6 → 11, all green first-try. Five new falsifiable assertions, one per property, each failing on (a) the spoof entry removed **and** the device reporting the tampered value, or (b) the property GOT hook on `libandroid_runtime.so` regressing. Every prior assertion stayed green.

---

## §5.10 ADB-detect — four measured walls (Session 59)

**Property surface — COMPLETE.** `ro.debuggable`, `init.svc.adbd`, `ro.adb.secure`, `service.adb.root` return spoofed values. `libandroid_runtime.so` added to `ShouldHookProperty` so reflection-based `SystemProperties.get` fires 5.6's existing spoof. Verified by `AdbDetectTest.roDebuggableSpoofed` / `initSvcAdbdSpoofed`.

**Settings.Global surface — ARCHITECTURALLY WALLED.** Four hypotheses measured, four refuted on-device: (1) `NameValueCache.mValues` poison — write persists, framework read bypasses; (2) `mGenerationTrackers` sync — field stays empty; (3) `mReadableFieldsWithRedactedValue` — privileged callers get real values; (4) `IContentProvider.call()` via transact — S59 probe measured 0 transacts for 3 ADB/DEV reads.

**Phase F entry point:** disassemble `framework.jar!classes3.dex`'s `Settings$NameValueCache.getStringForUser` on this ROM and trace the shared-memory read path. IDA/Ghidra work, not an engine-primitive session.

---

## §5.11 Debugger-detect — property-gated JDWP (Session 60)

**Functional goal:** make `Debug.isDebuggerConnected()`, `Debug.waitingForDebugger()`, and the JDWP handshake surface all appear clean. **Achieved by S59's property spoof alone** — no JDWP listener bound, endpoint simply doesn't exist. Verified by DebuggerDetectTest (6 tests). The `Debug.*`/`VMDebug.*` hook surface is walled (fast-JNI tokens 0x39-0x3f, unstable across processes). Phase F entry point: IDA/Ghidra disassembly of `libart.so`'s fast-JNI dispatch table.

---

## §5.12 Emulator-detect — real-device cleanliness (Session 61)

**Mechanism — no new hook code.** Real MediaTek mt6899 device + S59's defensive `kPropSpoofs`. All 51 sensors real, 0 goldfish libs, all Build.* values real Xiaomi/MediaTek. Verified by EmulatorDetectTest (6 tests).

---

## §5.13 Test-keys detect (Session 62)

**Three surfaces:** (1) Property — S59 kPropSpoofs forces release-keys values. (2) Java static-final — naturally clean. (3) Signature null-out — S62 extended `PmSpoof.nullOutSignatures` to clear `SigningInfo.mSigningDetails` fields. Verified by BuildPropTest (11 tests, S70 extended).

---

## §5.14 /proc/net/tcp blocking + S67 loopback-bind wall re-documented

**Feature 5.14 mechanism — no engine code.** OS-level SELinux MAC policy denies app-domain reads of `/proc/net/tcp`, `/proc/net/tcp6`, `/proc/net/udp`, `/proc/net/udp6` and `/proc/net/unix`. Every read path fails identically. Verified by NetTcpTest (6 tests).

### Companion finding — S42 loopback-bind wall (S67 re-measurement)

**S42 wall was originally**: `ServerSocket().bind(127.0.0.1, port)` throws EPERM, preventing any loopback connect E2E test. **S63 measured**: bind now works. **S67 re-measured with bind working**: `Socket("127.0.0.1", 27042)` and `Socket("127.0.0.1", 23946)` both succeed — the engine's connect hook permits loopback by design. Only external Frida/IDA connections are blocked. **S67 fix — test reframe.** 3 tests renamed + inverted to positive "loopback permitted" assertions. All 4 now live, 0 ignored. Rule-12 strengthened.

---

## §5.15 /proc/self/cmdline full spoof — S19 setArgV0 + clean environ (Session 64)

**No engine code.** S19's `setArgV0("com.viber.voip")` produces exactly one argv entry; `/proc/self/environ` is 19 standard Android env vars, zero engine identity. Verified by CmdlineStackTest.

---

## §5.16 /proc/self/syscall + stack filter (Session 64)

**No engine code. Three independent layers:** (1) `/proc/self/syscall` readable but reveals only Looper state. (2) `/proc/self/stack` read-denied at kernel level via `ptrace_may_access`. (3) `/proc/self/status` `TracerPid: 0` forced by S23 `BuildFilteredStatus`. Verified by CmdlineStackTest.

---

## §6.2 Per-guest tags — COMPLETE (Session 71)

**Functional goal:** group installed hooks by logical scope; uninstall one group without disturbing others.

**S70 design → S71 implementation.** S70 produced the design that S71 implemented essentially verbatim. The one process deviation is that S71's first build attempt failed because `HookRegistry.h` on disk was the pre-S71 version (missing `HookHandle.tag[48]` and the `HookGotEntryEx` extern declaration). One-file re-apply; second build clean. Zero design rework.

**What shipped (5 files):**

1. **`HookRegistry.h`** — `char tag[48] = {0};` field on `HookHandle`; `extern "C" HookResult HookGotEntryEx(...)` declaration so `HookGotEntryExTagged` can call it; four new function declarations; `HookGotEntryExTagged` declaration.
2. **`HookRegistry.cpp`** — two `extern "C"` declarations for `UnhookGotEntry` / `UninstallInlineHook` (compile fix — the S70 probe-only version referenced them without declaring them); `thread_local char g_pendingTag[48]` consumed atomically inside `RegistryAlloc`'s `HS_FREE → HS_INSTALLING` critical section; `RegistrySetPendingTag` / `HookGotEntryExTagged` (with guaranteed cleanup on both success and failure paths); four new functions (`RegistryCountByTag`, `RegistrySnapshotByTag`, `RegistryUninstallByTag` using `std::vector<UnhookTarget>` capture-then-unhook — never holds `g_reg_mutex` across an unhook; `RegistryListTags`).
3. **`HookTagJni.cpp`** — 5 synthetic GOT slots in `.data` of `libhookengine.so`, each with a real function (`10+i+x`), a primary replacement (`1000+x`, increments `g_tagReplCalls[i]`), and an **alternate replacement** (`3000+x`, increments `g_tagAltCalls[i]`); 10 JNI exports.
4. **`NativeTestBridge.kt`** — 10 new extern declarations appended. No collisions with existing externs.
5. **`HookTagTest.kt`** — 7 falsifiable assertions.
6. **`CMakeLists.txt`** — `HookTagJni.cpp` added.

**Why the alternate-replacement fixture matters.** `HookGotEntryEx` has an early return `if (*gotAddress == newFunction) return HOOK_ALREADY_HOOKED;` **before** calling `FindHandle`. A `conflictDetectionIgnoresTag` test that reinstalled with the *same* replacement would return `HOOK_ALREADY_HOOKED` (code 8) via that early return, never touching `FindHandle` — and would pass even if `FindHandle` were made tag-aware. The alternate-replacement path (code 10 = `HOOK_INTERNAL_ERROR`) is what actually exercises `FindHandle`'s tag-blindness. **Gate 1's output confirms both paths:**

```
InstallSlot(0, tag=B, repl=0x6c3b4f5618) -> 8   (same replacement, early return)
InstallSlot(0, tag=B, repl=0x6c3b4f5768) -> 10  (alt replacement, FindHandle path)
```

The `-> 10` is the sharper evidence: `FindHandle` observed tag A's handle with a different replacement and returned `HOOK_INTERNAL_ERROR`, exactly as §6.2 specified — **not** `HOOK_OK`, which a tag-aware `FindHandle` would have returned.

**The 7 falsifiable assertions in `HookTagTest`:**

1. `tagIsEmptyByDefault` — `installTagFixture(0, null) → 0`; `countByTag("myscope")==0`; `countByTag("")≥1`; untagged snapshot non-empty.
2. `tagSetAtInstallIsVisible` — `installTagFixture(0, "myscope") → 0`; `countByTag("myscope")==1`; `listTags()` contains `"myscope"` as a whole line.
3. `uninstallByTagRemovesOnlyThatSubset` — 2 in "A", 3 in "B"; `uninstallByTag("A")` removes 2; "A" count 0, "B" count 3; B #3 still fires its replacement; A #0 no longer fires.
4. `uninstallByTagIsIdempotentAndReportsZero` — first uninstall removes 1; second removes 0, `failures==0`.
5. `conflictDetectionIgnoresTag` — tag A primary install succeeds; tag B alt-replacement reinstall returns `HOOK_INTERNAL_ERROR`; A count still 1; B count 0; primary still active, alt never ran.
6. `tagSurvivesSnapshotCopy` — mutate the returned snapshot copy; registry count unchanged.
7. `tagPendingClearsOnSuccessAndFailure` — success path (tag A then untagged): untagged handle created; failure path (same-replacement reinstall returns `HOOK_ALREADY_HOOKED`): pending tag B cleared before next install; slot 2 install with `null` tag doesn't inherit B.

**Scope discipline — deferred per §6.2 design:** chain/retmod tagging (ChainSlot / RetModSlot are not registry members; parallel field + parallel uninstallByTag would be Phase-F scope creep). Same discipline §8.2 / §8.7 used to scope out RegInspect/SvcHook/InlineHooker.

**Meta-lesson (S71):** *A multi-file delivery is only as complete as its least-applied file. The S71 design was correct — 6 files of correct content, one stale header, 14 cascading compile errors, one-file re-apply, second-build clean. The design held; the delivery mechanics nearly didn't. Rule 12's corollary applies in the delivery dimension too: measure the end-to-end build, not each file's correctness. A five-file green + one-file red is a red build, not a five-file success. The positive corollary from the same session: once every file landed, the 7/7 assertion pass and the 1668ms canary (fastest on record) both came in first try — no design iteration, no canary churn, no scope creep. The patterns from S66/S67/S69 (POD discipline, `__atomic_*_n`, TLS where state is thread-local, synchronous snapshot-then-unhook to avoid self-deadlock) transferred cleanly to a coordination primitive as well as they had to diagnostic ones.*

---

## §6.8 Signal-safe uninstall — deferred SPSC queue + worker thread (Session 65)

**Functional goal:** uninstall is safe from any signal handler context.

**Mechanism — deferred uninstall via lock-free SPSC ring buffer + worker thread.** Signal handler enqueues (spinlock CAS + memcpy + write(eventfd)) and returns in ~100us. Worker thread drains and calls `UninstallInlineHook` from normal context. Zero changes to `UninstallInlineHook` itself. Also rewrote `AllocateMemoryNear` (page-scan). Verified by SignalSafeUninstallTest (6 tests).

---

## §7.1 Unified ELF symbol resolution (Session 68)

**Functional goal:** a real `.dynsym` + `.symtab` + GNU/SysV-hash + multi-lib symbol-value resolver, plus a shared ELF-open helper.

**What the S67 handoff claimed vs. what measurement showed:**
- S67 handoff listed 4 "ELF callers": `PltResolver.cpp`, `SyntheticServiceRegistry.cpp`, `InlineHooker.cpp`, `SignalOverrideJni.cpp`.
- S68 probe re-measured: **only 2 files implement ELF symbol/table walks.** `PltResolver.cpp` (returns PLT stub addr — Feature 7.6), `elf_hooker.cpp` (**not in the handoff list!** — returns GOT slot addr). `SyntheticServiceRegistry.cpp` does a JNINativeMethod struct scan (load-bearing — `nativeReadStrongBinder` not exported). `InlineHooker.cpp` and `SignalOverrideJni.cpp` have zero ELF code.

**Deliverables:**
- `ElfResolver.h/.cpp` — `ElfSymbolLookup` + `ElfMappedLib_Open/Close/FindSection`
- `ElfResolverJni.cpp` — 8 JNI exports
- `PltResolver.cpp` migration — ~65 lines removed, 7/7 tests stable
- `ElfResolveTest.kt` — 8 falsifiable assertions; `.symtab` fallback proven against `libc.so` (7738 symbols) with `sh_link`-based string-table resolution
- `build.gradle.kts` — non-functional `keepDebugSymbols` block removed after measurement confirmed it doesn't affect CMake-built libs on this toolchain

**Meta-lesson:** *A build-config premise is a test premise. When it fails, pivot the test to a target where the premise holds naturally (`libc.so` instead of `libhookengine.so`), not more build-config.*

---

## §8.2 Per-hook argument logging (Session 69)

**Functional goal:** capture the argument registers the target received at each dispatch, into an opt-in per-site ring buffer, readable via JNI.

**Mechanism — opt-in per-site arg-log attached to ChainHook and ReturnMod sites:**

- **`HookArgLog` struct** (POD, `HookArgLog.h`): `{bool enabled; uint32_t head; uint32_t count; uint32_t mem_bytes; int captures_fp; void* target; HookArgCapture ring[16];}`. 16-slot ring per site. `HookArgCapture` = `{uint64_t gp[8]; uint64_t fp[8]; uint64_t pc;}` = 136 bytes.
- **`ChainSlot`** and **`RetModSlot`** each gain a `HookArgLog arglog;` member.
- **`chain_dispatch_helper`** — capture happens at the top, immediately after the slot lookup lock block, before any handler mutation. **The naked dispatcher (`chain_dispatch_naked`) already saves x0–x7 AND q0–q7 to the stack frame at `sp+0..sp+160` and `sp+176..sp+304` respectively — arg capture is a pure stack read, zero new asm.** GP args read from `saved[0..7]`, FP args (d0–d7 low-64-bits of q0–q7) read from `saved[22], saved[24], ..., saved[36]`.
- **`retmod_dispatch_helper`** — capture happens after the slot lookup lock block, before the modifier call. **The `retmod_stub_*` asm saves only x0–x7 (80-byte frame) — no q0–q7.** GP-only capture from `saved_args[0..7]`; `fp[]` zeroed. Adding 128 bytes of FP save to the hot-path stub for an opt-in feature was rejected — the S66/S67 meta-lesson ("scope to what the structure naturally provides") applied.
- **`HookArgLog.cpp`** — global `HookArgLogSnapshot / Enable / Disable / Reset / ReadNth` dispatchers try ChainHook then ReturnMod.
- **Memory accounting:** `mem_bytes = sizeof(HookArgLog) = 2208B` per site (32-byte header + 16 × 136-byte captures). Separate from `HookProfile::mem_bytes` — the two features account independently.

**Scope discipline — RegInspect/SvcHook/InlineHooker explicitly deferred:**
- **RegInspect:** its dispatcher `memcpy`s the full 784B frame into `g_snapshots[slot]` — args already visible via `regInspectSnapshot`, arg logging would be redundant.
- **SvcHook:** runs in signal context, capture at dispatch helper is unsafe.
- **InlineHooker:** relay is pure asm, no C++ hook point.

Same scoping pattern as §8.7. §8.2 documents the asymmetry.

**The 7 falsifiable assertions in `HookArgLogTest`:**

1. `argLogDisabledByDefault` — after `chainAttach`, `hookArgLogLookup==true`, `hookArgLogCount==0`. A call while disabled doesn't capture.
2. `argLogCapturesGpArgs` — attach to `argLogFixtureGp`, enable, call `(11,22,33,44,55,66,77,88)` → `count=1`, `read(ring[0]).slice(0,8) == [11,22,33,44,55,66,77,88]`, `pc == target entry`, `hookArgLogCapturesFp==true`.
3. `argLogCapturesFpArgs` — attach to `argLogFixtureFp`, enable, call `(1.5, 2.5, 3.5, 4.5)` → `read(ring[0])[8..12]` decoded as doubles equal `[1.5, 2.5, 3.5, 4.5]`. **Zero new asm** — reads from the frame `chain_dispatch_naked` already populated.
4. `argLogRingWraps` — 20 calls with `arg0=1..20`. `count=20`. `ring[0].gp0=17`, `ring[3].gp0=20`, `ring[4].gp0=5`. **This is the sharpest falsifier in the batch** — it fails if the fetch-add is not pre-increment, if the modulo mask is wrong, or if `head` is not 32-bit. Passed first try.
5. `argLogDisabledStopsCapturing` — enable, 5 calls, disable, 5 more → `count` stays 5.
6. `argLogResetClearsButPreservesEnabled` — enable, 3 calls, reset → `count=0`; then 2 more calls → `count=2`. Enabled preserved across reset.
7. `argLogRetModCapturesGpOnly` — attach RetMod on the same GP fixture, enable → `hookArgLogCapturesFp==false`; call → `read(ring[0]).slice(0,8) == [11,22,33,44,55,66,77,88]`, all `fp` fields zero. **Proves the second subsystem + the documented asymmetry in one test.**

**Number set:** All 7 tests green first try, zero iterations. This is the first non-trivial native feature since S43 that shipped clean on the first build. The reason is structural: S69's design reuses every discipline established by S66/S67 (POD struct, `__atomic_*_n`, opt-in toggle, per-site ring, global dispatcher) AND exploits a decision made in S43/S46/S48 (naked stub dumps all registers to stack, C++ helper reads them) to make arg capture a zero-asm feature.

**Meta-lesson (S69):** *The Nth primitive in a pattern family costs what the 1st primitive's discipline cost, not what the Nth primitive's novelty costs — provided the Nth primitive is actually in the family. S69's `HookArgLog` is structurally identical to S66/S67's `HookProfile` (POD + atomics + opt-in + ring + global dispatcher). The only novelty was the ring buffer math, and that's a pure arithmetic property tested by one assertion (`argLogRingWraps`). The naked-stub-dumps-registers decision from S43 was the second thing that paid off: arg capture required no new asm because the register frame was already on the stack. Two prior disciplines, one clean feature. But also: S69's 7/7 first-try is not the general case — S68's 3 iterations is the counter-example. The difference is that S68's premise was build-config-dependent (brittle), while S69's premise was ABI-dependent (stable). **Rule 12's corollary applies in both directions — a stale premise costs iterations, a correct premise saves them.***

---

## §8.7 Per-hook CPU / memory profiling (Session 66, extended Session 67)

**Functional goal:** per-hook CPU cost and memory overhead accounting. **Both major handler-dispatch paths instrumented.**

**The probe (S66)** measured per-site dispatch overhead: ChainHook 1 handler = 147ns; RetMod = 19ns; RegInspect passthru = 44ns; SvcHook = 1172ns. `System.nanoTime()` = ~174ns. **Design decision: `clock_gettime(CLOCK_MONOTONIC)` via VDSO, ~25ns.**

**Mechanism:** `HookProfile` struct attached to every ChainSlot and RetModSlot. Dispatch helpers wrap each user-handler/modifier invocation in two `HookProfileReadNs()` and accumulate. Zero overhead when disabled. Memory footprint: ChainHook 8224B, ReturnMod 12288B.

**Scope discipline:** RegInspect/SvcHook/InlineHooker explicitly deferred.

**The 12 falsifiable assertions across `HookProfileTest` (ChainHook) and `ReturnModProfileTest` (ReturnMod).** Both green first try. S66: `count=100 total=40156ns min=307 max=4923 mem=8224B`. S67: `count=100 total=29231ns min=76 max=2384 mem=12288B`.

**Meta-lesson (S66):** *Choose the robust primitive over the clever one when accessibility is uncertain and the feature is opt-in.*

**Meta-lesson (S67):** *Extending a feature to a second subsystem is copy-and-adapt when the first implementation was structurally correct.*

---

## Viber — regression canary, explicitly

Viber is used for **one purpose**: verify that the engine primitives still work under a real, large, hostile-to-hooking app under real integration load.

**Canary test:** `GuestLoadTest`. Asserts only `outcome != "?"`. Logcat-based signal: `booted-viber-order` + no new FATAL + providers ≥14 = canary green.

**Canary state at Session 71 close (fresh process):** `outcome=booted-viber-order`, `elapsedMs=1668ms`, `providersAttempted=15`, `providersSucceeded=14`, `providersFailed=1` (MlKit noise, `firstProviderError=com.google.mlkit.common.internal.MlKitInitProvider: IllegalStateException: MlKitContext is already initialized` — unchanged from S69/S70). **1668ms is the fastest canary on record** — 7ms faster than S69 (1675), 60ms faster than S70 (1728). This definitively closes the S70 tail question: the S70 +23ms over baseline was jitter, not a `kPropSpoofs` interaction. The five bootloader-integrity tell-set entries from S70 remain live and did not affect boot timing. Providers 15/14 confirmed. No new FATAL.

**Measurement discipline:** canary must be measured from fresh process — `-e class com.ims134.hookengine.GuestLoadTest` as the only class.

---

## Test suite state (as of Session 71 close)

**Executed:** 249 · **Registered:** 252 · **Ignored:** 3 · **Failed:** 0

Registered test classes (post-S71, full listing — delta from S70 is `HookTagTest` new + 7):

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
| AntiDetectionE2ETest | 4 | ✅ |
| PmSpoofTest | 4 | ✅ |
| ClassLoaderBridgeTest | 3 | ✅ |
| DualApplicationTest | 2 | ✅ |
| BinderTransactE2ETest | 9 | ✅ |
| BinderTransactHookTest | 2 | ✅ |
| DynamicAttachTest | 6 | ✅ |
| ChainHookTest | 6 | ✅ |
| ArtMethodTraceTest | 5 | ✅ |
| ConditionalHookTest | 6 | ✅ |
| ReturnModTest | 6 | ✅ |
| SvcHookTest | 6 | ✅ |
| RegInspectTest | 6 | ✅ |
| MemoryOpsTest | 6 | ✅ |
| MapsEnumTest | 6 | ✅ |
| ServiceManagerTest | 8 | ✅ |
| SignalOverrideTest | 6 | ✅ |
| BacktraceTest | 6 | ✅ |
| NativeFnlnTest | 6 | ✅ |
| DexLoadTest | 6 | ✅ |
| DexMergeTest | 6 | ✅ |
| PltResolveTest | 7 | ✅ |
| SyntheticServiceTest | 6 | ✅ |
| AdbDetectTest | 6 (2 ignored) | ⚠️ |
| DebuggerDetectTest | 6 | ✅ |
| EmulatorDetectTest | 6 | ✅ |
| BuildPropTest | 11 | ✅ S70 |
| NetTcpTest | 6 | ✅ |
| CmdlineStackTest | 6 | ✅ |
| SignalSafeUninstallTest | 6 | ✅ |
| HookProfileTest | 6 | ✅ |
| ReturnModProfileTest | 6 | ✅ |
| ElfResolveTest | 8 | ✅ S68 |
| HookArgLogTest | 7 | ✅ S69 |
| **HookTagTest** | **7** | **✅ S71** |
| GuestLoadTest | 1 | ✅ (canary) |
| ActivityBootProbeTest | 1 | 🔒 ignored |
| IntegrationTest | 1 | ✅ |
| Diagnostic probes | several | non-gating |

**Ignored count: 3** = `ActivityBootProbeTest` (1) + `AdbDetectTest` (2 — Settings.Global walled by §5.10).

**Test count history (registered):**
- S40: 51 · S41: 51 · S42: 57 · S43: 63 · S44: 68 · S45: 74 · S46: 80 · S47: 86 · S48: 92 · S49: 98
- S50: 104 · S51: 112 · S52: 118 · S53: 124 · S54: 130 · S55: 136 · S56: 142 · S57: 149 · S58: 155
- S59: 168 · S60: 177 · S61: 183 / 184 measured · S62: 191 · S63: 198 · S64: 205 · S65: 212
- S66: 219 · S67: 225 · S68: 233 · S69: 240 · S70: 245 · **S71 close: 252** (+7: HookTagTest)

---

## Session 72 first actions (authoritative)

Phase F is 7/11 sessions complete (S65 ✅, S66 ✅, S67 ✅, S68 ✅, S69 ✅, S70 ✅, S71 ✅). **Category 6 is now 9/9 COMPLETE.** The only remaining ⚠️ is 5.10 (Settings.Global walled) — which is a **disassembly session** (IDA/Ghidra), not an engine-primitive session.

Session 72 targets one of the three Phase-F disassembly items. **Recommendation: 5.10 Settings.Global — the highest-value target** because it's the last partially-complete feature in the inventory, and closing it (or confirming the wall) transitions 5.10 from ⚠️ → ✅ or ⚠️ → documented-wall.

1. Confirm S71 commit landed (`git log --oneline -3`).
2. **Fresh-process canary rerun** to confirm S71 no-regression. Expected `outcome=booted-viber-order`, providers 15/14, `elapsedMs` in the 1660-1705 band (S71 measured 1668 — expect similar).
3. **5.10 Settings.Global disassembly prep:**
  - Pull `framework.jar` from the device.
  - Decompile `classes3.dex` with baksmali or `jadx` to locate `Settings$NameValueCache.getStringForUser`.
  - Trace the shared-memory read path (the `sNameValueCache`'s underlying map, whether the read ultimately comes from a `MemoryFile` / `SharedMemory` / direct mmap).
  - Produce a concrete proposal for S73 to implement (either a new hook mechanism with a candidate target address, or an honest "this is architecturally walled and here's the exact reason — 5.10 becomes a documented wall" close).
  - **No code changes for 5.10 in S72** if the disassembly produces no candidate. If it does, S72 can scope it to S73.
4. **If disassembly produces a candidate:** implement the hook in S73, un-ignore the two `AdbDetectTest` assertions, and see if they go green or confirm the wall.
5. **If disassembly confirms the wall:** S72 closes with 5.10 → `⚠️` downgraded to a documented wall (comparable to §5.14/§5.15/§5.16 precedent), and S73 moves to the S51 cache-hit or S60 Debug/VMDebug disassembly instead.

**Expected scope:** Medium. Pure analysis. Zero engine code unless a candidate emerges.

---

## Rules of engagement (unchanged, still authoritative)

- **Target is the engine feature inventory, not ApplicationProxy coverage.**
- **Viber is canary only. Canary must be measured from fresh process.**
- **Every session advances at least one ⚠️/❌/🔧 → ✅.** (S71: 6.2 → ✅.)
- **Verification-first:** verify any prior-session unverified code before writing new code.
- **Rule 5 (behavior match) — partial disclosure for structural ports.**
- **Rule 12: test assertions must be able to fail.** Corollary: **a test whose premise is stale is also a Rule 12 violation.** **A build-config premise is a test premise.** ABI/ABI-level premises are stable; build-config premises are brittle — prefer the former in test design. **Delivery-dimension corollary (S71): a multi-file delivery is only as complete as its least-applied file. Measure end-to-end, not per-file.**
- **Rule 14: every test-run instruction includes `./gradlew` + `adb install -r -t` on both APKs.**
- **Rule 15: engine is native.**
- **Rule 7 (decomp-first) when wire format / symbol / field layout is uncertain.**
- **No `am instrument` output = no session close.**
- **Deferred items stay deferred:** handleJHMethod (S18), tnFIN code-36 byte rewrite (S12), clone-app features, chain/retmod tagging (deferred from §6.2).
- **Full-file replacements only for code files.** Documentation edits use targeted edits within the existing file.
- **The S59 meta-lesson:** install success ≠ interception success.
- **The S60 meta-lesson:** spec-predicted mechanism ≠ the only mechanism that achieves the goal. Verify *whether*, not *how*.
- **The S61 meta-lesson:** the functional goal may already be achieved by the process's real state.
- **The S62 meta-lesson:** a single surface can be defended by multiple independent walls.
- **The S63 meta-lesson:** the functional goal may already be achieved by OS-level MAC policy.
- **The S64 meta-lesson:** the functional goal may be achieved at multiple independent layers of the stack.
- **The S65 meta-lesson:** non-trivial native features reveal bugs one subsystem at a time.
- **The S66 meta-lesson:** choose the robust primitive over the clever one.
- **The S67 meta-lesson:** extending a feature to a second subsystem is copy-and-adapt when the first implementation was structurally correct. Also — a stale @Ignore is a lie about current behavior.
- **The S68 meta-lesson:** a build-config premise is a test premise. Pivot the test to where the premise holds naturally; don't add more build-config to force the premise true.
- **The S69 meta-lesson:** the Nth primitive in a pattern family costs what the 1st primitive's discipline cost, not what the Nth primitive's novelty cost. Two prior disciplines made S69 zero-new-asm and 7/7 first-try. But S68's 3-iteration session is the counter-example: brittle (build-config) premises cost iterations, stable (ABI) premises save them.
- **The S70 meta-lesson:** when every branch of a decision table converges on the same verdict, the measurement step is diagnostic, not gating. A non-gating diagnostic whose outcome doesn't affect the verdict doesn't need to be resolved before shipping.
- **The S71 meta-lesson:** a multi-file delivery is only as complete as its least-applied file. Five correct files + one stale file = a red build, not a five-file success. Rule 12's corollary applies in the delivery dimension: measure end-to-end, not per-file. The design itself held up — S71 shipped 7/7 first-try on the tagging semantics with zero design iteration; the failure was in delivery hygiene, not in engineering.

---

## Hard-won facts (canonical reference)

Facts from Sessions 1–40 retained verbatim. Additions from Sessions 33–71:

*(all previous facts retained)*

- Feature 7.1 — Unified ELF symbol API (S68). Key facts:
  - **`.symtab` companion string table MUST be found via `symtab_hdr->sh_link`, not by section name lookup.**
  - **AGP + NDK 30.0.16248370 strips `.symtab` from CMake-built `.so` files even in `debug` builds.** `keepDebugSymbols` doesn't work on this toolchain.
  - **APK-internal paths (`dlpi_name` = `...base.apk!/lib/...`) are not filesystem-openable.** `ElfMappedLib_Open` extended with filesystem-path mode.
  - **Pseudo-libs `[vdso]`, `[vvar]`, `[stack]` must be skipped before `open()`.**
  - **`ElfSymbolLookupLastPath()` diagnostic is a Rule 12 enabler.** Without it, `.symtab` fallback is un-falsifiable.

- Feature 8.2 — Per-hook argument logging (S69). Key facts:
  - **`chain_dispatch_naked` already saves x0–x7 AND q0–q7 to the stack frame** at `sp+0..sp+48` (GP) and `sp+176..sp+304` (FP). GP arg `x0..x7` = `saved[0..7]`. FP arg `d0..d7` = low-64-bits of `q0..q7` at `saved[22], saved[24], ..., saved[36]`. **Zero new asm needed for GP+FP capture.**
  - **`retmod_stub_*` saves only x0–x7 (80-byte frame).** No q0–q7 on the frame. Adding 128 bytes to save q0–q7 on every RetMod dispatch was rejected for an opt-in feature — GP-only capture is the honest compromise. `HookArgLog.captures_fp` field distinguishes the two subsystems at runtime.
  - **`argLogFixtureGp` must be `naked`** so its arguments stay in x0–x7 at entry.
  - **`argLogFixtureFp` must be normal C++ (noinline)** — its double args are already in d0–d3 per AAPCS64, and `chain_dispatch_naked` saves those d registers to the stack before the dispatch helper runs.
  - **`__atomic_fetch_add(&log->head, 1)` returns the PRE-increment value.** After 20 captures, `head=20`, `ring[0..3]` hold calls #17..#20, `ring[4..15]` hold calls #5..#16. **Test 4 (`argLogRingWraps`) asserts this exactly.**
  - **`mem_bytes = sizeof(HookArgLog) = 2208B`** (32-byte header + 16 × 136-byte captures). Reported separately from `HookProfile::mem_bytes`.
  - **`hookArgLogRead(target, ringIdx)` returns `LongArray[17]` = `{gp0..gp7, fp0..fp7, pc}`.** Doubles decoded via `Double.fromBits(arr[8+i])`.
  - **The two-feature independence (`HookProfile` and `HookArgLog`) means disabling one doesn't affect the other.**
  - **`HookArgLog` fields are plain POD accessed via `__atomic_*_n`** — same discipline as `HookProfile`.

- Feature 5.6 — Build property coverage (S70). Key facts:
  - **`g_fakeProps` slots are allocated one-per-distinct-spoofed-name and never released.** The array size is therefore a hard ceiling on how many spoofed names can ever be `__system_property_find`-able. At 16 slots and 15 spoofed names the old ceiling had zero headroom — a `static_assert` now binds the two sizes so any future overflow is a build failure, not a silent `Fake___system_property_find` returning `nullptr`.
  - **A property that is already honest on the device gains nothing from a spoof entry.** `ro.product.first_api_level` and `ro.build.version.security_patch` were deliberately **not** added: a fabricated value manufactures an inconsistency with `Build.VERSION.*` / `minSdk` / vendor patch level, which is a louder tell than the real value. (S69 meta-lesson applied in the negative direction.)
  - **The `BuildPropTest` assertions are the feature, not the table entries.** They fail if the property ever flips to an unlocked tell — whether that flip comes from a removed spoof entry or a ROM update. On a device whose real values are clean (this one), the entries are behavior-neutral and the assertions are regression guards; on a device whose real values are tampered, the entries are load-bearing and the assertions are the verification.
  - **Vendor libraries (`PowerHalWrapper`, MediaTek framework) read vendor properties via `__system_property_get` but those libs are not in `ShouldHookProperty`'s scope.** So vendor-side `ro.boot.warranty_bit` / `ro.warranty_bit` reads hit the real device values, not the spoof. The S70 canary move (1675 → 1728) was suspected to be a vendor-side interaction with the new tell-set entries; **S71 measured 1668ms — faster than S69 — so the S70 1728ms was jitter, not a real interaction.** The vendor-suspect hypothesis is empirically refuted.

- Feature 6.2 — Per-guest tags (S71). Key facts:
  - **`HookHandle.tag[48]` is a plain field set while the handle is `HS_INSTALLING`, inside the same `g_reg_mutex` critical section as the state flip.** A post-hoc `RegistrySetTag(target, ...)` would race with `RegistryUninstallByTag`'s `HS_INSTALLED` filter — `RegistryUninstallByTag` only ever sees `HS_INSTALLED` handles, so a handle must be fully-tagged by the time it's `HS_INSTALLED`.
  - **Thread-local pending tag (`g_pendingTag[48]`) is consumed by `RegistryAlloc` under `g_reg_mutex`.** The tag lives only for the interval between `RegistrySetPendingTag(tag)` (or `HookGotEntryExTagged`'s wrap) and the `RegistryAlloc` that follows inside `HookGotEntryEx`. `HookGotEntryExTagged` guarantees cleanup on both success and failure paths — direct `RegistrySetPendingTag` calls must replicate this dance or risk leaking the tag to the next install on the same thread.
  - **`FindHandle` is tag-blind and must stay that way.** One address holds one patch. Conflict detection operates on target address only. A tag-aware `FindHandle` would allow a second install on an already-hooked address under a different tag, silently double-patching the slot — exactly what 6.7 (inter-hook conflict detection) exists to prevent.
  - **`HookGotEntryEx`'s early `if (*gotAddress == newFunction) return HOOK_ALREADY_HOOKED;` fires before `FindHandle`.** So a same-replacement reinstall on an already-owned target returns code 8 (ALREADY_HOOKED) without ever reaching `FindHandle`. To exercise `FindHandle` directly, install a **different** replacement — `HOOK_INTERNAL_ERROR` (code 10) is the correct response when `FindHandle` finds a handle with a different replacement. This is why the S71 fixture set has both `g_tagRepls[]` and `g_tagAlts[]`.
  - **`RegistryUninstallByTag` uses `std::vector<UnhookTarget>` capture-then-unhook, never holds `g_reg_mutex` across an unhook.** Both `UnhookGotEntry` and `UninstallInlineHook` call `RegistryFree`, which re-acquires `g_reg_mutex` — a lock_guard spanning the unhook loop self-deadlocks on the first iteration.
  - **Synthetic GOT slots in `.data` of `libhookengine.so` are safe targets for `HookGotEntryEx`.** `ReadPageProt(pageStart)` succeeds (the `.data` page is always in `/proc/self/maps`), `mprotect(RW)` succeeds, the pointer write lands, and `HookHandle.target` records the slot address so uninstall restores the original value correctly. No need for real GOT slots or real symbol imports for tagging tests.
  - **`HookRegistry.h` must carry both the `HookHandle.tag[48]` field AND the `extern "C" HookResult HookGotEntryEx(...)` declaration.** Missing either causes cascading compile errors when `HookRegistry.cpp`'s `HookGotEntryExTagged` and the tag-filtering walkers are compiled — this is exactly what happened on S71's first build attempt.

- S69 companion finding — **first-try green is not the general case, it's the case when the premise is stable.** S69 shipped 7/7 first try because every premise was an ABI fact. S68 shipped on the 3rd iteration because the premise was a build-config fact. The lesson is not "prefer ABI premises" — it's "measure the premise BEFORE writing the test; a stale premise is the iteration tax." S70 (5/5 new assertions green first-try) and S71 (7/7 green first-try) both confirm: when the premise is ABI- or data-stable and the design reuses a proven pattern, iterations are zero.

---

## The blunt version

**Four categories complete:** Category 1 (Hook mechanisms, 9/9), Category 2 (Memory primitives, 7/7), Category 3 (IPC / kernel boundary, 7/7), Category 4 (Class loading, 6/6). **Category 6 (Safety / correctness) is 9/9 COMPLETE** as of S71. **Category 7 (Symbol / ELF) is 6/6 COMPLETE.** **Category 8 (Diagnostics) is 7/7 COMPLETE.** Only Category 5 remains partial: 15/16 filters, with `5.10` (ADB-detect / Settings.Global) as the last ⚠️. **Phase E CLOSED** at S64. **Phase F is 7/11 sessions complete** — 6.8 signal-safe uninstall ✅ (S65), 8.7 per-hook profiling ✅ (S66+S67), 7.1 unified ELF symbol API ✅ (S68), 8.2 per-hook argument logging ✅ (S69), 5.6 build property coverage ✅ (S70), **6.2 per-guest tags ✅ (S71)**. Sessions 41–71 shipped thirty-four verified primitives, all on-device verified with fail-able tests:

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
- 59: ADB detect blocking (⚠️ PARTIAL — Settings.Global walled)
- 60: debugger detect blocking (✅ — property-gated JDWP)
- 61: emulator detect blocking (✅ — real-device cleanliness)
- 62: test-keys detect blocking (✅ — 3 surfaces)
- 63: /proc/net/tcp blocking (✅ — OS-level SELinux)
- 64: /proc/self/cmdline + syscall/stack filter (✅ — setArgV0 + kernel + S23)
- 65: signal-safe uninstall (✅ — deferred SPSC + worker; `AllocateMemoryNear` page-scan)
- 66: per-hook profiling ChainHook (✅ — `HookProfile` + CLOCK_MONOTONIC)
- 67: per-hook profiling ReturnMod (✅ — same pattern, 6/6 first try)
- 68: unified ELF symbol API (✅ — `ElfResolver` + `ElfMappedLib`; 3 iterations)
- 69: per-hook argument logging (✅ — `HookArgLog` + 16-slot ring; 7/7 first try)
- 70: build property coverage (✅ — 5 tell-set props + pool bump + `static_assert`; 5/5 new tests first try)
- 71: per-guest tags (✅ — `HookTag` + TLS pending tag + capture-then-unhook; 7/7 first try after one-file build fix; canary 1668ms, fastest on record)

**252 registered tests, 249 executed, 3 ignored, 0 failed at S71 close.** Session 66 was the cleanest native-deliverable session — 6/6 green first build. Session 67 matched that. Session 69 matched and exceeded it — 7/7 green first build on a feature with a novel data structure. Session 70 matched that again — 5/5 green first build on the 5.6 tell-set extension. **Session 71 matched that on the assertion side (7/7 first-try) with one process hiccup: the first build attempt failed on a stale `HookRegistry.h`, costing one extra build cycle and no design work.** S68's 3 iterations were the cost of a stale build-config premise; S71's 1 iteration was the cost of a stale file at the delivery boundary. Both are Rule-12-corollary-shaped failures: measure the premise (per S68) or measure the delivery (per S71) before declaring success.

**Phase F is 7/11 sessions in.** Remaining: 5.10 (Settings.Global shared-mem disassembly — Phase F), plus the three Phase-F disassembly items (S51 cache-hit, S60 Debug/VMDebug fast-JNI, §5.10 Settings.Global). Session 75 closes.

**If a future session can't state a specific ⚠️/❌/🔧 → ✅ transition in one sentence, it's not a session.**