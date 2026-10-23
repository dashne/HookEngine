package com.ims134.hookengine

import androidx.test.ext.junit.runners.AndroidJUnit4
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class DualApplicationTest {

    /**
     * By the time any test runs, DualApplication.attachBaseContext has already
     * executed. PmSpoof.install() was called there — proxy and mList field are
     * in place before guest code, and before any test code.
     */
    @Test
    fun pmSpoofInstalledAtStartup() {
        assertTrue(
            "PmSpoof must be installed by DualApplication.attachBaseContext, " +
                    "before any test runs",
            PmSpoof.isInstalled()
        )
    }

    /**
     * Same for ClassLoaderBridge.install() — must be called during
     * attachBaseContext, not deferred to test time.
     */
    @Test
    fun classLoaderBridgeInstalledAtStartup() {
        assertTrue(
            "ClassLoaderBridge must be installed by DualApplication.attachBaseContext",
            ClassLoaderBridge.isInstalled()
        )
    }
}