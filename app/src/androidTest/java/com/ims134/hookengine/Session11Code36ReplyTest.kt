package com.ims134.hookengine

import android.os.Parcel
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class Session11Code36ReplyTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Before
    fun setup() {
        val ok = BinderTransactHook.install()
        assertTrue("hook install failed: ${BinderTransactHook.lastError}", ok)
        Thread.sleep(200)
        BinderTransactHook.drainCodes()
        Thread.sleep(50)
        BinderTransactHook.drainCodes()
    }

    @After
    fun cleanup() {
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
    }

    @Test
    fun dumpRealCode36ReplyForIda() {
        ctx.packageManager.getInstalledPackages(0)
        Thread.sleep(100)

        val bytes = NativeTestBridge.dumpLastCode36Reply()
        assertNotNull("no code-36 reply captured", bytes)
        assertTrue("reply too short (${bytes!!.size} bytes)", bytes.size > 16)

        // Log first 128 bytes as hex — this is what IDA needs.
        val head = bytes.take(minOf(bytes.size, 128))
            .joinToString(" ") { "%02x".format(it) }
        android.util.Log.d("Session11",
            "real code-36 reply size=${bytes.size} first128=$head")

        val p = Parcel.obtain()
        try {
            p.unmarshall(bytes, 0, bytes.size)
            p.setDataPosition(0)
            val ex  = p.readInt()
            val has = p.readInt()
            android.util.Log.d("Session11", "real reply ex=$ex has=$has")
        } finally {
            p.recycle()
        }
    }
}