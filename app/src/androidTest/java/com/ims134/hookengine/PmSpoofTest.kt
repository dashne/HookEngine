package com.ims134.hookengine

import android.content.pm.PackageManager
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.After
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith

@RunWith(AndroidJUnit4::class)
class PmSpoofTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext

    @Before
    fun setup() {
        PmSpoof.hiddenPackages = emptySet()
        PmSpoof.install(ctx)
    }

    @After
    fun cleanup() {
        PmSpoof.hiddenPackages = emptySet()
    }

    @Test
    fun signatureSpoofWorksOnSelf() {
        @Suppress("DEPRECATION")
        val pi = ctx.packageManager.getPackageInfo(ctx.packageName, PackageManager.GET_SIGNATURES)
        @Suppress("DEPRECATION")
        val sigs = pi.signatures
        assertTrue("signatures must be empty after PmSpoof", sigs == null || sigs.isEmpty())
    }

    @Test
    fun installedPackagesFiltersSelfWhenHidden() {
        PmSpoof.hiddenPackages = setOf(ctx.packageName)
        val installed = ctx.packageManager.getInstalledPackages(0)
        assertFalse(
            "hidden self must not appear in getInstalledPackages",
            installed.any { it.packageName == ctx.packageName }
        )
    }

    @Test
    fun installedPackagesShowsSelfWhenNotHidden() {
        PmSpoof.hiddenPackages = emptySet()
        val installed = ctx.packageManager.getInstalledPackages(0)
        assertTrue(
            "self must appear in getInstalledPackages when not hidden",
            installed.any { it.packageName == ctx.packageName }
        )
    }

    @Test
    fun installedApplicationsFiltersSelfWhenHidden() {
        PmSpoof.hiddenPackages = setOf(ctx.packageName)
        val apps = ctx.packageManager.getInstalledApplications(0)
        assertFalse(
            "hidden self must not appear in getInstalledApplications",
            apps.any { it.packageName == ctx.packageName }
        )
    }
}