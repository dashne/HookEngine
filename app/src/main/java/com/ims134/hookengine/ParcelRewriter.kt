package com.ims134.hookengine

import android.content.pm.PackageInfo
import android.os.Parcel
import android.os.Parcelable
import android.util.Log

object ParcelRewriter {
    private const val TAG = "DualCore"
    private const val IFACE_TOKEN = "android.content.pm.IPackageManager"

    @Volatile var ourPackageName: String? = null

    /** Static accessor for C++ JNI (Kotlin object instance field is not static). */
    @JvmStatic
    fun getOurPackageNameForCpp(): String? = ourPackageName

    @JvmStatic
    fun rewriteReply(code: Int, data: Parcel, reply: Parcel) {
        val startSize = try { reply.dataSize() } catch (_: Throwable) { 0 }
        val ok = try {
            when (code) {
                3  -> rewriteGetPackageInfo(data, reply)
                36 -> rewriteGetInstalledPackages(reply)
            }
            true
        } catch (t: Throwable) {
            Log.e(TAG, "ParcelRewriter: code=$code threw: " +
                    "${t.javaClass.simpleName}: ${t.message}", t)
            false
        }
        if (ok) {
            try { reply.setDataSize(reply.dataPosition()) } catch (_: Throwable) { }
        } else {
            try { reply.setDataSize(startSize) } catch (_: Throwable) { }
        }
    }

    private fun readRequestedPackage(data: Parcel): String? {
        val pos = data.dataPosition()
        return try {
            data.setDataPosition(0)
            data.readInt()                                  // strict mode policy
            data.readInt()                                  // work source
            val magic = data.readInt()
            if (magic != 0x53595354) {
                Log.e(TAG, "ParcelRewriter: unexpected magic 0x${Integer.toHexString(magic)}")
                return null
            }
            val iface = data.readString()
            if (iface != IFACE_TOKEN) {
                Log.e(TAG, "ParcelRewriter: unexpected iface '$iface'")
                return null
            }
            data.readString()
        } catch (t: Throwable) {
            Log.e(TAG, "ParcelRewriter: readRequestedPackage threw: " +
                    "${t.javaClass.simpleName}: ${t.message}", t)
            null
        } finally {
            try { data.setDataPosition(pos) } catch (_: Throwable) {}
        }
    }

    private fun rewriteGetPackageInfo(data: Parcel, reply: Parcel) {
        val requested = readRequestedPackage(data) ?: return
        val ourPkg = ourPackageName ?: return
        if (requested != ourPkg) return

        reply.setDataPosition(0)
        val exception = reply.readInt()
        if (exception != 0) return
        val hasResult = reply.readInt()
        if (hasResult == 0) return

        val pi = PackageInfo.CREATOR.createFromParcel(reply)
        PmSpoof.nullOutSignatures(pi)

        reply.setDataPosition(0)
        reply.writeInt(0)
        reply.writeInt(1)
        pi.writeToParcel(reply, 0)
        Log.d(TAG, "ParcelRewriter: code 3 signatures nulled for $requested")
    }

    private fun rewriteGetInstalledPackages(reply: Parcel) {
        reply.setDataPosition(0)
        val exception = reply.readInt()
        if (exception != 0) return
        val hasResult = reply.readInt()
        if (hasResult == 0) return

        val slice = readParceledSlice(reply) ?: run {
            Log.e(TAG, "ParcelRewriter: readParceledSlice failed")
            return
        }
        val list = PmSpoof.getSliceListNative(slice) ?: run {
            Log.e(TAG, "ParcelRewriter: getSliceListNative failed on ${slice.javaClass.name}")
            return
        }
        var modified = 0
        for (item in list) {
            if (item is PackageInfo) {
                PmSpoof.nullOutSignatures(item)
                modified++
            }
        }
        if (modified == 0) return

        reply.setDataPosition(0)
        reply.writeInt(0)
        reply.writeInt(1)
        (slice as Parcelable).writeToParcel(reply, 0)
        Log.d(TAG, "ParcelRewriter: code 36 nulled signatures in $modified PackageInfos")
    }

    private fun readParceledSlice(reply: Parcel): Parcelable? {
        val cls = try {
            Class.forName("android.content.pm.ParceledListSlice")
        } catch (t: Throwable) {
            Log.e(TAG, "ParcelRewriter: ParceledListSlice class load failed: $t")
            return null
        }
        val field = HiddenApiBridge.reflectGetDeclaredField(cls, "CREATOR") ?: run {
            Log.e(TAG, "ParcelRewriter: CREATOR field not found")
            return null
        }
        if (!HiddenApiBridge.reflectSetAccessible(field)) {
            Log.e(TAG, "ParcelRewriter: setAccessible(CREATOR) failed")
            return null
        }
        val creator = try {
            field.get(null) as? Parcelable.Creator<*>
        } catch (t: Throwable) {
            Log.e(TAG, "ParcelRewriter: CREATOR.get(null) threw: $t")
            null
        } ?: return null
        return try {
            creator.createFromParcel(reply) as? Parcelable
        } catch (t: Throwable) {
            Log.e(TAG, "ParcelRewriter: createFromParcel threw: $t")
            null
        }
    }
}