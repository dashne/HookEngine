package com.ims134.hookengine

import android.os.Process
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileNotFoundException

/**
 * Session 64 — Features 5.15 (/proc/self/cmdline full spoof) and
 * 5.16 (/proc/self/syscall + /proc/self/stack filter) probe.
 *
 * Diagnostic only. No assertions. Rule 12 does not apply to probes. All
 * output goes to logcat under tag "CmdlineStackProbe".
 *
 * Six questions:
 *
 *   Q1. /proc/self/cmdline content. S19's setArgV0 already overwrites
 *       argv[0] with the guest package name (com.viber.voip). But argv[1..N]
 *       may still contain the original paths. Probe both the total byte
 *       length and the null-separated argv entries.
 *
 *   Q2. /proc/self/environ content. Does it leak the engine name or
 *       engine-specific env vars? Even if the file is readable, if it
 *       contains only system env vars (PATH, ANDROID_DATA, etc.) it's
 *       not a leak.
 *
 *   Q3. /proc/self/syscall content. Format: `<nr> <arg0> <arg1> ...
 *       <sp> <pc>`. When no ptracer is attached, this reflects the
 *       current syscall being executed by the reading thread — usually
 *       harmless. When a ptracer is attached, the format includes more
 *       state. Probe to see what's exposed.
 *
 *   Q4. /proc/self/stack content. Kernel stack trace of the reading
 *       thread. Typically only kernel symbols, not user symbols —
 *       harmless unless the process is ptraced (then user symbols appear).
 *
 *   Q5. SELinux context. For each of the three files, is the read
 *       allowed or denied? If denied, 5.15 and 5.16 close the same way
 *       as 5.14 — OS kernel MAC policy.
 *
 *   Q6. Cross-check TracerPid. /proc/self/status already has TracerPid
 *       forced to 0 by S23 BuildFilteredStatus. Verify that's still live
 *       so we know the tracing state is already covered by an existing
 *       filter.
 *
 * @Before installFileHooks() — same pattern as all probes since S60.
 * Idempotent. Ensures GOT hooks are live even if a prior test polluted
 * state.
 */
@RunWith(AndroidJUnit4::class)
class CmdlineStackProbeTest {

    private val TAG = "CmdlineStackProbe"

    @Before
    fun reinstallHooks() {
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    // ─── helper: read via File API, return classified result ────────
    private fun probeReadable(path: String, maxBytes: Int = 512): String {
        return try {
            val bytes = File(path).readBytes()
            val n = minOf(bytes.size, maxBytes)
            val preview = bytes.copyOfRange(0, n).joinToString("") { b ->
                val c = b.toInt() and 0xFF
                if (c in 0x20..0x7E || c == 0x0A) c.toChar().toString()
                else "\\x%02x".format(c)
            }
            "READABLE (${bytes.size} bytes) preview='$preview'"
        } catch (e: FileNotFoundException) {
            "DENIED_FILE_NOT_FOUND (${e.message})"
        } catch (t: Throwable) {
            "UNEXPECTED_${t.javaClass.simpleName} (${t.message})"
        }
    }

    private fun probeOsOpen(path: String): String {
        return try {
            val fd = Os.open(path, OsConstants.O_RDONLY, 0)
            try { Os.close(fd) } catch (_: Throwable) {}
            "READABLE (fd open succeeded)"
        } catch (e: ErrnoException) {
            val name = OsConstants.errnoName(e.errno) ?: "unknown(${e.errno})"
            "DENIED_ERRNO_${name} (${e.message})"
        } catch (t: Throwable) {
            "UNEXPECTED_${t.javaClass.simpleName} (${t.message})"
        }
    }

    // ─── Q1: /proc/self/cmdline ─────────────────────────────────────
    private fun probeCmdline() {
        Log.d(TAG, "=== Q1: /proc/self/cmdline ===")
        val path = "/proc/self/cmdline"
        try {
            val bytes = File(path).readBytes()
            Log.d(TAG, "  File.readBytes(): ${bytes.size} bytes")
            // Split on NUL, log each argv entry
            val argv = mutableListOf<String>()
            val sb = StringBuilder()
            for (b in bytes) {
                val c = b.toInt() and 0xFF
                if (c == 0) {
                    if (sb.isNotEmpty()) {
                        argv.add(sb.toString())
                        sb.setLength(0)
                    }
                } else if (c in 0x20..0x7E) {
                    sb.append(c.toChar())
                } else {
                    sb.append("\\x%02x".format(c))
                }
            }
            if (sb.isNotEmpty()) argv.add(sb.toString())
            Log.d(TAG, "  argv count = ${argv.size}")
            for ((i, a) in argv.withIndex()) {
                Log.d(TAG, "    argv[$i] = '$a'")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  File.readBytes() threw: ${t.javaClass.simpleName}: ${t.message}")
        }
        Log.d(TAG, "  Os.open(): ${probeOsOpen(path)}")
    }

    // ─── Q2: /proc/self/environ ─────────────────────────────────────
    private fun probeEnviron() {
        Log.d(TAG, "=== Q2: /proc/self/environ ===")
        val path = "/proc/self/environ"
        try {
            val bytes = File(path).readBytes()
            Log.d(TAG, "  File.readBytes(): ${bytes.size} bytes")
            val envs = mutableListOf<String>()
            val sb = StringBuilder()
            for (b in bytes) {
                val c = b.toInt() and 0xFF
                if (c == 0) {
                    if (sb.isNotEmpty()) {
                        envs.add(sb.toString())
                        sb.setLength(0)
                    }
                } else if (c in 0x20..0x7E || c == '='.code) {
                    sb.append(c.toChar())
                } else {
                    sb.append("\\x%02x".format(c))
                }
            }
            if (sb.isNotEmpty()) envs.add(sb.toString())
            Log.d(TAG, "  env count = ${envs.size}")
            // Log the keys only (values could be long)
            for ((i, e) in envs.withIndex()) {
                val eq = e.indexOf('=')
                val key = if (eq > 0) e.substring(0, eq) else e
                val valPreview = if (eq > 0) e.substring(eq + 1).take(80) else ""
                Log.d(TAG, "    env[$i] key='$key' value='${valPreview}'")
            }
            // Check for engine-specific leakage
            val leakCandidates = listOf("ims134", "hookengine", "viber", "guest")
            for (candidate in leakCandidates) {
                val matched = envs.any { it.contains(candidate, ignoreCase = true) }
                Log.d(TAG, "  environ contains '$candidate': $matched")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  File.readBytes() threw: ${t.javaClass.simpleName}: ${t.message}")
        }
        Log.d(TAG, "  Os.open(): ${probeOsOpen(path)}")
    }

    // ─── Q3: /proc/self/syscall ─────────────────────────────────────
    private fun probeSyscall() {
        Log.d(TAG, "=== Q3: /proc/self/syscall ===")
        val path = "/proc/self/syscall"
        Log.d(TAG, "  File.readText: ${probeReadable(path)}")
        Log.d(TAG, "  Os.open(): ${probeOsOpen(path)}")
    }

    // ─── Q4: /proc/self/stack ───────────────────────────────────────
    private fun probeStack() {
        Log.d(TAG, "=== Q4: /proc/self/stack ===")
        val path = "/proc/self/stack"
        Log.d(TAG, "  File.readText: ${probeReadable(path, 1024)}")
        Log.d(TAG, "  Os.open(): ${probeOsOpen(path)}")
    }

    // ─── Q5: SELinux context for all four files ─────────────────────
    private fun probeSelinuxContexts() {
        Log.d(TAG, "=== Q5: SELinux denial pattern across target files ===")
        // Just re-attempt reads on each — the AVC log lines are the
        // ground truth. This method's job is to produce a clean summary
        // so the full probe output has one consolidated view.
        val paths = listOf(
            "/proc/self/cmdline",
            "/proc/self/environ",
            "/proc/self/syscall",
            "/proc/self/stack"
        )
        for (p in paths) {
            Log.d(TAG, "  $p: File=${probeReadable(p, 0)} Os=${probeOsOpen(p)}")
        }
    }

    // ─── Q6: verify TracerPid still forced to 0 ─────────────────────
    private fun probeTracerPid() {
        Log.d(TAG, "=== Q6: /proc/self/status TracerPid check ===")
        try {
            val lines = File("/proc/self/status").readLines()
            for (line in lines) {
                if (line.startsWith("TracerPid:")) {
                    Log.d(TAG, "  status line: '$line'")
                    val value = line.substringAfter(':').trim()
                    Log.d(TAG, "  TracerPid = '$value' (expect '0' from S23 BuildFilteredStatus)")
                    break
                }
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  /proc/self/status read threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    // ─── Q7: path aliasing (bonus) ──────────────────────────────────
    private fun probePathAliasing() {
        Log.d(TAG, "=== Q7: path aliasing ===")
        val pid = Process.myPid()
        for (base in listOf("cmdline", "syscall", "stack")) {
            val selfResult = probeReadable("/proc/self/$base", 0)
            val pidResult = probeReadable("/proc/$pid/$base", 0)
            Log.d(TAG, "  $base: self=$selfResult pid=$pidResult")
        }
    }

    @Test
    fun probeCmdlineStackSurface() {
        Log.d(TAG, "########## S64 CmdlineStackProbeTest START ##########")
        probeCmdline()
        probeEnviron()
        probeSyscall()
        probeStack()
        probeSelinuxContexts()
        probeTracerPid()
        probePathAliasing()
        Log.d(TAG, "########## S64 CmdlineStackProbeTest END ##########")
    }
}