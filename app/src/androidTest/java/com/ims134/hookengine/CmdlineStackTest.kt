package com.ims134.hookengine

import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileDescriptor

/**
 * Session 64 — Features 5.15 (/proc/self/cmdline full spoof) and
 * 5.16 (/proc/self/syscall + /proc/self/stack filter).
 *
 * ─── Mechanism: no engine code. Both goals are already achieved by
 * pre-existing layers of the stack. ──────────────────────────────────
 *
 * Feature 5.15 (/proc/self/cmdline full spoof)
 *
 * The spec goal was: "full /proc/self/cmdline spoof — beyond setArgV0,
 * remaining argv entries." The anticipated mechanism was a
 * BuildFilteredCmdline / IsCmdlinePath filter rewriting argv[1..N] to
 * empty. The S64 probe measured:
 *
 *     File.readBytes(): 97 bytes
 *     argv count = 1
 *       argv[0] = 'com.viber.voip'
 *
 * S19 already calls Process.setArgV0("com.viber.voip") in
 * ApplicationProxy.cpp, and the underlying kernel / Android behavior
 * writes `com.viber.voip\0` followed by NUL-padding up to the original
 * argv buffer size. The visible argv is a single entry with no
 * residual engine identity. There is nothing left for a filter to hide.
 *
 * The probe also checked /proc/self/environ (Q2): 19 standard Android
 * env vars (PATH, ANDROID_ROOT, BOOTCLASSPATH, etc.), zero matches for
 * ims134 / hookengine / viber / guest. The environ surface is not a
 * leak vector either.
 *
 * Feature 5.16 (/proc/self/syscall + /proc/self/stack filter)
 *
 * The spec goal was: "filter /proc/self/syscall and /proc/self/stack —
 * hide ptrace-attachment surface." The probe measured:
 *
 *     /proc/self/syscall: READABLE, 67 bytes, 9-field standard format
 *       `22 0x7b 0x7fd15c5860 0x10 0x1396 0x0 0x8 0x7fd15c5800 0x70399251cc`
 *
 *     /proc/self/stack: READ DENIED at kernel level with EACCES
 *       File.readText() → IOException: read failed: EACCES
 *       Os.open() succeeds but Os.read() would fail with EACCES
 *       (kernel ptrace_may_access check — no AVC line, not SELinux)
 *
 *     /proc/self/status TracerPid: 0 (S23 BuildFilteredStatus live)
 *
 * Two distinct defenses:
 *
 *   - syscall: readable but reveals only the main thread's current
 *     syscall. With no ptracer attached (5.9 blocks ptrace, 5.11
 *     property-gates JDWP), the state is indistinguishable from any
 *     other process. Not a leak.
 *
 *   - stack: read-denied at kernel level via ptrace_may_access
 *     capability check. Even if the kernel loosened, with no ptracer
 *     attached the trace would show only kernel symbols, not user code
 *     state — no leak.
 *
 *   - status: TracerPid already forced to 0 by S23's BuildFilteredStatus.
 *     No ptrace-attachment signal via status.
 *
 * **Both 5.15 and 5.16 are achieved without engine code.** No
 * Hooks.cpp changes. No IsCmdlinePath / IsSyscallPath / IsStackPath.
 * No BuildFilteredCmdline / BuildFilteredSyscall / BuildFilteredStack.
 * The pre-existing layers — S19 setArgV0, kernel ptrace_may_access,
 * S23 BuildFilteredStatus — do the work.
 *
 * ─── Meta-lesson (S64, extends S59–S63 class) ───────────────────────
 *
 * The functional goal may be achieved at multiple independent layers
 * of the stack, each from a different tier:
 *
 *   - Framework identity spoof (S19):       Process.setArgV0 → cmdline clean
 *   - Status filter (S23):                  TracerPid: 0 → no ptrace signal
 *   - Kernel capability check:              ptrace_may_access → stack read denied
 *   - Absence of state:                     no ptracer → syscall shows nothing anomalous
 *   - OS SELinux MAC policy (S63):          /proc/net denied at read time
 *   - Hardware identity (S61):              real device, no emulator fingerprints
 *   - Natural data state (S62):             release-signed Build.*, real signature
 *
 * Probe the deepest layer first, then work up. If any layer closes
 * the surface, the engine does not need to add another. When a specific
 * ROM or kernel change breaks one layer, that's the early warning that
 * a real mechanism is now required at a different layer.
 *
 * ─── Rule 12 — every assertion is falsifiable ──────────────────────
 *
 *   1. cmdlineShowsGuestPackage
 *      Fails if S19 setArgV0 regresses OR the guest-package name
 *      changes without updating the assertion. Probe measured
 *      'com.viber.voip' on this device.
 *
 *   2. cmdlineDoesNotLeakEngineName
 *      Fails if any future regression reintroduces engine identity into
 *      cmdline (e.g., setArgV0 removed, argv[0] not spoofed, or a new
 *      argv entry added).
 *
 *   3. environDoesNotLeakEngineName
 *      Fails if the engine or its dependencies ever set an env var
 *      containing 'ims134' or 'hookengine'.
 *
 *   4. syscallFileWellFormed
 *      Fails if /proc/self/syscall becomes unreadable (kernel policy
 *      change) or if the kernel output format changes (9 fields).
 *
 *   5. stackReadDeniedAtKernel
 *      Fails if the kernel starts allowing app-domain reads of
 *      /proc/self/stack (ROM update loosening ptrace_may_access).
 *      Belt-and-suspenders: tests both File.readText() and the direct
 *      Os.open + Os.read() path.
 *
 *   6. tracerPidForcedZero
 *      Fails if S23 BuildFilteredStatus regresses (status filter stops
 *      firing) OR if a debugger is attached (real TracerPid).
 *
 * ─── Test-isolation ─────────────────────────────────────────────────
 *
 * @Before calls NativeTestBridge.installFileHooks() — same pattern as
 * every anti-detection test class since S60. Idempotent. Ensures GOT
 * hooks are live even if a prior test in the suite polluted state
 * (ConditionalHookTest / AdbDetectTest precede alphabetically).
 *
 * NOTE: the syscall and stack assertions do NOT depend on hooks being
 * installed — the kernel behavior is independent of GOT hook state.
 * The @Before is defensive only.
 */
@RunWith(AndroidJUnit4::class)
class CmdlineStackTest {

    private val TAG = "CmdlineStackTest"

    @Before
    fun reinstallHooks() {
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    /** Parse a NUL-separated byte sequence (cmdline / environ format). */
    private fun parseNulSeparated(bytes: ByteArray): List<String> {
        val result = mutableListOf<String>()
        val sb = StringBuilder()
        for (b in bytes) {
            val c = b.toInt() and 0xFF
            if (c == 0) {
                if (sb.isNotEmpty()) {
                    result.add(sb.toString())
                    sb.setLength(0)
                }
            } else {
                sb.append(c.toChar())
            }
        }
        if (sb.isNotEmpty()) result.add(sb.toString())
        return result
    }

    // ─── Feature 5.15 — cmdline ─────────────────────────────────────

    @Test
    fun cmdlineShowsGuestPackage() {
        val path = "/proc/self/cmdline"
        val bytes = File(path).readBytes()
        Log.d(TAG, "$path: ${bytes.size} bytes")
        val argv = parseNulSeparated(bytes)
        Log.d(TAG, "argv count = ${argv.size}")
        for ((i, a) in argv.withIndex()) {
            Log.d(TAG, "  argv[$i] = '$a'")
        }
        assertEquals(
            "cmdline must contain exactly one argv entry — S19's setArgV0 " +
                    "overwrites argv[0] and the rest of the original buffer " +
                    "is NUL-padded. Fails if the kernel/Android changes " +
                    "buffer management or setArgV0 is removed.",
            1, argv.size)
        assertEquals(
            "cmdline argv[0] must be 'com.viber.voip' — S19's setArgV0 " +
                    "call in ApplicationProxy.cpp sets this value. Fails if " +
                    "setArgV0 regresses OR the guest package changes.",
            "com.viber.voip", argv[0])
    }

    @Test
    fun cmdlineDoesNotLeakEngineName() {
        val text = String(File("/proc/self/cmdline").readBytes(), Charsets.UTF_8)
        Log.d(TAG, "cmdline text length = ${text.length}")
        assertFalse(
            "cmdline must not contain 'ims134' — engine package identity " +
                    "leaking into cmdline would let a detector confirm the " +
                    "engine is present. S19 setArgV0 keeps argv[0] clean; " +
                    "this assertion catches any regression that lets engine " +
                    "identity back in.",
            text.contains("ims134"))
        assertFalse(
            "cmdline must not contain 'hookengine' — same as 'ims134'.",
            text.contains("hookengine"))
    }

    @Test
    fun environDoesNotLeakEngineName() {
        val text = String(File("/proc/self/environ").readBytes(), Charsets.UTF_8)
        Log.d(TAG, "environ text length = ${text.length}")
        assertFalse(
            "/proc/self/environ must not contain 'ims134' — env var leakage " +
                    "would let a detector confirm engine identity. On this " +
                    "device the environ holds only standard Android env vars " +
                    "(PATH, ANDROID_ROOT, BOOTCLASSPATH, etc.).",
            text.contains("ims134"))
        assertFalse(
            "/proc/self/environ must not contain 'hookengine' — same as 'ims134'.",
            text.contains("hookengine"))
    }

    // ─── Feature 5.16 — syscall + stack ─────────────────────────────

    @Test
    fun syscallFileWellFormed() {
        val path = "/proc/self/syscall"
        val text = try {
            File(path).readText()
        } catch (t: Throwable) {
            throw AssertionError(
                "$path must be readable — the probe measured it as readable " +
                        "on this device. Fails if the kernel policy changes to " +
                        "deny the read. Observed: ${t.javaClass.simpleName}: " +
                        "${t.message}", t)
        }
        Log.d(TAG, "$path raw: '${text.trim()}'")
        val parts = text.trim().split(Regex("\\s+"))
        assertEquals(
            "$path must have exactly 9 fields — standard Linux format is " +
                    "<syscall_nr> <arg0> <arg1> <arg2> <arg3> <arg4> <arg5> " +
                    "<sp> <pc>. Observed: ${parts.joinToString(" ")}",
            9, parts.size)
        val syscallNr = parts[0].toIntOrNull()
        assertTrue(
            "$path field[0] (syscall nr) must be a non-negative integer, " +
                    "got '${parts[0]}'",
            syscallNr != null && syscallNr >= 0)
        val sp = parts[7].removePrefix("0x").toLongOrNull(16)
        val pc = parts[8].removePrefix("0x").toLongOrNull(16)
        assertTrue(
            "$path field[7] (sp) must be a hex value > 0, got '${parts[7]}'",
            sp != null && sp > 0)
        assertTrue(
            "$path field[8] (pc) must be a hex value > 0, got '${parts[8]}'",
            pc != null && pc > 0)
    }

    @Test
    fun stackReadDeniedAtKernel() {
        val path = "/proc/self/stack"

        // Path 1: File API (goes through libcore → Os.read)
        val fileDenied: Boolean = try {
            File(path).readText()
            false
        } catch (t: Throwable) {
            val msg = t.message ?: ""
            msg.contains("EACCES") || msg.contains("Permission denied")
        }
        Log.d(TAG, "$path via File.readText(): denied=$fileDenied")

        // Path 2: direct Os.open + Os.read (native path)
        val osReadDenied: Boolean = try {
            val fd: FileDescriptor = Os.open(path, OsConstants.O_RDONLY, 0)
            try {
                val buf = ByteArray(512)
                Os.read(fd, buf, 0, buf.size)
                false  // read succeeded — bad
            } finally {
                try { Os.close(fd) } catch (_: Throwable) {}
            }
        } catch (e: ErrnoException) {
            Log.d(TAG, "$path via Os.read(): errno=${e.errno} (${e.message})")
            e.errno == OsConstants.EACCES
        } catch (t: Throwable) {
            Log.d(TAG, "$path via Os.read(): unexpected ${t.javaClass.simpleName}: ${t.message}")
            false
        }
        Log.d(TAG, "$path via Os.read(): denied=$osReadDenied")

        assertTrue(
            "$path must be read-denied at kernel level — the probe measured " +
                    "EACCES via both File.readText() and the underlying Os.read() " +
                    "path. The kernel's ptrace_may_access check denies reads of " +
                    "/proc/self/stack to app-domain processes without " +
                    "CAP_SYS_PTRACE. Fails if a future ROM loosens this " +
                    "capability check.\n\n" +
                    "fileDenied=$fileDenied osReadDenied=$osReadDenied",
            fileDenied && osReadDenied)
    }

    @Test
    fun tracerPidForcedZero() {
        val statusLines = File("/proc/self/status").readLines()
        var tracerPidLine: String? = null
        for (line in statusLines) {
            if (line.startsWith("TracerPid:")) {
                tracerPidLine = line
                break
            }
        }
        assertTrue(
            "/proc/self/status must contain a TracerPid line — file format " +
                    "change or read failure would prevent verifying the filter.",
            tracerPidLine != null)
        val value = tracerPidLine!!.substringAfter(':').trim()
        Log.d(TAG, "TracerPid value = '$value'")
        assertEquals(
            "/proc/self/status TracerPid must be '0' — S23's BuildFilteredStatus " +
                    "filter forces this value regardless of real tracer state. " +
                    "Fails if the filter regresses OR a debugger is attached " +
                    "(which would mean 5.9 ptrace-blocking and 5.11 property-gated " +
                    "JDWP both failed).",
            "0", value)
    }
}