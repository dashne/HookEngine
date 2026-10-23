#include <jni.h>
#include <android/log.h>
#include <string>
#include "AntiDetection.h"
#include "TestReport.h"

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

int InstallHooks();
extern "C" int UninstallAllHooks();
extern "C" int ShouldHidePathForTest(const char* path);

// From BuildFilteredStatus — expose via small helper
extern "C" int ShouldFilterStatusPath(const char* path);

extern "C" int RunAntiDetectionTests(JNIEnv* env) {
    int pass = 0, fail = 0;
    auto emit = [&](bool ok, const char* name, const char* detail) {
        testreport::Report(name, ok, detail);
        if (ok) pass++; else fail++;
    };

    // Install both the file-io hooks (status path) and anti-detection (connect)
    int rc_file = InstallHooks();
    int rc_anti = InstallAntiDetection(env);

    // 1. Both install paths succeeded
    {
        char d[128];
        snprintf(d, sizeof d, "installHooks=%d installAntiDetection=%d", rc_file, rc_anti);
        emit(rc_file == 0 && rc_anti >= 1, "anti_detection_installed", d);
    }

    // 2. Connect hook is installed (trampoline set)
    {
        int installed = IsConnectHookInstalled();
        char d[64];
        snprintf(d, sizeof d, "connect_hook_installed=%d", installed);
        emit(installed == 1, "connect_hook_installed", d);
    }

    // 3. ShouldBlockConnect returns true for Frida port 27042
    {
        int block = ShouldBlockConnectForTest("127.0.0.1", 27042);
        char d[96];
        snprintf(d, sizeof d, "block(127.0.0.1:27042)=%d", block);
        emit(block == 1, "frida_port_27042_logically_blocked", d);
    }

    // 4. ShouldBlockConnect returns true for Frida port 27043
    {
        int block = ShouldBlockConnectForTest("127.0.0.1", 27043);
        char d[96];
        snprintf(d, sizeof d, "block(127.0.0.1:27043)=%d", block);
        emit(block == 1, "frida_port_27043_logically_blocked", d);
    }

    // 5. ShouldBlockConnect returns true for IDA port 23946
    {
        int block = ShouldBlockConnectForTest("127.0.0.1", 23946);
        char d[96];
        snprintf(d, sizeof d, "block(127.0.0.1:23946)=%d", block);
        emit(block == 1, "ida_port_23946_logically_blocked", d);
    }

    // 6. ShouldBlockConnect returns false for a normal port (8080)
    {
        int block = ShouldBlockConnectForTest("127.0.0.1", 8080);
        char d[96];
        snprintf(d, sizeof d, "block(127.0.0.1:8080)=%d", block);
        emit(block == 0, "normal_port_8080_allowed", d);
    }

    // 7. ShouldBlockConnect returns false for a normal host (not loopback)
    {
        int block = ShouldBlockConnectForTest("8.8.8.8", 27042);
        char d[96];
        snprintf(d, sizeof d, "block(8.8.8.8:27042)=%d", block);
        emit(block == 0, "frida_port_non_loopback_allowed", d);
    }

    // 8. Magisk /sbin/.magisk logically hidden
    {
        int hide = ShouldHidePathForTest("/sbin/.magisk");
        char d[96];
        snprintf(d, sizeof d, "hide(/sbin/.magisk)=%d", hide);
        emit(hide == 1, "magisk_sbin_logically_hidden", d);
    }

    // 9. Magisk /data/adb/magisk logically hidden
    {
        int hide = ShouldHidePathForTest("/data/adb/magisk");
        char d[96];
        snprintf(d, sizeof d, "hide(/data/adb/magisk)=%d", hide);
        emit(hide == 1, "magisk_adb_logically_hidden", d);
    }

    // 10. Magisk /data/adb/modules logically hidden
    {
        int hide = ShouldHidePathForTest("/data/adb/modules");
        char d[96];
        snprintf(d, sizeof d, "hide(/data/adb/modules)=%d", hide);
        emit(hide == 1, "magisk_modules_logically_hidden", d);
    }

    // 11. /debug_ramdisk hidden
    {
        int hide = ShouldHidePathForTest("/debug_ramdisk");
        char d[96];
        snprintf(d, sizeof d, "hide(/debug_ramdisk)=%d", hide);
        emit(hide == 1, "debug_ramdisk_logically_hidden", d);
    }

    // ── Test: /debug_ramdisk/foo prefix matched ──
    {
        int hide = ShouldHidePathForTest("/debug_ramdisk/foo");
        emit(hide == 1, "debug_ramdisk_prefix_hidden",
             hide ? "hidden" : "VISIBLE (bug!)");
    }

    // ── Test: /debug_ramdiskXYZ boundary NOT matched ──
    {
        int hide = ShouldHidePathForTest("/debug_ramdiskXYZ");
        emit(hide == 0, "debug_ramdisk_boundary_rejected",
             hide ? "HIDDEN (boundary bug!)" : "visible (correct)");
    }

    // ── Test: /cache/magisk/foo prefix matched ──
    {
        int hide = ShouldHidePathForTest("/cache/magisk/foo");
        emit(hide == 1, "cache_magisk_prefix_hidden",
             hide ? "hidden" : "VISIBLE (bug!)");
    }

    // ── Test: /cache/magiskXYZ boundary NOT matched ──
    {
        int hide = ShouldHidePathForTest("/cache/magiskXYZ");
        emit(hide == 0, "cache_magisk_boundary_rejected",
             hide ? "HIDDEN (boundary bug!)" : "visible (correct)");
    }

    // 12. Normal path not hidden (no overreach)
    {
        int hide = ShouldHidePathForTest("/data/app/com.example/base.apk");
        char d[96];
        snprintf(d, sizeof d, "hide(/data/app/com.example/base.apk)=%d", hide);
        emit(hide == 0, "normal_path_not_hidden", d);
    }

    // 13. /proc/self/status is a filter target
    {
        int filter = ShouldFilterStatusPath("/proc/self/status");
        char d[96];
        snprintf(d, sizeof d, "filter(/proc/self/status)=%d", filter);
        emit(filter == 1, "status_path_registered", d);
    }

    // 14. /proc/self/maps is NOT a status target (maps handled separately)
    {
        int filter = ShouldFilterStatusPath("/proc/self/maps");
        char d[96];
        snprintf(d, sizeof d, "filter(/proc/self/maps)=%d", filter);
        emit(filter == 0, "maps_path_not_misrouted", d);
    }

    LOGD("RunAntiDetectionTests: %d passed, %d failed", pass, fail);
    return pass;
}