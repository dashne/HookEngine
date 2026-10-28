package com.ims134.hookengine

import android.os.Process
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.net.InetAddress
import java.net.InetSocketAddress
import java.net.ServerSocket

/**
 * Session 63 — Feature 5.14 (/proc/net/tcp analysis blocking) probe.
 *
 * Diagnostic only. No assertions. Rule 12 does not apply to probes — the
 * purpose is to measure, not to gate. All output goes to logcat under
 * tag "NetTcpProbe".
 *
 * The six questions:
 *
 *   Q1. Raw file dump — what does /proc/net/tcp look like on this ROM?
 *       Line count, header, first 20 non-header lines verbatim. Establishes
 *       the format (Linux standard: `sl local_address rem_address st ...`)
 *       so the filter parser can be written against real data, not spec.
 *
 *   Q2. Path aliasing — are /proc/net/tcp, /proc/self/net/tcp, and
 *       /proc/<pid>/net/tcp the same file (byte-identical)? Determines
 *       which path variants `IsNetTcpPath` must match in Hooks.cpp.
 *
 *   Q3. Socket ownership — for each line in /proc/net/tcp, count by TCP
 *       state (LISTEN / ESTABLISHED / TIME_WAIT / other). Walk
 *       /proc/self/fd/, collect `socket:[inode]` entries, cross-correlate
 *       against /proc/net/tcp's inode column. Tells us:
 *       - Are any listening sockets held by THIS process? (Expected: zero.
 *         Engine binds nothing.)
 *       - Is inode-based filtering even viable as a design choice?
 *
 *   Q4. Loopback bind capability — the S42 wall. Try binding to
 *       127.0.0.1:0, ::1:0, and 127.0.0.1:27042. If bind works on this
 *       run, the positive test case ("engine's listener is hidden") can
 *       be live. If it fails (SocketException: EPERM), the case is
 *       @Ignore'd and the decision-function test carries the feature.
 *
 *   Q5. Java read path reaches the GOT hook? Baseline measurement.
 *       - File("/proc/net/tcp").readText()  → libjavacore GOT path
 *       - NativeTestBridge.readFileRaw()     → raw syscall path
 *       Both must return content pre-filter. This establishes the
 *       baseline. After the filter is installed (Case A of the decision
 *       matrix), the Java read MUST diverge from the raw read. If it
 *       does, hook fires on the Java path. If not, we're in Case B and
 *       need a different mechanism.
 *       NOTE: readFileRaw is truncated to 255 bytes by its JNI impl —
 *       we count '\n' characters in the returned prefix rather than
 *       expecting the full file.
 *
 *   Q6. Blocked-port hit count in the real file — count lines in
 *       /proc/net/tcp and /proc/net/tcp6 whose local port (field 2,
 *       after ':') is in {27042, 27043, 27052, 11111, 23946}.
 *       Expected: zero. Nothing on this device listens on those ports,
 *       so the filter has nothing to hide today — but the mechanism
 *       still exists as a defensive capability for future engine ports.
 *
 * @Before installFileHooks() — same pattern as prior probes
 * (AdbDetectTest, DebuggerDetectTest, EmulatorDetectTest, BuildPropTest).
 * Idempotent. Ensures the GOT hooks are live even if a prior test in the
 * suite polluted state.
 */
@RunWith(AndroidJUnit4::class)
class NetTcpProbeTest {

    private val TAG = "NetTcpProbe"

    // Ports already blocked by AntiDetection.cpp's IsBlockedPort
    private val BLOCKED_PORTS = listOf(27042, 27043, 27052, 11111, 23946)

    // TCP states (from net/tcp_states.h in the kernel)
    private val STATE_ESTABLISHED = "01"
    private val STATE_LISTEN      = "0A"
    private val STATE_TIME_WAIT   = "06"
    private val STATE_CLOSE_WAIT  = "08"

    @Before
    fun reinstallHooks() {
        val rc = NativeTestBridge.installFileHooks()
        Log.d(TAG, "installFileHooks() = $rc (0 = OK)")
    }

    // ─── Q1: raw file dumps ─────────────────────────────────────────
    private fun probeRawFileDumps() {
        Log.d(TAG, "=== Q1: raw /proc/net/* dumps ===")
        val paths = listOf(
            "/proc/net/tcp",
            "/proc/net/tcp6",
            "/proc/net/udp",
            "/proc/net/udp6",
            "/proc/net/unix"
        )
        for (path in paths) {
            try {
                val f = File(path)
                if (!f.exists()) {
                    Log.d(TAG, "  $path: DOES NOT EXIST")
                    continue
                }
                val lines = f.readLines()
                Log.d(TAG, "  $path: exists, ${lines.size} lines, ${f.length()} bytes")
                val show = minOf(lines.size, 21)  // header + 20 data lines
                for (i in 0 until show) {
                    Log.d(TAG, "    [$i] ${lines[i]}")
                }
                if (lines.size > 21) {
                    Log.d(TAG, "    ... (${lines.size - 21} more lines elided)")
                }
            } catch (t: Throwable) {
                Log.d(TAG, "  $path read threw: ${t.javaClass.simpleName}: ${t.message}")
            }
        }
    }

    // ─── Q2: path aliasing ──────────────────────────────────────────
    private fun probePathAliasing() {
        Log.d(TAG, "=== Q2: /proc/net/tcp path aliasing ===")
        val pid = Process.myPid()
        val variants = listOf(
            "/proc/net/tcp",
            "/proc/self/net/tcp",
            "/proc/$pid/net/tcp"
        )
        var firstContent: String? = null
        for (v in variants) {
            try {
                val content = File(v).readText()
                if (firstContent == null) {
                    firstContent = content
                    Log.d(TAG, "  $v: ${content.length} bytes (baseline)")
                } else {
                    val identical = content == firstContent
                    Log.d(TAG, "  $v: ${content.length} bytes, identical_to_baseline=$identical")
                }
            } catch (t: Throwable) {
                Log.d(TAG, "  $v threw: ${t.javaClass.simpleName}: ${t.message}")
            }
        }
    }

    // ─── Q3: socket ownership ───────────────────────────────────────
    private fun probeSocketOwnership() {
        Log.d(TAG, "=== Q3: socket ownership ===")
        val lines = try { File("/proc/net/tcp").readLines() }
        catch (t: Throwable) {
            Log.d(TAG, "  read threw: ${t.javaClass.simpleName}: ${t.message}")
            return
        }
        if (lines.size < 2) {
            Log.d(TAG, "  file too short (${lines.size} lines)")
            return
        }

        var established = 0
        var listen = 0
        var timeWait = 0
        var closeWait = 0
        var other = 0
        val listenInodes = mutableSetOf<Long>()
        val allInodes = mutableSetOf<Long>()

        for (i in 1 until lines.size) {
            val parts = lines[i].trim().split(Regex("\\s+"))
            if (parts.size < 10) continue
            val state = parts[3]
            val inode = parts[9].toLongOrNull() ?: continue
            allInodes.add(inode)
            when (state) {
                STATE_ESTABLISHED -> established++
                STATE_LISTEN -> {
                    listen++
                    listenInodes.add(inode)
                    val local = parts[1]
                    Log.d(TAG, "  LISTEN line[$i]: local=$local inode=$inode uid=${parts[7]}")
                }
                STATE_TIME_WAIT -> timeWait++
                STATE_CLOSE_WAIT -> closeWait++
                else -> other++
            }
        }

        Log.d(TAG, "  state counts: LISTEN=$listen ESTABLISHED=$established " +
                "TIME_WAIT=$timeWait CLOSE_WAIT=$closeWait other=$other " +
                "totalInodes=${allInodes.size}")

        // Walk /proc/self/fd/ — collect socket:[inode] entries held by us
        val ourInodes = mutableSetOf<Long>()
        val fdDir = File("/proc/self/fd")
        val fdFiles = fdDir.listFiles() ?: emptyArray()
        Log.d(TAG, "  /proc/self/fd has ${fdFiles.size} entries")
        for (fd in fdFiles) {
            try {
                val target = fd.canonicalPath  // resolves symlink
                if (target.startsWith("socket:[")) {
                    val inodeStr = target.removePrefix("socket:[").removeSuffix("]")
                    inodeStr.toLongOrNull()?.let { ourInodes.add(it) }
                }
            } catch (_: Throwable) {
                // fd closed between listFiles and canonicalPath — ignore
            }
        }
        Log.d(TAG, "  our process holds ${ourInodes.size} sockets (${ourInodes.size} fds are socket:[] entries)")
        if (ourInodes.isNotEmpty()) {
            for (inode in ourInodes.take(20)) {
                val inTcpTable = inode in allInodes
                val inListenSet = inode in listenInodes
                Log.d(TAG, "    our socket inode=$inode inTcpTable=$inTcpTable inListenSet=$inListenSet")
            }
            if (ourInodes.size > 20) {
                Log.d(TAG, "    ... (${ourInodes.size - 20} more)")
            }
        }
        val ourListeners = listenInodes.intersect(ourInodes)
        Log.d(TAG, "  our process LISTENING sockets in /proc/net/tcp: ${ourListeners.size}")
        if (ourListeners.isNotEmpty()) {
            Log.d(TAG, "    ⚠️  ${ourListeners} — engine/framework binds something on this run")
        }
    }

    // ─── Q4: loopback bind capability ──────────────────────────────
    private fun probeLoopbackBind() {
        Log.d(TAG, "=== Q4: loopback bind capability (S42 wall check) ===")
        val sockets = mutableListOf<ServerSocket>()

        // Case 1: 127.0.0.1:0 (ephemeral port)
        try {
            val s = ServerSocket()
            s.bind(InetSocketAddress(InetAddress.getByName("127.0.0.1"), 0))
            val actualPort = s.localPort
            Log.d(TAG, "  127.0.0.1:0 bind: OK — localPort=$actualPort")
            sockets.add(s)
            // Read /proc/net/tcp, look for our bound port
            try {
                val lines = File("/proc/net/tcp").readLines()
                var found = false
                for (line in lines.drop(1)) {
                    val parts = line.trim().split(Regex("\\s+"))
                    if (parts.size < 4) continue
                    val local = parts[1]
                    val portHex = local.substringAfter(":", "").takeIf { it.isNotEmpty() } ?: continue
                    if (portHex.toIntOrNull(16) == actualPort) {
                        Log.d(TAG, "    port $actualPort found in /proc/net/tcp: $line")
                        found = true
                        break
                    }
                }
                if (!found) {
                    Log.d(TAG, "    port $actualPort NOT visible in /proc/net/tcp")
                }
            } catch (t: Throwable) {
                Log.d(TAG, "    re-read threw: ${t.javaClass.simpleName}")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  127.0.0.1:0 bind threw: ${t.javaClass.simpleName}: ${t.message}")
        }

        // Case 2: ::1:0 (IPv6 loopback, ephemeral)
        try {
            val s = ServerSocket()
            s.bind(InetSocketAddress(InetAddress.getByName("::1"), 0))
            Log.d(TAG, "  [::1]:0 bind: OK — localPort=${s.localPort}")
            sockets.add(s)
        } catch (t: Throwable) {
            Log.d(TAG, "  [::1]:0 bind threw: ${t.javaClass.simpleName}: ${t.message}")
        }

        // Case 3: 127.0.0.1:27042 (blocked Frida port)
        try {
            val s = ServerSocket()
            s.bind(InetSocketAddress(InetAddress.getByName("127.0.0.1"), 27042))
            Log.d(TAG, "  127.0.0.1:27042 bind: OK (surprise — expected EPERM or address-in-use)")
            sockets.add(s)
        } catch (t: Throwable) {
            Log.d(TAG, "  127.0.0.1:27042 bind threw: ${t.javaClass.simpleName}: ${t.message}")
        }

        // Cleanup
        for (s in sockets) {
            try { s.close() } catch (_: Throwable) {}
        }
        Log.d(TAG, "  closed ${sockets.size} socket(s)")
    }

    // ─── Q5: java read vs raw syscall read (baseline) ──────────────
    private fun probeReadPathDivergence() {
        Log.d(TAG, "=== Q5: java File read vs raw syscall read (baseline) ===")
        Log.d(TAG, "  NOTE: readFileRaw is truncated to 255 bytes by its JNI impl.")
        Log.d(TAG, "  We count '\\n' in the returned prefix — pre-filter, both should")
        Log.d(TAG, "  agree on the number of lines visible in the first 255 bytes.")

        // Java path (through libjavacore GOT hook)
        try {
            val javaLines = File("/proc/net/tcp").readLines()
            Log.d(TAG, "  Java File.readLines(): ${javaLines.size} lines total")
        } catch (t: Throwable) {
            Log.d(TAG, "  Java read threw: ${t.javaClass.simpleName}: ${t.message}")
        }

        // Raw syscall path (bypasses GOT — kernel view)
        try {
            val raw = NativeTestBridge.readFileRaw("/proc/net/tcp")
            val rawLen = raw?.length ?: 0
            val rawNewlines = raw?.count { it == '\n' } ?: 0
            Log.d(TAG, "  Raw readFileRaw(): ${rawLen} bytes, $rawNewlines newlines " +
                    "(truncated prefix of the file)")
            if (raw != null && rawLen > 0) {
                val firstLine = raw.substringBefore('\n')
                Log.d(TAG, "  Raw first line: '$firstLine'")
            }
        } catch (t: Throwable) {
            Log.d(TAG, "  Raw read threw: ${t.javaClass.simpleName}: ${t.message}")
        }
    }

    // ─── Q6: blocked-port hit count in real file ───────────────────
    private fun probeBlockedPortHits() {
        Log.d(TAG, "=== Q6: blocked-port hits in real files ===")
        for (path in listOf("/proc/net/tcp", "/proc/net/tcp6")) {
            try {
                val lines = File(path).readLines()
                if (lines.size < 2) {
                    Log.d(TAG, "  $path: ${lines.size} lines (too short)")
                    continue
                }
                var hits = 0
                for (i in 1 until lines.size) {
                    val parts = lines[i].trim().split(Regex("\\s+"))
                    if (parts.size < 2) continue
                    val local = parts[1]
                    val portHex = local.substringAfter(":", "").takeIf { it.isNotEmpty() } ?: continue
                    val port = portHex.toIntOrNull(16) ?: continue
                    if (port in BLOCKED_PORTS) {
                        hits++
                        Log.d(TAG, "  $path[$i]: local=$local blocked_port=$port")
                    }
                }
                Log.d(TAG, "  $path: $hits blocked-port lines in ${lines.size - 1} data lines")
            } catch (t: Throwable) {
                Log.d(TAG, "  $path read threw: ${t.javaClass.simpleName}: ${t.message}")
            }
        }
    }

    @Test
    fun probeNetTcpSurface() {
        Log.d(TAG, "########## S63 NetTcpProbeTest START ##########")
        probeRawFileDumps()
        probePathAliasing()
        probeSocketOwnership()
        probeLoopbackBind()
        probeReadPathDivergence()
        probeBlockedPortHits()
        Log.d(TAG, "########## S63 NetTcpProbeTest END ##########")
    }
}