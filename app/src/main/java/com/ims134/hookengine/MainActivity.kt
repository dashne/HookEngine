package com.ims134.hookengine

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.view.Gravity
import android.widget.TextView

class MainActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // Start the background service
        val intent = Intent(this, RemoteService::class.java)
        startService(intent)

        // Plain TextView, no Compose.
        //
        // Session 34 — Compose removed. AndroidComposeView.onAttachedToWindow
        // calls StrictMode.setVmPolicy during its first traversal. On API 37
        // that method transacts to INetworkManagementService.setUidCleartextNetworkPolicy,
        // which requires android.permission.NETWORK_STACK or
        // android.permission.MAINLINE_NETWORK_STACK — signature permissions
        // not grantable to normal apps. FATAL on main, process killed,
        // ActivityBootProbeTest's posted runnable never runs.
        //
        // MainActivity's role in the probe: launchable Activity that goes
        // through AMS and registers in ActivityThread.mActivities[0] —
        // that token is the input to Wall C4's steal probe. UI is not part
        // of the workflow. Guest SplashActivity doesn't use Compose, so this
        // crash was never from the guest side — it was our own scaffolding.
        val tv = TextView(this).apply {
            text = "Check Logcat for Service results!"
            textSize = 20f
            gravity = Gravity.CENTER
        }
        setContentView(tv)
    }
}