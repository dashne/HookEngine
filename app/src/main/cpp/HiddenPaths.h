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

    // ─── Shell-command denial ──────────────────────────────────────────────
// Tokenizes a shell command string on shell metacharacters and checks
// each token against IsBlockedExecPath. Handles:
//   "su"                    → blocked (bare)
//   "su -c id"              → blocked (su token)
//   "echo hi; su"           → blocked (su token after ;)
//   "echo hi && su"         → blocked (su token after &&)
//   "foo | su"              → blocked (su token after |)
//   "/system/bin/su -c id"  → blocked (path token)
//   "$(su)" / "`su`"        → blocked (su token, ( ) ` are boundaries)
//   "issue --help"          → allowed (issue is not su, not a blocked path)
//   "sunshine"              → allowed (sunshine is not su)
//   "subin"                 → allowed
//   "sudo apt install"      → allowed (sudo is a distinct binary)
//
// Does NOT handle quoted tokens: `sh -c '"su"'` still bypasses this check.
// That's acceptable — the shell eventually execve()s the real binary and
// the execve hook (IsSuPath) catches it at the last mile. system() is a
// first-line defense; execve is the actual gate.
    inline bool IsBlockedShellCommand(const char* cmd) {
        if (!cmd || !cmd[0]) return false;

        auto isBoundary = [](char c) {
            return c == '\0' || c == ' ' || c == '\t' || c == '\n' ||
                   c == '\r' || c == ';' || c == '|' || c == '&' ||
                   c == '`' || c == '(' || c == ')' || c == '{' ||
                   c == '}' || c == '<' || c == '>';
        };

        const char* tokStart = nullptr;
        for (const char* p = cmd; ; p++) {
            bool boundary = isBoundary(*p);
            if (tokStart == nullptr && !boundary) {
                tokStart = p;
            }
            if (tokStart != nullptr && boundary) {
                size_t len = (size_t)(p - tokStart);
                if (len == 2 && tokStart[0] == 's' && tokStart[1] == 'u') {
                    return true;   // bare "su" token
                }
                if (len > 0 && len < 256) {
                    char buf[256];
                    memcpy(buf, tokStart, len);
                    buf[len] = '\0';
                    if (IsBlockedExecPath(buf)) return true;
                }
                tokStart = nullptr;
            }
            if (*p == '\0') break;
        }
        return false;
    }

} // namespace HiddenPaths