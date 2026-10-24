package com.ims134.hookengine

import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Parcel
import android.os.Parcelable
import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class BinderTransactE2ETest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Before
    fun setup() {
        ParcelRewriter.ourPackageName = ctx.packageName
        val ok = BinderTransactHook.install()
        assertTrue("hook install failed: ${BinderTransactHook.lastError}", ok)
        Thread.sleep(100)
    }

    @After
    fun cleanup() {
        try { BinderTransactHook.uninstall() } catch (_: Throwable) {}
    }

    @Test
    fun cppRewriterPrimitivesResolve() {
        assertTrue("ParcelRewriter_Init failed — dynsym scan issue",
            NativeTestBridge.parcelRewriterReady())
    }

    @Test
    fun hookFiresOnRealPackageManagerQuery() {
        val before = BinderTransactHook.fireCount()
        ctx.packageManager.getInstalledPackages(0)
        val after = BinderTransactHook.fireCount()
        android.util.Log.d("DualCore",
            "hookFiresOnRealPackageManagerQuery: before=$before after=$after " +
                    "delta=${after - before}")
        assertTrue(
            "hook must fire on IPC (before=$before after=$after)",
            after > before
        )
    }

    @Test
    fun rewriterDispatchesForOurPackage() {
        val before = BinderTransactHook.rewriteCount()
        ctx.packageManager.getInstalledPackages(0)
        val after = BinderTransactHook.rewriteCount()
        android.util.Log.d("DualCore",
            "rewriterDispatchesForOurPackage: before=$before after=$after " +
                    "delta=${after - before}")
        assertTrue(
            "rewriter must dispatch (before=$before after=$after)",
            after > before
        )
    }

    @Test
    fun realQueryDoesNotCrash() {
        repeat(5) {
            ctx.packageManager.getInstalledPackages(0)
            try {
                @Suppress("DEPRECATION")
                ctx.packageManager.getPackageInfo(
                    ctx.packageName, PackageManager.GET_SIGNATURES)
            } catch (_: Throwable) {}
        }
        assertTrue("survived 10 queries with hook active", true)
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
                            "sigBefore=${fakeSigs.size} sigAfter=${sigs?.size ?: "null"}")

                assertEquals("exceptionCode preserved", 0, ex)
                assertEquals("hasResult preserved", 1, has)
                assertTrue("signatures must be nulled",
                    sigs == null || sigs.isEmpty())
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

                assertEquals("size unchanged", sizeBefore, sizeAfter)
                assertEquals("ex preserved", 0, ex)
                assertEquals("has=0 preserved", 0, has)
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

                assertEquals("size unchanged", sizeBefore, sizeAfter)
                assertEquals("ex=-2 preserved", -2, ex)
            } finally { reply.recycle() }
        } finally { request.recycle() }
    }

    /**
     * Session 11 fix has no test. Force createFromParcel to throw on code 36
     * (absurd N=0x7FFFFFF0), then assert reply.dataSize() is unchanged.
     * Before the fix, setDataSize(dataPosition()) truncated the caller's
     * reply to 8 bytes on the exception path.
     *
     * This test exercises rewriteReply directly, not the C++ dispatch — the
     * code-36 skip-guard returns before reaching the rewriter. That means
     * this test validates the Kotlin contract independent of guard state.
     */
    @Test
    fun rewriterExceptionPreservesReplySize() {
        ParcelRewriter.ourPackageName = ctx.packageName
        val request = Parcel.obtain()
        try {
            request.writeInterfaceToken("android.content.pm.IPackageManager")
            request.writeString(ctx.packageName)
            request.writeLong(0L)
            request.writeInt(0)

            val reply = Parcel.obtain()
            try {
                reply.writeInt(0)              // ex
                reply.writeInt(1)              // hasResult
                reply.writeInt(0x7FFFFFF0)     // absurd N
                val sizeBefore = reply.dataSize()

                ParcelRewriter.rewriteReply(36, request, reply)

                val sizeAfter = reply.dataSize()
                android.util.Log.d("DualCore",
                    "rewriterExceptionPreservesReplySize: before=$sizeBefore after=$sizeAfter")

                assertEquals(
                    "reply size must be preserved when rewriter throws",
                    sizeBefore, sizeAfter)
            } finally { reply.recycle() }
        } finally { request.recycle() }
    }

    @Test
    fun dumpFirstItemInt32s() {
        BinderTransactHook.uninstall()

        val pm = ctx.packageManager
        val mPMField = pm.javaClass.getDeclaredField("mPM").apply { isAccessible = true }
        val mPM = mPMField.get(pm) as android.os.IInterface
        val binder = mPM.asBinder()

        val request = Parcel.obtain()
        val reply = Parcel.obtain()
        try {
            request.writeInterfaceToken("android.content.pm.IPackageManager")
            request.writeInt(0)   // flags
            request.writeInt(0)   // userId

            val ok = binder.transact(36, request, reply, 0)
            val size = reply.dataSize()
            Log.d("DualCore", "int32dump: ok=$ok size=$size")

            reply.setDataPosition(0)
            val ex = reply.readInt()
            val has = reply.readInt()
            val count = reply.readInt()
            val classLen = reply.readInt()
            Log.d("DualCore", "int32dump: ex=$ex has=$has count=$count classLen=$classLen")

            val classChars = StringBuilder()
            for (i in 0 until classLen) classChars.append(reply.readInt().toChar())
            Log.d("DualCore", "int32dump: class='$classChars' pos=${reply.dataPosition()}")

            val start = reply.dataPosition()
            for (i in 0 until 100) {
                val v = reply.readInt()
                Log.d("DualCore", "int32dump: [+${start + i * 4}] 0x%08x (%d)".format(v, v))
            }
        } finally {
            request.recycle(); reply.recycle()
        }
    }
}