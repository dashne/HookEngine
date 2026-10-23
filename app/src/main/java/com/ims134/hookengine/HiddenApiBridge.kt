package com.ims134.hookengine

import android.util.Log

/**
 * Lifts hidden-API enforcement via reflection calls issued from a
 * freshly-attached native thread. Matches the pattern of
 * org.chickenhook.restrictionbypass.NativeReflectionBypass.
 *
 * The filtered Java methods (Class.getDeclaredMethod / getDeclaredField)
 * skip the hidden API check when the caller has no app classloader in
 * its stack. Calling them from a fresh AttachCurrentThread thread achieves
 * exactly that: no Java frames above the JNI boundary, so ART defaults
 * to a privileged caller domain.
 *
 * Works on ROMs where VMRuntime.setHiddenApiExemptions is removed.
 */
object HiddenApiBridge {
    private const val TAG = "DualCore"

    init { System.loadLibrary("hookengine") }

    /**
     * Returns the Field object for (class, name), obtained via reflection
     * from a native-attached thread. Null if the field doesn't exist OR
     * if the bypass was not effective on this ROM.
     */
    external fun reflectGetDeclaredField(cls: Class<*>, name: String): java.lang.reflect.Field?

    /**
     * Returns the Method object for (class, name, paramTypes), via the same
     * native-attached-thread path. Null on failure.
     */
    external fun reflectGetDeclaredMethod(
        cls: Class<*>, name: String, paramTypes: Array<Class<*>>
    ): java.lang.reflect.Method?

    /** Sets accessible on a member via the same bypass path. */
    external fun reflectSetAccessible(member: java.lang.reflect.AccessibleObject): Boolean

    @Volatile var lastError: String = ""
        private set
}