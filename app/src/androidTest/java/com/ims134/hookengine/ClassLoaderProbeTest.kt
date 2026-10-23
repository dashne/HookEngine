package com.ims134.hookengine

import android.util.Log
import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith
import java.lang.reflect.Modifier

@RunWith(AndroidJUnit4::class)
class ClassLoaderProbeTest {

    @Test
    fun probeClassLoaderCandidates() {
        val candidates = listOf(
            Triple("dalvik.system.DexFile", "loadClassBinaryName",
                arrayOf(String::class.java, ClassLoader::class.java)),
            Triple("dalvik.system.DexFile", "loadClass",
                arrayOf(String::class.java, ClassLoader::class.java)),
            Triple("java.lang.ClassLoader", "loadClass",
                arrayOf(String::class.java)),
            Triple("java.lang.ClassLoader", "loadClass",
                arrayOf(String::class.java, Boolean::class.javaPrimitiveType)),
            Triple("java.lang.ClassLoader", "findClass",
                arrayOf(String::class.java)),
            Triple("dalvik.system.BaseDexClassLoader", "findClass",
                arrayOf(String::class.java)),
        )
        for ((cls, mth, params) in candidates) {
            try {
                val m = Class.forName(cls).getDeclaredMethod(mth, *params)
                val native = Modifier.isNative(m.modifiers)
                val vis = Modifier.toString(m.modifiers)
                Log.d("CLProbe", "$cls.$mth native=$native mods='$vis'")
            } catch (t: Throwable) {
                Log.d("CLProbe", "$cls.$mth NOT FOUND ($t)")
            }
        }
    }
}