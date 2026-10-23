#pragma once
#include <cstring>
#include <cstddef>

namespace HiddenPaths {

// Boundary-checked prefix match. True if:
//   - p == prefix (exact)
//   - prefix ends with '/' AND p starts with prefix (already a boundary)
//   - p starts with prefix AND next char is '/' or '\0'
    inline bool MatchPrefix(const char* p, const char* prefix) {
        if (!p || !prefix) return false;
        size_t plen = strlen(prefix);
        if (plen == 0) return false;
        if (strncmp(p, prefix, plen) != 0) return false;
        if (prefix[plen - 1] == '/') return true;
        return p[plen] == '\0' || p[plen] == '/';
    }

    inline bool MatchExact(const char* p, const char* exact) {
        return p && exact && strcmp(p, exact) == 0;
    }

// ─── Exec-ve denial table ──────────────────────────────────────────────
// Paths that must never be exec'd. Matched with boundary check.
    inline bool IsBlockedExecPath(const char* p) {
        if (!p || !p[0]) return false;

        // Bare "su" — resolved via PATH by the shell
        if (MatchExact(p, "su")) return true;

        static const char* kExact[] = {
                // Su binaries
                "/system/bin/su", "/system/xbin/su", "/sbin/su",
                "/system/sd/xbin/su", "/system/bin/failsafe/su",
                "/system/xbin/daemonsu", "/system/bin/.ext/.su",
                "/system/usr/we-need-root/su-backup", "/su/bin/su",
                // Magisk binaries
                "/system/bin/magisk", "/system/xbin/magisk", "/sbin/magisk",
                nullptr
        };
        for (int i = 0; kExact[i]; i++) {
            if (MatchExact(p, kExact[i])) return true;
        }

        static const char* kPrefix[] = {
                "/data/adb/", "/sbin/.magisk", "/debug_ramdisk", "/cache/magisk",
                nullptr
        };
        for (int i = 0; kPrefix[i]; i++) {
            if (MatchPrefix(p, kPrefix[i])) return true;
        }
        return false;
    }

// ─── File-hide table ───────────────────────────────────────────────────
// Paths that should return ENOENT from stat/open/etc.
    inline bool IsHiddenFilePath(const char* p) {
        if (!p || !p[0]) return false;

        // Test-only marker — used by AntiDetectionE2ETest to verify the GOT
        // hook fires. Production paths never contain this substring.
        if (strstr(p, ".hookengine-test-hidden")) return true;

        static const char* kPrefix[] = {
                "/data/adb/", "/sbin/.magisk", "/debug_ramdisk", "/cache/magisk",
                nullptr
        };
        for (int i = 0; kPrefix[i]; i++) {
            if (MatchPrefix(p, kPrefix[i])) return true;
        }

        static const char* kExact[] = {
                // Su binaries
                "/system/bin/su", "/system/xbin/su", "/sbin/su",
                "/system/sd/xbin/su", "/system/bin/failsafe/su",
                "/system/xbin/daemonsu", "/system/bin/.ext/.su",
                "/system/usr/we-need-root/su-backup", "/su/bin/su",
                // Superuser apps
                "/system/app/Superuser.apk", "/system/app/SuperSU/SuperSU.apk",
                "/system/etc/.has_su_daemon",
                // Magisk binaries
                "/system/bin/magisk", "/system/xbin/magisk", "/sbin/magisk",
                nullptr
        };
        for (int i = 0; kExact[i]; i++) {
            if (MatchExact(p, kExact[i])) return true;
        }
        return false;
    }

} // namespace HiddenPaths