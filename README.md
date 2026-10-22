# HookEngine

A userspace hooking engine for Android — proof of concept for the **DualHost** native
virtualization engine. Demonstrates that app-level containerization primitives (Binder
UID spoofing, file redirection, property spoofing, signature spoofing, root hiding) can
be implemented entirely with **userspace hooks** — no kernel module, no custom ROM, no root.

All capabilities below were verified on a physical device in a single session.

---

## Features

| Capability | Status | Mechanism |
|---|---|---|
| `Binder.getCallingUid` context-aware hook | ✅ | ArtMethod entry-point patch + JNI replacement |
| Binder hook cleanly restorable | ✅ | Original entry point saved and restored |
| File redirect (`open`, `stat`, `readdir`, `getdents`) | ✅ | GOT/PLT hooks in `libjavacore` / `libopenjdk` / `libopenjdkjvm` |
| Property spoof (`__system_property_*` family) | ✅ | GOT hook on libc property functions |
| Native UID hook (`getuid`, `geteuid`, `getgid`, `getegid`) | ✅ | GOT hook, respects system-call depth |
| ARM64 inline hooker (relay + trampoline) | ✅ | `MAP_FIXED_NOREPLACE` near allocation, RX restore |
| Item 1 — greylist `@hide` access | ✅ | Plain reflection (no bypass needed on API 37) |
| Item 2 — `statx` + `readdir` filtering | ✅ | GOT hooks with path/dirent allow-lists |
| Item 3 — package signature spoof | ✅ | `mPM` + `ActivityThread.sPackageManager` proxy |
| Item 5 — root detection bypass | ✅ | Path hiding for `su`, `magisk`, etc. |

### What each item means

- **Item 1** — Hidden `@hide` greylist APIs (e.g. `SystemProperties.native_get`) are
  reachable via ordinary reflection on API 37. No `setHiddenApiExemptions` needed.
- **Item 2** — `statx` and `readdir64` are filtered so guest code sees a virtualized
  filesystem view.
- **Item 3** — `PackageManager.getPackageInfo` results are intercepted and their
  signatures replaced. Requires patching **both** the instance `mPM` field and the
  static `ActivityThread.sPackageManager` (HyperOS routes through the static one).
- **Item 5** — Root indicator paths (`/system/bin/su`, `magisk`, etc.) are hidden from
  `access`/`stat`/`readdir` while legitimate binaries (`sh`) remain visible.

---

## Tested environment

| | |
|---|---|
| Device | Xiaomi 17T (physical) |
| ROM | HyperOS |
| Android | 17 (API 37) |
| ABI | ARM64 |
| Build | v27 |

Targets API 37 / ART on ARM64. Earlier Android versions are untested and will need
adjustments to the ArtMethod layout and GOT hook target list.

---

## Architecture
