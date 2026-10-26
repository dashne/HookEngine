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
 * Session 26 — Milestone 4 second-wall probe.
 *
 * Fixes Wall C1 (Session 25): Activity.<init> line 1018 calls new Handler(),
 * which needs Looper.myLooper() on the caller thread. The androidx test
 * runner thread (ThreadGroup "main") is NOT the Android main thread — it
 * has no Looper. Post to Looper.getMainLooper() (ActivityThread's real main
 * looper) which is the same thread ActivityThread.performLaunchActivity uses.
 * Posting to main is correct usage, not a workaround.
 *
 * Forbidden: Looper.prepare() on the test thread. Would create a Looper
 * nobody drains — a fake fix that hides the constraint.
 *
 * Extends probe to also call Activity.attach (required by onCreate) and
 * then onCreate / onStart / onResume. Whichever fires first is Wall C2.
 *
 * Rule 12: every step reports to activity-boot.txt + logcat tag
 * "ActivityBoot". Only assertion is that the probe produced outcomes.
 */
@RunWith(AndroidJUnit4::class)
class ActivityBootProbeTest {

    private val ctx get() = InstrumentationRegistry.getInstrumentation().targetContext
    private val tag = "ActivityBoot"

    /** Hierarchy-aware method lookup — needed for protected onCreate/onStart/onResume. */
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

        // ── 0. Guest boot (safe on test thread) ──
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

        // ── 1. Launcher Activity ──
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

        // ── 2. Guest classloader + Activity class ──
        val guestCL = guestCtx.classLoader
        report("guestCL", guestCL?.javaClass?.name)
        val activityClass = try {
            guestCL.loadClass(launcher.name)
        } catch (t: Throwable) {
            report("loadClass.threw", "${t.javaClass.simpleName}: ${t.message}")
            finish(outcomes); return
        }
        report("activityClass", activityClass.name)

        // ── 3. ActivityThread + mInstrumentation ──
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

        // ── 4. newActivity method ──
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

        // ── 5. Intent ──
        val intent = Intent(Intent.ACTION_MAIN).apply {
            addCategory(Intent.CATEGORY_LAUNCHER)
            component = ComponentName(guestPkg, launcher.name)
        }

        // ── 6. Post ALL framework work to main looper (Wall C1 fix) ──
        val latch = CountDownLatch(1)
        Handler(Looper.getMainLooper()).post {
            try {
                report("mainThread.name", Thread.currentThread().name)
                report("mainThread.hasLooper", Looper.myLooper() != null)

                // ── 6a. newActivity (Wall C1 retry) ──
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

                // ── 6b. Activity state BEFORE attach (expected null — newActivity doesn't attach) ──
                report("preAttach.mBase", try { activity.baseContext?.javaClass?.name } catch (t: Throwable) { "threw ${t.javaClass.simpleName}" })
                report("preAttach.mApplication", try { activity.application?.javaClass?.name } catch (t: Throwable) { "threw ${t.javaClass.simpleName}" })
                report("preAttach.mWindow", try { activity.window?.javaClass?.name } catch (t: Throwable) { "threw ${t.javaClass.simpleName}" })

                // ── 6c. Activity state AFTER newActivity ──
                // newActivity calls Activity.attach internally (Session 26
                // stack trace: Instrumentation.newActivity(1446) →
                // Activity.attach(9482)). No explicit attach needed.
                report("postAttach.mBase", try { activity.baseContext?.javaClass?.name } catch (t: Throwable) { "threw ${t.javaClass.simpleName}" })
                report("postAttach.mApplication", try { activity.application?.javaClass?.name } catch (t: Throwable) { "threw ${t.javaClass.simpleName}" })
                report("postAttach.mWindow", try { activity.window?.javaClass?.name } catch (t: Throwable) { "threw ${t.javaClass.simpleName}" })

                // ── 6e. onCreate — Wall C2 probe ──
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

                // ── 6f. onStart ──
                val onStartM = findMethod(activityClass, "onStart")
                if (onStartM != null) {
                    try {
                        onStartM.invoke(activity)
                        report("onStart.result", "success")
                    } catch (t: Throwable) {
                        val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                        report("onStart.threw", "${cause.javaClass.name}: ${cause.message}")
                        report("onStart.stack", cause.stackTraceToString().lines().take(25).joinToString(" | "))
                        return@post
                    }
                } else {
                    report("onStart.method", "not found")
                }

                // ── 6g. onResume ──
                val onResumeM = findMethod(activityClass, "onResume")
                if (onResumeM != null) {
                    try {
                        onResumeM.invoke(activity)
                        report("onResume.result", "success")
                    } catch (t: Throwable) {
                        val cause = (t as? java.lang.reflect.InvocationTargetException)?.targetException ?: t
                        report("onResume.threw", "${cause.javaClass.name}: ${cause.message}")
                        report("onResume.stack", cause.stackTraceToString().lines().take(25).joinToString(" | "))
                        return@post
                    }
                } else {
                    report("onResume.method", "not found")
                }

                report("lifecycle.result", "onCreate+onStart+onResume all succeeded")
                report("note", "First wall is behind onResume — WindowManager.addView or AMS registration (Session 27 target)")
            } catch (t: Throwable) {
                report("uncaught", "${t.javaClass.name}: ${t.message}")
                report("uncaught.stack", t.stackTraceToString().lines().take(25).joinToString(" | "))
            } finally {
                latch.countDown()
            }
        }

        val completed = try {
            latch.await(60, TimeUnit.SECONDS)
        } catch (_: InterruptedException) { false }

        if (!completed) report("mainThread.timeout", "60s — probe hung")
        finish(outcomes)
    }

    private fun finish(outcomes: List<String>) {
        val f = File(ctx.filesDir, "activity-boot.txt")
        f.writeText(outcomes.joinToString("\n"))
        Log.d(tag, "activity-boot.txt written: ${f.absolutePath} (${outcomes.size} lines)")
        assertTrue("probe ran and produced outcomes", outcomes.isNotEmpty())
    }
}