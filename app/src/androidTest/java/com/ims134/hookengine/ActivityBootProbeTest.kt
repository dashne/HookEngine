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
 * Session 31 (v5) — Wall C4 token steal, permission-verified, crash-logged.
 *
 * v4 (previous) hit a NEW wall: ACCESS_NETWORK_STATE denial during the
 * MainActivity launch path. Manifest v5 declares INTERNET,
 * ACCESS_NETWORK_STATE, ACCESS_WIFI_STATE. This probe adds:
 *
 *   1. Explicit permission self-check — reports granted/denied for each,
 *      proving whether the manifest rebuild took (Rule 14 protection).
 *
 *   2. Uncaught exception handler that logs FATAL to a file BEFORE the
 *      process can die from it. If a SecurityException still happens on
 *      main during MainActivity launch, we capture the trace and can
 *      diagnose from activity-boot-uncaught.txt even after am instrument
 *      reports failure or hangs.
 *
 *   3. Pre-warm guard kept — if Settings.Global reads still fail (Wall C2b),
 *      skip MainActivity launch and report AMS-registration verdict cleanly
 *      without risking the process kill path.
 *
 * Four verdicts:
 *   A — preWarm OK, permissions granted, MainActivity launches, token
 *       captured, addView succeeds → Wall C4 operational. Session 32 makes
 *       token handoff portable.
 *   B — addView throws SecurityException → Wall C4-WMS. Session 32 = 1-line
 *       packageName fix.
 *   C — permissions denied → manifest didn't rebuild / install. Rule 14
 *       discipline gap. Reinstall and rerun.
 *   D — preWarm fails → Wall C2b fix insufficient. Report AMS-registration
 *       verdict. No process kill.
 */
@RunWith(AndroidJUnit4::class)
class ActivityBootProbeTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val tag = "ActivityBoot"

    private fun findMethod(cls: Class<*>, name: String, vararg params: Class<*>): Method? {
        var c: Class<*>? = cls
        while (c != null) {
            try {
                val m = c.getDeclaredMethod(name, *params)
                m.isAccessible = true
                return m
            } catch (_: NoSuchMethodException) {
                c = c.superclass
            }
        }
        return null
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

        // ═══════════════════════════════════════════════════════════════
        // Uncaught handler — logs any FATAL to a file before process dies
        // ═══════════════════════════════════════════════════════════════
        val uncaughtFile = File(ctx.filesDir, "activity-boot-uncaught.txt")
        try { uncaughtFile.writeText("") } catch (_: Throwable) {}
        val prevHandler = Thread.getDefaultUncaughtExceptionHandler()
        Thread.setDefaultUncaughtExceptionHandler { thread, throwable ->
            try {
                uncaughtFile.appendText(
                    "UNCAUGHT thread='${thread.name}' id=${thread.id}\n" +
                            "exception=${throwable.javaClass.name}: ${throwable.message}\n" +
                            throwable.stackTraceToString() + "\n\n"
                )
            } catch (_: Throwable) {}
            prevHandler?.uncaughtException(thread, throwable)
        }

        // ═══════════════════════════════════════════════════════════════
        // Permission self-check (Rule 14 protection)
        // ═══════════════════════════════════════════════════════════════
        val permsToCheck = listOf(
            "android.permission.INTERNET",
            "android.permission.ACCESS_NETWORK_STATE",
            "android.permission.ACCESS_WIFI_STATE"
        )
        var permDenied = 0
        for (perm in permsToCheck) {
            val status = try { ctx.checkSelfPermission(perm) } catch (t: Throwable) { -1 }
            val name = perm.substringAfterLast('.')
            when (status) {
                PackageManager.PERMISSION_GRANTED -> report("perm.$name", "granted")
                PackageManager.PERMISSION_DENIED  -> { report("perm.$name", "denied"); permDenied++ }
                else -> { report("perm.$name", "unknown($status)"); permDenied++ }
            }
        }
        report("perm.summary", "denied=$permDenied")

        // ═══════════════════════════════════════════════════════════════
        // Guest boot
        // ═══════════════════════════════════════════════════════════════
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

        // ═══════════════════════════════════════════════════════════════
        // Pre-warm Settings.Global (Wall C2b protection)
        // ═══════════════════════════════════════════════════════════════
        val settingsNames = listOf(
            "window_animation_scale",
            "transition_animation_scale",
            "animator_duration_scale"
        )
        var preWarmOk = 0
        var preWarmFail = 0
        for (name in settingsNames) {
            try {
                val v = Settings.Global.getInt(ctx.contentResolver, name, -1)
                report("preWarm.$name", "ok=$v")
                preWarmOk++
            } catch (t: Throwable) {
                val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                report("preWarm.$name.threw", "${cause.javaClass.name}: ${cause.message}")
                preWarmFail++
            }
        }
        report("preWarm.summary", "ok=$preWarmOk fail=$preWarmFail")
        val canLaunchHost = (preWarmFail == 0)

        // ═══════════════════════════════════════════════════════════════
        // Launcher Activity from archive
        // ═══════════════════════════════════════════════════════════════
        val pm = ctx.packageManager
        val flags = PackageManager.GET_ACTIVITIES or PackageManager.GET_META_DATA
        val archive = pm.getPackageArchiveInfo(apk.absolutePath, flags)
        if (archive == null) { report("archive", "null"); finish(outcomes); return }

        val guestPkg = archive.packageName
        report("guestPkg", guestPkg)
        val activities = archive.activities?.filterNotNull() ?: emptyList()
        report("activityCount", activities.size)

        val launcher = activities.firstOrNull { ai ->
            ai.name?.let { n ->
                n.contains("MainActivity") || n.contains("SplashActivity") || n.contains("Launcher")
            } == true
        } ?: activities.firstOrNull()
        if (launcher == null) { report("launcher", "none"); finish(outcomes); return }
        report("launcher", launcher.name)

        val guestAppInfo = archive.applicationInfo?.apply {
            sourceDir = apk.absolutePath
            publicSourceDir = apk.absolutePath
        }
        launcher.applicationInfo = guestAppInfo

        // ═══════════════════════════════════════════════════════════════
        // Guest classloader + Activity class
        // ═══════════════════════════════════════════════════════════════
        val guestCL = guestCtx.classLoader
        report("guestCL", guestCL?.javaClass?.name)
        val activityClass = try {
            guestCL.loadClass(launcher.name)
        } catch (t: Throwable) {
            report("loadClass.threw", "${t.javaClass.simpleName}: ${t.message}")
            finish(outcomes); return
        }
        report("activityClass", activityClass.name)

        // ═══════════════════════════════════════════════════════════════
        // ActivityThread + mInstrumentation
        // ═══════════════════════════════════════════════════════════════
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

        // ═══════════════════════════════════════════════════════════════
        // newActivity method
        // ═══════════════════════════════════════════════════════════════
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

        // ═══════════════════════════════════════════════════════════════
        // Host Activity launch (token source) — guarded by preWarm
        // ═══════════════════════════════════════════════════════════════
        var hostToken: IBinder? = null
        var hostTokenSource = "<skipped>"

        if (canLaunchHost) {
            report("hostLaunch.attempt", "true (pre-warm OK)")
            val mainIntent = Intent(ctx, MainActivity::class.java).apply {
                addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TOP)
            }
            var startSent = false
            try {
                ctx.startActivity(mainIntent)
                startSent = true
                report("mainIntent.sent", "true")
            } catch (t: Throwable) {
                report("mainIntent.threw", "${t.javaClass.name}: ${t.message}")
            }

            if (startSent) {
                val mActivitiesField = HiddenApiBridge.reflectGetDeclaredField(atCls, "mActivities")
                if (mActivitiesField == null) {
                    report("mActivities.field", "not found")
                } else {
                    HiddenApiBridge.reflectSetAccessible(mActivitiesField)
                    val deadline = System.currentTimeMillis() + 10_000L
                    var lastSize = -1
                    while (System.currentTimeMillis() < deadline && hostToken == null) {
                        try {
                            val map = mActivitiesField.get(at)
                            if (map is Map<*, *>) {
                                if (map.size != lastSize) {
                                    report("mActivities.size", map.size)
                                    lastSize = map.size
                                }
                                for (key in map.keys) {
                                    if (key is IBinder) {
                                        hostToken = key
                                        hostTokenSource = "ActivityThread.mActivities.key"
                                        break
                                    }
                                }
                            }
                        } catch (_: Throwable) {}
                        if (hostToken == null) Thread.sleep(50)
                    }
                }
            }
        } else {
            report("hostLaunch.attempt", "false (pre-warm failed — Wall C2b not fixed)")
            report("verdict.prelaunch", "Wall C4 = AMS registration required (in-process token source unavailable)")
        }

        report("hostToken.source", hostTokenSource)
        report("hostToken", hostToken)
        report("hostToken.hashCode", hostToken?.hashCode())
        report("hostToken.class", hostToken?.javaClass?.name)

        // ═══════════════════════════════════════════════════════════════
        // MAIN THREAD — guest newActivity + lifecycle + token steal + addView
        // ═══════════════════════════════════════════════════════════════
        val latch = CountDownLatch(1)
        Handler(Looper.getMainLooper()).post {
            try {
                report("mainThread.name", Thread.currentThread().name)
                report("mainThread.hasLooper", Looper.myLooper() != null)

                val activity = try {
                    newActivityM.invoke(
                        instrumentation,
                        activityClass, guestCtx, null, guestApp, intent,
                        launcher, launcher.name ?: "title",
                        null, null, null
                    ) as? Activity
                } catch (t: Throwable) {
                    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                    report("newActivity.threw", "${cause.javaClass.name}: ${cause.message}")
                    report("newActivity.stack", cause.stackTraceToString().lines().take(25).joinToString(" | "))
                    return@post
                }

                report("newActivity.result", activity?.javaClass?.name)
                if (activity == null) {
                    report("newActivity.null", "returned null without exception")
                    return@post
                }

                report("preAttach.mBase", activity.baseContext?.javaClass?.name)
                report("preAttach.mApplication", activity.application?.javaClass?.name)
                report("preAttach.mWindow", activity.window?.javaClass?.name)

                val onCreateM = findMethod(activityClass, "onCreate", Bundle::class.java)
                if (onCreateM == null) {
                    report("onCreate.method", "not found")
                } else {
                    try {
                        onCreateM.invoke(activity, null as Bundle?)
                        report("onCreate.result", "success")
                    } catch (t: Throwable) {
                        val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                        report("onCreate.threw", "${cause.javaClass.name}: ${cause.message}")
                        report("onCreate.stack", cause.stackTraceToString().lines().take(30).joinToString(" | "))
                        return@post
                    }
                }

                val onStartM = findMethod(activityClass, "onStart")
                onStartM?.let {
                    try {
                        it.invoke(activity)
                        report("onStart.result", "success")
                    } catch (t: Throwable) {
                        val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                        report("onStart.threw", "${cause.javaClass.name}: ${cause.message}")
                        report("onStart.stack", cause.stackTraceToString().lines().take(25).joinToString(" | "))
                        return@post
                    }
                }

                val onResumeM = findMethod(activityClass, "onResume")
                onResumeM?.let {
                    try {
                        it.invoke(activity)
                        report("onResume.result", "success")
                    } catch (t: Throwable) {
                        val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                        report("onResume.threw", "${cause.javaClass.name}: ${cause.message}")
                        report("onResume.stack", cause.stackTraceToString().lines().take(25).joinToString(" | "))
                        return@post
                    }
                }

                report("lifecycle.result", "onCreate+onStart+onResume all succeeded")

                report("preSteal.mToken", try {
                    val f = HiddenApiBridge.reflectGetDeclaredField(Activity::class.java, "mToken")
                    HiddenApiBridge.reflectSetAccessible(f)
                    f?.get(activity)
                } catch (_: Throwable) { "unreadable" })

                report("preSteal.lp.token", try {
                    activity.window.attributes.token
                } catch (_: Throwable) { "unreadable" })

                if (hostToken != null) {
                    try {
                        val mTokenField = HiddenApiBridge.reflectGetDeclaredField(Activity::class.java, "mToken")
                        if (mTokenField != null) {
                            HiddenApiBridge.reflectSetAccessible(mTokenField)
                            mTokenField.set(activity, hostToken)
                            report("steal.mToken.result", "success")
                            report("steal.mToken.after", mTokenField.get(activity))
                        } else {
                            report("steal.mToken.field", "not found")
                        }
                    } catch (t: Throwable) {
                        report("steal.mToken.threw", "${t.javaClass.name}: ${t.message}")
                    }

                    try {
                        val lp = activity.window.attributes
                        lp.token = hostToken
                        report("steal.lp.token.result", "success")
                        report("steal.lp.token.after", lp.token?.hashCode())
                    } catch (t: Throwable) {
                        report("steal.lp.token.threw", "${t.javaClass.name}: ${t.message}")
                    }

                    try {
                        val win = activity.window
                        var cls: Class<*>? = win.javaClass
                        var f: java.lang.reflect.Field? = null
                        while (cls != null && f == null) {
                            try {
                                f = cls.getDeclaredField("mAppToken")
                            } catch (_: NoSuchFieldException) {
                                cls = cls.superclass
                            }
                        }
                        if (f != null) {
                            f.isAccessible = true
                            val before = f.get(win)
                            f.set(win, hostToken)
                            report("steal.mAppToken.before", before)
                            report("steal.mAppToken.result", "success")
                        } else {
                            report("steal.mAppToken.field", "not found")
                        }
                    } catch (t: Throwable) {
                        report("steal.mAppToken.threw", "${t.javaClass.name}: ${t.message}")
                    }
                } else {
                    report("steal.skipped", "hostToken null — WMS will reject with BadTokenException")
                }

                report("postSteal.mToken", try {
                    val f = HiddenApiBridge.reflectGetDeclaredField(Activity::class.java, "mToken")
                    HiddenApiBridge.reflectSetAccessible(f)
                    f?.get(activity)
                } catch (_: Throwable) { "unreadable" })

                report("postSteal.lp.token", try {
                    activity.window.attributes.token
                } catch (_: Throwable) { "unreadable" })

                report("postSteal.lp.token.hashCode", try {
                    activity.window.attributes.token?.hashCode()
                } catch (_: Throwable) { -1 })

                var wmOk = false
                var decorOk = false
                var addViewOk = false
                try {
                    val wm = activity.window.windowManager
                    report("wallC4.wm", wm.javaClass.name)
                    wmOk = true

                    val decor = activity.window.decorView
                    report("wallC4.decorView", decor.javaClass.name)
                    decorOk = true

                    val lp = activity.window.attributes
                    report("wallC4.lp", lp?.javaClass?.name)
                    report("wallC4.lp.type", lp?.type)
                    report("wallC4.lp.token", lp?.token)
                    report("wallC4.lp.token.class", lp?.token?.javaClass?.name)
                    report("wallC4.lp.token.hashCode", lp?.token?.hashCode())
                    report("wallC4.lp.packageName", lp?.packageName)
                    report("wallC4.lp.flags", lp?.flags)
                    report("wallC4.lp.format", lp?.format)
                    report("wallC4.lp.width", lp?.width)
                    report("wallC4.lp.height", lp?.height)

                    wm.addView(decor, lp)
                    addViewOk = true
                    report("wallC4.addView.result", "success")
                    report("wallC4.decorView.isAttachedToWindow", decor.isAttachedToWindow)
                    report("wallC4.decorView.width", decor.width)
                    report("wallC4.decorView.height", decor.height)
                    report("wallC4.decorView.visibility", decor.visibility)
                    report("wallC4.decorView.windowToken", decor.windowToken)
                } catch (t: Throwable) {
                    val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                    report("wallC4.wmOk", wmOk)
                    report("wallC4.decorOk", decorOk)
                    report("wallC4.addViewOk", addViewOk)
                    report("wallC4.threw", "${cause.javaClass.name}: ${cause.message}")
                    report("wallC4.stack", cause.stackTraceToString().lines().take(30).joinToString(" | "))
                }

                report("note", "token-steal v5 complete")
            } catch (t: Throwable) {
                report("uncaught", "${t.javaClass.name}: ${t.message}")
                report("uncaught.stack", t.stackTraceToString().lines().take(25).joinToString(" | "))
            } finally {
                latch.countDown()
            }
        }

        val completed = try {
            latch.await(90, TimeUnit.SECONDS)
        } catch (_: InterruptedException) { false }

        if (!completed) report("mainThread.timeout", "90s — main thread dead or blocked")
        finish(outcomes)
    }

    private fun finish(outcomes: List<String>) {
        val f = File(ctx.filesDir, "activity-boot.txt")
        f.writeText(outcomes.joinToString("\n"))
        Log.d(tag, "activity-boot.txt written: ${f.absolutePath} (${outcomes.size} lines)")
        assertTrue("probe ran and produced outcomes", outcomes.isNotEmpty())
    }
}