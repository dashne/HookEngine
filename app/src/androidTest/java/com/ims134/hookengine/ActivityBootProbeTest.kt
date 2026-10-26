package com.ims134.hookengine

import android.app.Activity
import android.app.Application
import android.app.Instrumentation
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import android.util.Log
import android.view.Surface
import android.view.View
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.lang.reflect.Method
import java.util.Collections
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * Session 35 — Wall C5 probe.
 *
 * Session 34 closed Wall C4 (WMS accepted the stolen token, addView returned
 * success). But immediately after addView, isAttachedToWindow=false,
 * width/height=0, windowToken=null — because the first Choreographer
 * traversal (ViewRootImpl.performTraversals → dispatchAttachedToWindow) has
 * not fired yet. That traversal is the next category of wall (Wall C5).
 *
 * Two-phase structure:
 *   Phase 1 (main thread): newActivity + lifecycle + token steal + addView,
 *                          then RETURN so Looper can drain pending
 *                          Choreographer callbacks and run the traversal.
 *   Sleep 250 ms on test thread (never on main).
 *   Phase 2 (main thread): read post-traversal state — isAttachedToWindow,
 *                          width/height, windowToken, mAttachInfo,
 *                          ViewRootImpl.mSurface validity.
 *
 * If the traversal crashes (Wall C5 — likely IWindowSession identity check,
 * SurfaceFlinger buffer allocation, or EGL context setup), the FATAL kills
 * main; phase2Latch never fires; probe times out at 10 s. Wall C5 named by
 * the FATAL stack in logcat.
 *
 * Rule 12: probe reports outcomes, asserts only that outcomes were produced.
 */
@RunWith(AndroidJUnit4::class)
class ActivityBootProbeTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val tag = "ActivityBoot"

    // Captured in phase 1 for phase 2 reads (post-addView, post-traversal).
    @Volatile private var guestDecor: View? = null
    @Volatile private var guestActivity: Activity? = null

    private fun findMethod(cls: Class<*>, name: String, vararg params: Class<*>): Method? {
        var c: Class<*>? = cls
        while (c != null) {
            try {
                val m = c.getDeclaredMethod(name, *params)
                m.isAccessible = true
                return m
            } catch (_: NoSuchMethodException) { c = c.superclass }
        }
        return null
    }

    private fun reflectRead(obj: Any, name: String): Any? {
        return try {
            val f = HiddenApiBridge.reflectGetDeclaredField(obj.javaClass, name) ?: return null
            if (!HiddenApiBridge.reflectSetAccessible(f)) return null
            f.get(obj)
        } catch (_: Throwable) { null }
    }

    @Test
    fun probeActivityInstantiation() {
        val outcomes = Collections.synchronizedList(mutableListOf<String>())
        fun report(key: String, value: Any?) {
            val s = "$key=${value ?: "null"}"
            outcomes += s
            Log.d(tag, s)
        }

        val apk = File(ctx.filesDir, "viber.apk")
        assertTrue("stage viber.apk at ${apk.absolutePath}", apk.exists())

        // Permission self-check (dumpsys says granted; checkSelfPermission
        // reports denied via PmSpoof interference — cosmetic, prewarm proves
        // syscall-level access works).
        val permsToCheck = listOf(
            "android.permission.INTERNET",
            "android.permission.ACCESS_NETWORK_STATE",
            "android.permission.ACCESS_WIFI_STATE"
        )
        var permDenied = 0
        for (perm in permsToCheck) {
            val status = try { ctx.checkSelfPermission(perm) } catch (_: Throwable) { -1 }
            val name = perm.substringAfterLast('.')
            if (status == PackageManager.PERMISSION_GRANTED) report("perm.$name", "granted")
            else { report("perm.$name", "denied($status)"); permDenied++ }
        }
        report("perm.summary", "denied=$permDenied")

        // Guest boot
        val loadResult = try {
            GuestHost.load(ctx, apk.absolutePath)
        } catch (t: Throwable) {
            report("GuestHost.load.threw", "${t.javaClass.simpleName}: ${t.message}")
            finish(outcomes); return
        }
        report("guestBoot.outcome", loadResult.outcome)
        report("guestBoot.onCreated", loadResult.onCreated)

        val guestApp: Application? = loadResult.guestApp ?: ApplicationProxy.getGuestApplication()
        val guestCtx: Context? = guestApp?.baseContext
        if (guestApp == null || guestCtx == null) {
            report("guestApp.null", true); finish(outcomes); return
        }
        report("guestApp", guestApp.javaClass.name)
        report("guestCtx", guestCtx.javaClass.name)

        // Prewarm (skip-if-correct in rewriter validated Session 34)
        val settingsNames = listOf("window_animation_scale", "transition_animation_scale", "animator_duration_scale")
        var preWarmOk = 0
        var preWarmFail = 0
        for (name in settingsNames) {
            try {
                val v = Settings.Global.getInt(ctx.contentResolver, name, -1)
                report("preWarm.$name", "ok=$v"); preWarmOk++
            } catch (t: Throwable) {
                val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                report("preWarm.$name.threw", "${cause.javaClass.name}: ${cause.message}")
                preWarmFail++
            }
        }
        report("preWarm.summary", "ok=$preWarmOk fail=$preWarmFail")
        val canLaunchHost = (preWarmFail == 0)

        // Archive
        val pm = ctx.packageManager
        val flags = PackageManager.GET_ACTIVITIES or PackageManager.GET_META_DATA
        val archive = pm.getPackageArchiveInfo(apk.absolutePath, flags) ?: run {
            report("archive", "null"); finish(outcomes); return
        }
        val guestPkg = archive.packageName
        report("guestPkg", guestPkg)
        val activities = archive.activities?.filterNotNull() ?: emptyList()
        report("activityCount", activities.size)
        val launcher = activities.firstOrNull { ai ->
            ai.name?.let { n -> n.contains("MainActivity") || n.contains("SplashActivity") || n.contains("Launcher") } == true
        } ?: activities.firstOrNull()
        if (launcher == null) { report("launcher", "none"); finish(outcomes); return }
        report("launcher", launcher.name)
        val guestAppInfo = archive.applicationInfo?.apply {
            sourceDir = apk.absolutePath
            publicSourceDir = apk.absolutePath
        }
        launcher.applicationInfo = guestAppInfo

        // Loader + class
        val guestCL = guestCtx.classLoader
        report("guestCL", guestCL?.javaClass?.name)
        val activityClass = try { guestCL.loadClass(launcher.name) } catch (t: Throwable) {
            report("loadClass.threw", "${t.javaClass.simpleName}: ${t.message}")
            finish(outcomes); return
        }
        report("activityClass", activityClass.name)

        // ActivityThread + Instrumentation
        val atCls = Class.forName("android.app.ActivityThread")
        val curATM = HiddenApiBridge.reflectGetDeclaredMethod(atCls, "currentActivityThread", arrayOf())
        if (curATM == null) { report("currentActivityThread.method", "not found"); finish(outcomes); return }
        curATM.isAccessible = true
        val at = try { curATM.invoke(null) } catch (t: Throwable) {
            report("currentActivityThread.invoke.threw", "${t.javaClass.simpleName}: ${t.message}")
            finish(outcomes); return
        }
        report("activityThread", at?.javaClass?.name)

        val instrField = HiddenApiBridge.reflectGetDeclaredField(atCls, "mInstrumentation")
        if (instrField == null) { report("mInstrumentation.field", "not found"); finish(outcomes); return }
        HiddenApiBridge.reflectSetAccessible(instrField)
        val instrumentation = try { instrField.get(at) as? Instrumentation } catch (t: Throwable) {
            report("mInstrumentation.get.threw", "${t.javaClass.simpleName}: ${t.message}")
            finish(outcomes); return
        }
        report("instrumentation", instrumentation?.javaClass?.name)
        if (instrumentation == null) { finish(outcomes); return }

        // newActivity method
        val newActivityM = HiddenApiBridge.reflectGetDeclaredMethod(
            Instrumentation::class.java,
            "newActivity",
            arrayOf(
                Class::class.java, Context::class.java, IBinder::class.java,
                Application::class.java, Intent::class.java, ActivityInfo::class.java,
                CharSequence::class.java, Activity::class.java, String::class.java,
                Any::class.java
            )
        )
        if (newActivityM == null) { report("newActivity.method", "not found"); finish(outcomes); return }
        newActivityM.isAccessible = true

        val intent = Intent(Intent.ACTION_MAIN).apply {
            addCategory(Intent.CATEGORY_LAUNCHER)
            component = ComponentName(guestPkg, launcher.name)
        }

        // Host Activity launch (token source)
        var hostToken: IBinder? = null
        var hostTokenSource = "<skipped>"
        if (canLaunchHost) {
            report("hostLaunch.attempt", "true (pre-warm OK)")
            val mainIntent = Intent(ctx, MainActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            }
            var startSent = false
            try {
                ctx.startActivity(mainIntent); startSent = true; report("mainIntent.sent", "true")
            } catch (t: Throwable) { report("mainIntent.threw", "${t.javaClass.name}: ${t.message}") }

            if (startSent) {
                val mActivitiesField = HiddenApiBridge.reflectGetDeclaredField(atCls, "mActivities")
                if (mActivitiesField != null) {
                    HiddenApiBridge.reflectSetAccessible(mActivitiesField)
                    val deadline = System.currentTimeMillis() + 10_000L
                    var lastSize = -1
                    while (System.currentTimeMillis() < deadline && hostToken == null) {
                        try {
                            val map = mActivitiesField.get(at)
                            if (map is Map<*, *>) {
                                if (map.size != lastSize) {
                                    report("mActivities.size", map.size); lastSize = map.size
                                }
                                for (key in map.keys) {
                                    if (key is IBinder) { hostToken = key; hostTokenSource = "mActivities.key"; break }
                                }
                            }
                        } catch (_: Throwable) {}
                        if (hostToken == null) Thread.sleep(50)
                    }
                }
            }
        } else {
            report("hostLaunch.attempt", "false (pre-warm failed)")
        }
        report("hostToken.source", hostTokenSource)
        report("hostToken", hostToken)
        report("hostToken.hashCode", hostToken?.hashCode())

        // ── Phase 1: main thread — newActivity + lifecycle + steal + addView ──
        // Return from this block cleanly so Looper can drain pending
        // Choreographer callbacks and run the first traversal.
        val phase1Latch = CountDownLatch(1)
        Handler(Looper.getMainLooper()).post {
            try {
                val activity = try {
                    newActivityM.invoke(
                        instrumentation, activityClass, guestCtx, null, guestApp, intent,
                        launcher, launcher.name ?: "title", null, null, null
                    ) as? Activity
                } catch (t: Throwable) {
                    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                    report("newActivity.threw", "${cause.javaClass.name}: ${cause.message}")
                    report("newActivity.stack", cause.stackTraceToString().lines().take(20).joinToString(" | "))
                    return@post
                }
                report("newActivity.result", activity?.javaClass?.name)
                if (activity == null) return@post

                // Lifecycle callbacks (Session 28 stable)
                val onCreateM = findMethod(activityClass, "onCreate", Bundle::class.java)
                try { onCreateM?.invoke(activity, null as Bundle?); report("onCreate.result", "success") }
                catch (t: Throwable) {
                    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                    report("onCreate.threw", "${cause.javaClass.name}: ${cause.message}"); return@post
                }
                val onStartM = findMethod(activityClass, "onStart")
                try { onStartM?.invoke(activity); report("onStart.result", "success") }
                catch (t: Throwable) {
                    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                    report("onStart.threw", "${cause.javaClass.name}: ${cause.message}"); return@post
                }
                val onResumeM = findMethod(activityClass, "onResume")
                try { onResumeM?.invoke(activity); report("onResume.result", "success") }
                catch (t: Throwable) {
                    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                    report("onResume.threw", "${cause.javaClass.name}: ${cause.message}"); return@post
                }
                report("lifecycle.result", "onCreate+onStart+onResume all succeeded")

                // Token steal (Session 34 closed Wall C4)
                if (hostToken != null) {
                    try {
                        val f = HiddenApiBridge.reflectGetDeclaredField(Activity::class.java, "mToken")
                        if (f != null) {
                            HiddenApiBridge.reflectSetAccessible(f)
                            f.set(activity, hostToken)
                            report("steal.mToken.result", "success")
                        }
                    } catch (t: Throwable) { report("steal.mToken.threw", "${t.javaClass.name}: ${t.message}") }
                    try {
                        activity.window.attributes.token = hostToken
                        report("steal.lp.token.result", "success")
                    } catch (t: Throwable) { report("steal.lp.token.threw", "${t.javaClass.name}: ${t.message}") }
                    try {
                        val win = activity.window
                        var cls: Class<*>? = win.javaClass
                        var f: java.lang.reflect.Field? = null
                        while (cls != null && f == null) {
                            try { f = cls.getDeclaredField("mAppToken") }
                            catch (_: NoSuchFieldException) { cls = cls.superclass }
                        }
                        if (f != null) {
                            f.isAccessible = true
                            f.set(win, hostToken)
                            report("steal.mAppToken.result", "success")
                        }
                    } catch (t: Throwable) { report("steal.mAppToken.threw", "${t.javaClass.name}: ${t.message}") }
                }

                // addView — Wall C4 verification carried forward
                try {
                    val wm = activity.window.windowManager
                    val decor = activity.window.decorView
                    val lp = activity.window.attributes
                    report("wallC4.lp.token.hashCode", lp.token?.hashCode())
                    wm.addView(decor, lp)
                    report("wallC4.addView.result", "success")
                    // Capture refs for phase 2 (post-traversal reads)
                    guestDecor = decor
                    guestActivity = activity
                } catch (t: Throwable) {
                    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                    report("wallC4.threw", "${cause.javaClass.name}: ${cause.message}")
                    report("wallC4.stack", cause.stackTraceToString().lines().take(20).joinToString(" | "))
                }
            } catch (t: Throwable) {
                report("phase1.uncaught", "${t.javaClass.name}: ${t.message}")
            } finally {
                phase1Latch.countDown()
            }
        }

        val phase1Done = try { phase1Latch.await(60, TimeUnit.SECONDS) } catch (_: InterruptedException) { false }
        if (!phase1Done) { report("phase1.timeout", "60s"); finish(outcomes); return }

        // ── Wait ~250 ms for Choreographer → first traversal ──
        // Main thread has returned from phase 1. Looper drains and Choreographer
        // fires ViewRootImpl.performTraversals. If that crashes (Wall C5),
        // main thread dies; phase 2 latch times out; FATAL visible in logcat.
        Thread.sleep(250)

        // ── Phase 2: read post-traversal state ──
        val phase2Latch = CountDownLatch(1)
        Handler(Looper.getMainLooper()).post {
            try {
                val decor = guestDecor
                val activity = guestActivity
                if (decor == null || activity == null) {
                    report("phase2.failed", "no captured references")
                    return@post
                }

                report("wallC5.activity.isFinishing", activity.isFinishing)
                report("wallC5.activity.isDestroyed", activity.isDestroyed)
                report("wallC5.decorView.isAttachedToWindow", decor.isAttachedToWindow)
                report("wallC5.decorView.width", decor.width)
                report("wallC5.decorView.height", decor.height)
                report("wallC5.decorView.visibility", decor.visibility)
                report("wallC5.decorView.windowToken", decor.windowToken)
                report("wallC5.decorView.windowToken.hashCode", decor.windowToken?.hashCode())

                // ViewRootImpl via View.mParent
                try {
                    val f = HiddenApiBridge.reflectGetDeclaredField(View::class.java, "mParent")
                    if (f == null) {
                        report("wallC5.viewRootImpl", "mParent field not found")
                    } else {
                        HiddenApiBridge.reflectSetAccessible(f)
                        val viewRoot = f.get(decor)
                        report("wallC5.viewRootImpl", viewRoot?.javaClass?.name)
                        if (viewRoot != null) {
                            val surface = reflectRead(viewRoot, "mSurface")
                            report("wallC5.viewRootImpl.mSurface",
                                when (surface) {
                                    is Surface -> "Surface(isValid=${surface.isValid})"
                                    null -> "null"
                                    else -> surface.toString()
                                })
                            report("wallC5.viewRootImpl.mAdded", reflectRead(viewRoot, "mAdded"))
                            report("wallC5.viewRootImpl.mFirst", reflectRead(viewRoot, "mFirst"))
                            report("wallC5.viewRootImpl.mAppVisible", reflectRead(viewRoot, "mAppVisible"))
                        }
                    }
                } catch (t: Throwable) {
                    report("wallC5.viewRootImpl.threw", "${t.javaClass.simpleName}: ${t.message}")
                }

                // mAttachInfo via View
                try {
                    val f = HiddenApiBridge.reflectGetDeclaredField(View::class.java, "mAttachInfo")
                    if (f != null) {
                        HiddenApiBridge.reflectSetAccessible(f)
                        val ai = f.get(decor)
                        report("wallC5.mAttachInfo", ai?.javaClass?.name)
                        if (ai != null) {
                            report("wallC5.mAttachInfo.mWindowToken", reflectRead(ai, "mWindowToken"))
                        }
                    }
                } catch (t: Throwable) {
                    report("wallC5.mAttachInfo.threw", "${t.javaClass.simpleName}: ${t.message}")
                }

                report("wallC5.readComplete", "true")
            } catch (t: Throwable) {
                report("phase2.uncaught", "${t.javaClass.simpleName}: ${t.message}")
            } finally {
                phase2Latch.countDown()
            }
        }

        val phase2Done = try { phase2Latch.await(10, TimeUnit.SECONDS) } catch (_: InterruptedException) { false }
        if (!phase2Done) report("phase2.timeout", "10s — main thread dead (Wall C5)")

        finish(outcomes)
    }

    private fun finish(outcomes: List<String>) {
        val f = File(ctx.filesDir, "activity-boot.txt")
        f.writeText(outcomes.joinToString("\n"))
        Log.d(tag, "activity-boot.txt written: ${f.absolutePath} (${outcomes.size} lines)")
        assertTrue("probe ran and produced outcomes", outcomes.isNotEmpty())
    }
}