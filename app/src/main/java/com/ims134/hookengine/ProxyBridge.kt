package com.ims134.hookengine

/**
 * Session 13 — Java surface for ApplicationProxy dispatchers.
 *
 * nativeCNVM — JNI entry. Decomp-verified vs sub_30BF7C + sub_30C238.
 *
 * handleJHMethod — NOT declared here. The C++ symbol exists in
 * ProxyBridge.cpp but its behavior is unverified (no sub_29E168 decomp).
 * Shipping the Kotlin external would let ProxyBridgeSurfaceTest's four
 * handleJHMethod tests pass over an implementation known to be wrong.
 * Those four tests will fail with UnsatisfiedLinkError until
 * handleJHMethod is fixed in a future session with sub_29E168 in hand.
 */
object ProxyBridge {
    init { System.loadLibrary("hookengine") }

    external fun nativeCNVM(
        method: java.lang.reflect.Method,
        obj: Any?,
        sig: String,
        args: Array<Any?>
    )
}