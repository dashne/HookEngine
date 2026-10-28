package com.ims134.hookengine

import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.io.FileNotFoundException

/**
 * Session 63 — Feature 5.14: /proc/net/tcp analysis blocking.
 *
 * ─── Mechanism: SELinux policy denies app-domain reads. No engine code. ──
 *
 * The spec goal was: "filter /proc/net/tcp and /proc/net/tcp6 — hide
 * listening sockets belonging to engine." The S50/S51 precedent — memfd
 * fd returning filtered bytes via a Fake_open GOT hook — was the
 * anticipated mechanism.
 *
 * The S63 probe (NetTcpProbeTest, on-device measurement) revealed a
 * stronger wall: SELinux policy on this MIUI/API 37 ROM classifies the
 * files /proc/net/tcp, tcp6, udp, udp6 under `proc_net_tcp_udp` and
 * /proc/net/unix under `proc_net`. The `untrusted_app` SELinux domain
 * (every app on the platform) is denied { read } on both contexts with
 * permissive=0 (enforcing). Every read path fails identically:
 *
 *   File("/proc/net/tcp").readText()       → FileNotFoundException: EACCES
 *   FileInputStream("/proc/net/tcp")       → same
 *   Os.open("/proc/net/tcp", O_RDONLY, 0)  → ErrnoException EACCES
 *   NativeTestBridge.readFileRaw(...)      → 0 bytes (kernel-denied)
 *
 * A detector running in this process needs to READ /proc/net/tcp to see
 * any engine listener. It can't. The detector gets the same EACCES the
 * engine does. There is no data to filter and no data for the detector
 * to read.
 *
 * **Feature 5.14 is achieved by the OS kernel policy, not by an engine
 * mechanism.** No Hooks.cpp changes, no NativeTestBridge changes, no
 * BuildFilteredNetTcp function. The "functioning goal is achieved by
 * the process's real state" pattern from S61 extends one layer deeper:
 * this time it's the OS kernel MAC policy, not the device identity.
 *
 * ─── Companion finding — S42 loopback bind wall regressed ───────────────
 *
 * NetTcpProbeTest Q4 measured that ServerSocket().bind(127.0.0.1:0) and
 * ::1:0 and 127.0.0.1:27042 all succeed on this run. S42's
 * AntiDetectionE2ETest has 3 tests @Ignore'd with "Loopback bind EPERM
 * on API 37 / HyperOS". That premise is no longer true on this build.
 * This is orthogonal to 5.14 (bind capability is preserved — the OS
 * policy is what keeps any future listener invisible), but it is a
 * Phase F candidate for re-measurement. Noted in PROJECT_STATE §5.14.
 *
 * ─── What's NOT covered by 5.14 (and out of scope) ──────────────────────
 *
 * - /proc/self/fd/ enumeration: readable, but each socket fd shows only
 *   as "socket:[inode]" — no port info. A detector could know "this
 *   process has a socket open" but not "which ports." Weak signal, and
 *   would require synthesizing the fd table via Fake_getdents64 /
 *   Fake_readdir. Distinct feature.
 * - /proc/<other_pid>/net/tcp: cross-process enumeration — same SELinux
 *   context, same denial. Confirmed by Q2 of the probe.
 * - /proc/net/route, /proc/net/dev, /proc/net/arp: not probed; likely
 *   denied under the same app-domain policy. A future session can widen
 *   the test class if/when a detector actually reads them.
 *
 * ─── Rule 12 — every assertion is falsifiable ──────────────────────────
 *
 *   1. procNetTcpUnreadableViaFile      — fails if the ROM loosens
 *                                          SELinux policy OR an app-level
 *                                          filter returns synthetic
 *                                          content from an otherwise-
 *                                          denied read
 *   2. procNetTcpUnreadableViaOsOpen    — fails if the denial is only at
 *                                          a Java layer (not kernel)
 *   3. procNetTcp6UnreadableViaFile     — IPv6 TCP table
 *   4. procNetUdpUnreadableViaFile      — UDP table
 *   5. procNetUdp6UnreadableViaFile     — IPv6 UDP table
 *   6. procNetUnixUnreadableViaFile     — Unix-domain socket table
 *
 * Each assertion fails on a specific ROM change or a specific policy
 * loosening. None is a tautology.
 *
 * ─── Test isolation ────────────────────────────────────────────────────
 *
 * @Before calls NativeTestBridge.installFileHooks() — same pattern as
 * AdbDetectTest / DebuggerDetectTest / EmulatorDetectTest / BuildPropTest.
 * NativeItemTest (alphabetically before NetTcpProbeTest / NetTcpTest)
 * calls uninstallAll() in @After; the re-install is idempotent.
 *
 * Note: this test class's assertions do NOT depend on hooks being
 * installed. The EACCES denial is enforced by the kernel regardless of
 * GOT hook state. The @Before is defensive only — if a future session
 * adds a legitimate /proc/net filter, this test class would still pass
 * because the assertion is about the OS-level denial, not about whether
 * our own GOT hook fires.
 */
@RunWith(AndroidJUnit4::class)
class NetTcpTest {

    private val TAG = "NetTcpTest"

    @Before
    fun reinstallHooks() {
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    /**
     * Classify the outcome of a File.readText() attempt into one of:
     *   DENIED_FILE_NOT_FOUND ...   → good (EACCES or ENOENT)
     *   READABLE (N bytes)          → bad (file is accessible)
     *   UNEXPECTED_*                → bad (some other failure mode)
     *
     * Both EACCES and ENOENT count as "denied" for the assertion's
     * purposes: in either case, a detector cannot read the file content.
     * EACCES is the expected state on this ROM; ENOENT would mean the
     * procfs node was removed, which is a different but equally valid
     * defense. Only READABLE fails the assertion.
     */
    private fun probeFileReadable(path: String): String {
        return try {
            val content = File(path).readText()
            "READABLE (${content.length} bytes)"
        } catch (e: FileNotFoundException) {
            "DENIED_FILE_NOT_FOUND (${e.message})"
        } catch (t: Throwable) {
            "UNEXPECTED_${t.javaClass.simpleName} (${t.message})"
        }
    }

    private fun isDenied(result: String): Boolean = result.startsWith("DENIED")

    // ─── 1. /proc/net/tcp via File API ──────────────────────────────
    @Test
    fun procNetTcpUnreadableViaFile() {
        val path = "/proc/net/tcp"
        val result = probeFileReadable(path)
        Log.d(TAG, "$path via File.readText(): $result")
        assertTrue(
            "$path must NOT be readable from this process.\n\n" +
                    "SELinux policy on MIUI/API 37 sets app-domain (untrusted_app) " +
                    "access to proc_net_tcp_udp as DENY with permissive=0 (enforcing). " +
                    "This is what makes Feature 5.14's 'hide our listening sockets' " +
                    "goal hold without any engine mechanism — a detector running in " +
                    "this process gets the same EACCES the engine does, sees no " +
                    "socket data.\n\n" +
                    "Fails if:\n" +
                    "  (a) the ROM loosens its SELinux policy and makes the file " +
                    "readable, OR\n" +
                    "  (b) an app-level filter is installed that returns synthetic " +
                    "content from an otherwise-denied read.\n\n" +
                    "In case (a) a /proc/net/tcp filter mechanism (memfd + " +
                    "BuildFilteredNetTcp in Hooks.cpp) would now be required — this " +
                    "test failing is the early warning.\n\n" +
                    "Observed: $result",
            isDenied(result)
        )
    }

    // ─── 2. /proc/net/tcp via direct kernel open ────────────────────
    @Test
    fun procNetTcpUnreadableViaOsOpen() {
        val path = "/proc/net/tcp"
        val result = try {
            val fd = Os.open(path, OsConstants.O_RDONLY, 0)
            try { Os.close(fd) } catch (_: Throwable) {}
            "READABLE (fd open succeeded)"
        } catch (e: ErrnoException) {
            val name = OsConstants.errnoName(e.errno) ?: "unknown(${e.errno})"
            "DENIED_ERRNO_${name} (${e.message})"
        } catch (t: Throwable) {
            "UNEXPECTED_${t.javaClass.simpleName} (${t.message})"
        }
        Log.d(TAG, "$path via Os.open(): $result")
        assertTrue(
            "Belt-and-suspenders: even the direct Os.open (kernel-level " +
                    "openat syscall, bypassing File / FileInputStream / any JVM " +
                    "layer) must be denied.\n\n" +
                    "If the File.readText path were denied at the JVM layer but the " +
                    "direct syscall succeeded, a sophisticated detector using " +
                    "android.system.Os could bypass the JVM filter. SELinux denies " +
                    "at the kernel level, so both paths must fail identically.\n\n" +
                    "Fails if the denial is only at a Java layer, not kernel.\n\n" +
                    "Observed: $result",
            isDenied(result)
        )
    }

    // ─── 3. /proc/net/tcp6 ──────────────────────────────────────────
    @Test
    fun procNetTcp6UnreadableViaFile() {
        val path = "/proc/net/tcp6"
        val result = probeFileReadable(path)
        Log.d(TAG, "$path via File.readText(): $result")
        assertTrue(
            "$path (IPv6 TCP table) must NOT be readable from this process.\n\n" +
                    "Same SELinux context (proc_net_tcp_udp) as /proc/net/tcp. A " +
                    "detector checking for Frida on IPv6 loopback (::1) needs this " +
                    "file; the EACCES denial covers the IPv6 vector as well as the " +
                    "IPv4 vector.\n\n" +
                    "Observed: $result",
            isDenied(result)
        )
    }

    // ─── 4. /proc/net/udp ───────────────────────────────────────────
    @Test
    fun procNetUdpUnreadableViaFile() {
        val path = "/proc/net/udp"
        val result = probeFileReadable(path)
        Log.d(TAG, "$path via File.readText(): $result")
        assertTrue(
            "$path must NOT be readable from this process.\n\n" +
                    "UDP Frida variants exist; a detector checking for UDP " +
                    "debugging channels needs this file. The same proc_net_tcp_udp " +
                    "SELinux context covers tcp + tcp6 + udp + udp6 — one policy " +
                    "decision, four files covered.\n\n" +
                    "Observed: $result",
            isDenied(result)
        )
    }

    // ─── 5. /proc/net/udp6 ──────────────────────────────────────────
    @Test
    fun procNetUdp6UnreadableViaFile() {
        val path = "/proc/net/udp6"
        val result = probeFileReadable(path)
        Log.d(TAG, "$path via File.readText(): $result")
        assertTrue(
            "$path (IPv6 UDP table) must NOT be readable from this " +
                    "process. Same SELinux context as /proc/net/udp.\n\n" +
                    "Observed: $result",
            isDenied(result)
        )
    }

    // ─── 6. /proc/net/unix ──────────────────────────────────────────
    @Test
    fun procNetUnixUnreadableViaFile() {
        val path = "/proc/net/unix"
        val result = probeFileReadable(path)
        Log.d(TAG, "$path via File.readText(): $result")
        assertTrue(
            "$path (Unix-domain socket table) must NOT be readable from " +
                    "this process.\n\n" +
                    "Frida variants that use a Unix socket instead of TCP would " +
                    "appear here. Denial + the Frida-port block from 5.8 together " +
                    "cover the unix-socket detection vector. SELinux context is " +
                    "proc_net (broader than proc_net_tcp_udp) but still app-domain " +
                    "denied.\n\n" +
                    "Observed: $result",
            isDenied(result)
        )
    }
}