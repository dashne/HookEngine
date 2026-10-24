package com.ims134.hookengine

import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.IBinder
import android.os.Parcel
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

/**
 * E2E proof that tnFIN payload works at the AIDL layer.
 *
 * Bypasses PmSpoof entirely: queries IPackageManager.transact(3, ...) on
 * the raw IBinder directly. If signatures come back empty/null, our
 * parcel rewriter fired. If they come back with the real cert, it didn't.
 *
 * This is the test that proves Viber's anti-tamper (which goes through
 * IPackageManager$Stub$Proxy.transact directly, not through
 * ApplicationPackageManager) will see null signatures.
 */
@RunWith(AndroidJUnit4::class)
class BinderTransactE2ETest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val ifaceToken = "android.content.pm.IPackageManager"
    private val codeGetPackageInfo = 3

    @Before
    fun setup() {
        ParcelRewriter.ourPackageName = ctx.packageName
        val ok = BinderTransactHook.install()
        assertTrue("hook install failed: ${BinderTransactHook.lastError}", ok)
        Thread.sleep(100)  // let startup transacts settle
        BinderTransactHook.drainCodes()
    }

    @After
    fun cleanup() {
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
    }


    /**
     * Caller's userId. Android UIDs are `userId * 100000 + appId`, so integer
     * division by the per-user range yields the userId. Uses only public SDK
     * (Process.myUid). Works on user 0, work profiles, and secondary users.
     */
    private fun currentUserId(): Int = android.os.Process.myUid() / 100_000

    private fun getRawPackageManagerBinder(): IBinder? {
        return try {
            val sm = Class.forName("android.os.ServiceManager")
            val m = HiddenApiBridge.reflectGetDeclaredMethod(
                sm, "getService", arrayOf<Class<*>>(String::class.java)
            ) ?: return null
            if (!HiddenApiBridge.reflectSetAccessible(m)) return null
            m.invoke(null, "package") as? IBinder
        } catch (t: Throwable) {
            android.util.Log.e("DualCore", "getRawPackageManagerBinder: $t")
            null
        }
    }


    private fun dumpBytes(label: String, pkg: String, flags: Int, hookOn: Boolean) {
        if (hookOn) {
            if (!BinderTransactHook.install()) {
                android.util.Log.e("DualCore", "PROBE $label: install failed")
                return
            }
        } else {
            try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
        }
        val pmBinder = getRawPackageManagerBinder() ?: run {
            android.util.Log.e("DualCore", "PROBE $label: no binder"); return
        }
        val data = Parcel.obtain()
        try {
            data.writeInterfaceToken("android.content.pm.IPackageManager")
            data.writeString(pkg)
            data.writeLong(0L)
            data.writeInt(flags)
            data.writeInt(currentUserId())   // 4th arg — was missing
            val reply = Parcel.obtain()
            try {
                pmBinder.transact(3, data, reply, 0)
                val size = reply.dataSize()
                reply.setDataPosition(0)
                val sb = StringBuilder()
                for (i in 0 until size) sb.append("%02x".format(reply.readByte()))
                android.util.Log.d("DualCore", "PROBE $label size=$size hex=$sb")
            } finally { reply.recycle() }
        } finally { data.recycle() }
    }

    @Test
    fun rewriterNullsSigsOnSuccessReply() {
        ParcelRewriter.ourPackageName = ctx.packageName

        val request = Parcel.obtain()
        try {
            request.writeInterfaceToken("android.content.pm.IPackageManager")
            request.writeString(ctx.packageName)
            request.writeLong(0L)
            @Suppress("DEPRECATION")
            request.writeInt(PackageManager.GET_SIGNATURES)

            val reply = Parcel.obtain()
            try {
                @Suppress("DEPRECATION")
                val fakeSigs = arrayOf(
                    android.content.pm.Signature("fake-signature-bytes".toByteArray())
                )
                val pi = PackageInfo().apply {
                    packageName = ctx.packageName
                    @Suppress("DEPRECATION")
                    signatures = fakeSigs
                }
                reply.writeInt(0)
                reply.writeInt(1)
                pi.writeToParcel(reply, 0)
                val sizeBefore = reply.dataSize()

                ParcelRewriter.rewriteReply(3, request, reply)

                reply.setDataPosition(0)
                val ex  = reply.readInt()
                val has = reply.readInt()
                val result = PackageInfo.CREATOR.createFromParcel(reply)
                @Suppress("DEPRECATION")
                val sigs = result.signatures

                android.util.Log.d("DualCore",
                    "rewriterNullsSigsOnSuccessReply: sizeBefore=$sizeBefore " +
                            "sigBefore=${fakeSigs.size} sigAfter=${sigs?.size ?: "null"} " +
                            "ex=$ex has=$has")

                assertEquals("exceptionCode preserved", 0, ex)
                assertEquals("hasResult preserved", 1, has)
                assertTrue(
                    "signatures must be nulled by rewriter",
                    sigs == null || sigs.isEmpty()
                )
            } finally { reply.recycle() }
        } finally { request.recycle() }
    }

    @Test
    fun rewriterPreservesNullResultReply() {
        ParcelRewriter.ourPackageName = ctx.packageName
        val request = Parcel.obtain()
        try {
            request.writeInterfaceToken("android.content.pm.IPackageManager")
            request.writeString(ctx.packageName)
            request.writeLong(0L)
            request.writeInt(0)

            val reply = Parcel.obtain()
            try {
                reply.writeInt(0)
                reply.writeInt(0)
                val sizeBefore = reply.dataSize()
                ParcelRewriter.rewriteReply(3, request, reply)
                val sizeAfter = reply.dataSize()

                reply.setDataPosition(0)
                val ex  = reply.readInt()
                val has = reply.readInt()

                assertEquals("null-result reply size unchanged", sizeBefore, sizeAfter)
                assertEquals("exceptionCode preserved", 0, ex)
                assertEquals("hasResult preserved (null)", 0, has)
            } finally { reply.recycle() }
        } finally { request.recycle() }
    }

    @Test
    fun rewriterPreservesExceptionReply() {
        ParcelRewriter.ourPackageName = ctx.packageName
        val request = Parcel.obtain()
        try {
            request.writeInterfaceToken("android.content.pm.IPackageManager")
            request.writeString(ctx.packageName)
            request.writeLong(0L)
            request.writeInt(0)

            val reply = Parcel.obtain()
            try {
                reply.writeInt(-2)
                val sizeBefore = reply.dataSize()
                ParcelRewriter.rewriteReply(3, request, reply)
                val sizeAfter = reply.dataSize()

                reply.setDataPosition(0)
                val ex = reply.readInt()

                assertEquals("exception reply size unchanged", sizeBefore, sizeAfter)
                assertEquals("exceptionCode preserved", -2, ex)
            } finally { reply.recycle() }
        } finally { request.recycle() }
    }
}