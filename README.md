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
| Build | v29 |

Targets API 37 / ART on ARM64. Earlier Android versions are untested and will need
adjustments to the ArtMethod layout and GOT hook target list.

---

## Architecture

```

    app/src/main/cpp/
      native-lib.cpp        JNI surface, Binder ArtMethod hook, test harness exports
      Hooks.cpp             GOT patches for file/property/UID/dlopen
      InlineHooker.cpp      ARM64 relay + trampoline inline hooker
      arm64_reloc.cpp       Instruction relocator (B, BL, ADRP, ADR)
      HookRegistry.cpp      512-slot hook registry
      elf_hooker.cpp        GOT lookup via dl_iterate_phdr
      RedirectTable.cpp     Path prefix rewrite table
      HiddenApi.cpp         libart.so symbol probe and hiddenapi bypass
      TestReport.h          PASS/FAIL sink shared by all test files
      test_item1..5.cpp     Behaviour tests (23 cases)
      test_regression.cpp   Bug 1-5 regression tests
      test_suite.cpp        Harness dispatch

    app/src/androidTest/java/com/ims134/hookengine/
      NativeTestBridge.kt   JNI bridge object
      NativeItemTest.kt     5 parameterized item tests
      RegressionTest.kt     5 regression tests (bugs 1-5)
      IntegrationTest.kt    1 selftest.txt verification

```

---

## Testing

### CI

The GitHub Actions workflow (`.github/workflows/test.yml`) is **build-only**:

```
./gradlew :app:assembleDebug :app:assembleDebugAndroidTest
```

This compiles the harness and the test APK. It does **not** run the hook engine, does not run any of the 33 tests, and does not produce device signal. Do not describe it as test coverage.

### Device tests are manual

The target device is a single API 37 arm64 physical device (Xiaomi 17T / HyperOS). Two automated paths exist and **both are broken on this device**:

1. **`:app:connectedDebugAndroidTest`** — fails to install the test APK with `SecurityException: INSTALL_GRANT_RUNTIME_PERMISSIONS` (UTP-injects a signature-permission flag MIUI refuses), logs `AndroidTestRunner failed`, and then reports `BUILD SUCCESSFUL`. Zero tests run; the task returns green. Do not rely on it.

2. **`am instrument -w` exit code** — returns `INSTRUMENTATION_CODE: -1` (RESULT_OK) on both success and failure. Exit code is meaningless.

### Verified device workflow

```
adb install -r -t app/build/outputs/apk/debug/app-debug.apk
adb install -r -t app/build/outputs/apk/androidTest/debug/app-debug-androidTest.apk
adb logcat -c
adb shell am instrument -w -r com.ims134.hookengine.test/androidx.test.runner.AndroidJUnitRunner
```

Pass criterion: the string `OK (N tests)` appears in the output, **and** no line contains `INSTRUMENTATION_STATUS_CODE: -2`. Both conditions must be checked; neither is implied by the exit code.

### What the device suite covers

| Class | Tests | What it verifies |
|---|---|---|
| `NativeItemTest` | 5 | 23 underlying cases across items 1–5 (inline hooking, rollback, concurrency, ELF, gate) |
| `RegressionTest` | 5 | Bugs 1–5 (page prot restore, dlopen invariant, PM field patch, relay leak, deferred free) |
| `IntegrationTest` | 1 | `selftest.txt` file channel through `RemoteService` — the only test that verifies the app's actual `onStartCommand` path |

Total: **11 JUnit tests**, exercising **33 underlying assertions**.

### Limitations

- Only verified on API 37 / arm64 / HyperOS. Other API levels are refused by `CheckHookCapability` — see Item 5.
- `testBuildType` is set to `debug`; release builds do not include the instrumentation suite.
- `abiFilters` is locked to `arm64-v8a` — test files contain ARM64 inline assembly and do not compile for x86, x86_64, or armv7.
- `connectedDebugAndroidTest` is not part of any workflow in this project.