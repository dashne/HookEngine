package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class BinderProbeTest {
    @Test
    fun probeBinderSymbols() {
        val n = NativeTestBridge.binderProbe()
        android.util.Log.d("DualCore", "BinderProbeTest: $n symbols found")
        // No assertion — this is diagnostic. Read logcat.
    }
}