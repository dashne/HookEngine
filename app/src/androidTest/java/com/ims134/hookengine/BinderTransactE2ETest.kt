package com.ims134.hookengine

import android.content.pm.PackageInfo
import android.content.pm.PackageManager
import android.os.Parcel
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertTrue
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
        // getInstalledPackages returns a large uncacheable reply —
        // reliably hits IPC on every call. Avoids framework cache
        // that made getPackageInfo(pkg, GET_SIGNATURES) a no-op.
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
        // rewriteCount increments in Hook_Transact when MatchesRequest returns
        // true, before the rewriter body runs. getInstalledPackages is code 36
        // (matches any package) — reliably dispatches, never cached.
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
        // The primary win over Session 10: caller thread does pure C++ only.
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
}