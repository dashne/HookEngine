PROJECT STATE: DualHostCore Native Virtualization Engine

1. CONFIRMED ARCHITECTURAL DECISIONS
   Environment: Android Studio, NDK 30.0.16248370, Physical ARM64 Device (Android 17 / API 37).
   Naming Convention: C# style (PascalCase methods, _camelCase variables).
   Page Size: Always query sysconf(_SC_PAGESIZE). Never hardcode.
   Memory Protection: RWX mprotect + MAP_FIXED_NOREPLACE for relay pages.
   Process Isolation: Hooks only in guest processes, never main UI.
   Global PLT Hooking: dl_iterate_phdr with whitelist (libjavacore/libopenjdk/data/app/data/user/data/data).
   Property API: hook _get, _find, _read_callback, _read — all four.
   ArtMethod hook: offset 24 on API 37 ARM64. Signature (JNIEnv*, jclass) mandatory.

2. IMPLEMENTATION STATUS (PROVEN)
   ☑ ELF Parser & PLT/GOT — idempotent HookGotEntry, multi-library patching
   ☑ File redirection — open/openat/stat/access/readlink/unlink/rename/mkdir/rmdir/fopen (+ variants)
   ☑ /proc/self/maps filter — memfd-backed, cached 2s, hides dualhost/hookengine/libdualcore lines
   ☑ dlopen / android_dlopen_ext interposer — rescan on late load
   ☑ __system_property_get — 9 props spoofed (ro.debuggable/secure/build.tags/build.type/qemu/adbd/adb.root/oem_unlock)
   ☑ __system_property_find / read_callback / read — fake prop_info table, all 4 API paths
   ☑ Native getuid/geteuid/getgid/getegid — GOT-hooked in /data/* and libjavacore
   ☑ Binder.getCallingUid — ArtMethod patched, dynamic UID via SetVirtualUid()
   ☑ ARM64 inline hooker — MAP_FIXED_NOREPLACE ±120MB, PC-relative refusal, patch verify

3. HARDENING BACKLOG — 5 ITEMS, IN EXECUTION ORDER

   Item 4 (PREREQUISITE): Inline hooker wiring + test
   - Expose inline hook via JNI, prove it fires on libc symbols not covered by GOT.
   - BLOCKS Item 1. Must land first.

   Item 1 (NEXT): HiddenAPI native bypass
   - Hook art::HiddenApi::ShouldDenyAccessToMember / GetMemberAction / GetDeclaredMethodInternal
     inside libart.so via the inline hooker.
   - Sym: version-dependent (API 30–33 vs 34+).
   - Depends on Item 4.

   Item 2: getdents64 + statx + fstatfs
   - opendir/readdir path — required for File.listFiles() correctness.
   - statx — required for File.length() / exists() on 30+ paths.

   Item 5: Root-detection path filter
   - /system/bin/su, /system/xbin/su, /sbin/su, /system/app/Superuser.apk,
     /data/adb/magisk*, Magisk paths, /system/xbin/daemonsu.

   Item 3: Signature spoof
   - Hook ArtMethod on PackageManager.getPackageInfo → replace PackageInfo.signatures with fake.

   Item 5 (final): Full integration test — ALL five features + existing engine
   - "Ultimate test" described in §6.

4. KNOWN LIMITATIONS (not in scope)
   - Play Integrity / SafetyNet — impossible without device attestation.
   - Certificate pinning — separate project (OkHttp/Conscrypt hooks).
   - Direct syscall(__NR_openat, ...) — bypasses GOT, needs inline hook of libc syscall() (Item 4 enables).
   - Frida detection strings — trivial, add to Item 5 if needed.

5. VALIDATION CRITERIA (existing)
   ☑ Stress test 8 threads / 10s (filesystem)
   ☑ JNI ArtMethod interception (Binder.getCallingUid)
   ☑ Multi-library GOT patching (400 libs tested, reduced to 3 whitelisted)
   ☑ Late-load rescan (dlopen interposer verified)
   ☑ Property spoof via Java API + native getuid

6. ULTIMATE TEST — final acceptance
   Single Kotlin test block that asserts ALL of:
   - Binder.getCallingUid() == virtualUid
   - Process.myUid() == virtualUid
   - nativeGetpid() == 12345 (inline hook proof)
   - SystemProperties.get("ro.debuggable") == "0"
   - reflection: Class.getDeclaredMethod("completelyHiddenSecret") succeeds (HiddenAPI)
   - File.listFiles() returns virtual entries not host entries (getdents64)
   - File.length() on virtual file matches (statx)
   - File("/system/bin/su").exists() == false (root detection)
   - getPackageInfo(own).signatures[0] != real signature (sig spoof)
     PASS = all 9 green in log. Any red = that item regressed.