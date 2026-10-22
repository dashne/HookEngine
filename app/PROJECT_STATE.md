# DualHost Native Engine — FINAL STATE (v27)

## All items complete
Item 1 — HiddenAPI not needed; greylist @hide works via reflection
Item 2 — statx, stat64, lstat64, opendir, readdir, readdir64, getdents64 all hooked
Item 3 — Sig spoof verified: both instance.mPM and static.sPackageManager patched
Item 4 — Inline hooker working
Item 5 — Root detection working

## Critical bug fixed
HookJavaMethod was restoring pages to PROT_READ only, dropping WRITE from RW pages.
On ART, ArtMethod structs live in writable pages. Removing write caused SEGV_ACCERR
at a fixed address inside that page on the next ART write (JIT compile, deopt,
stack trace). Fix: read original protection from /proc/self/maps, restore exactly
that. Reads and writes preserved.

## Correct answer for Binder.getCallingUid
Keep the ArtMethod hook BUT:
- Read orig prot from /proc/self/maps (RW → restore RW)
- Save entry point for restore
- restoreBinderMethod() unhooks cleanly
- Same scope for callers: real UID when tls_inSystemCall or thread=  Binder:*,
  fake UID otherwise
- Never call _OriginalGetCallingUid from quick-invoke (invalid env)

## Correct answer for sig spoof
Patch BOTH:
- ApplicationPackageManager.mPM (instance field)
- ActivityThread.sPackageManager (static field)
  The proxy handler must match by result type, not method name.
  On API 37 the method is getPackageInfoAsUser, not getPackageInfo.
  Null-out paths: PackageInfo.signatures AND PackageInfo.signingInfo.mSigningDetails.signatures.

## Correct answer for dlopen
Do NOT hook dlopen on system libs — SELinux execmod denies mprotect RX on those
pages, and it triggers JIT crashes. Only hook libart/nativeloader/nativebridge,
and skip /data/data/.../startup_agents/ (JVMTI agent).

## What we proved about MIUI
STEP0a/b/c show the ROM handles every PackageManager call we need. No MIUI
corruption, no AppCategoryPolicy gate, no getPackageInfoAsUser failure that
matters. All 20 rounds of "MIUI is broken" claims were wrong; the crash was always
our page protections.