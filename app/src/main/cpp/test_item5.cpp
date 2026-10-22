// Item 5 tests — ART compatibility gate.
//
// The gate now only accepts VERIFIED API 37. All other API levels are
// refused with HOOK_UNSUPPORTED_API. These tests verify:
//
//   A. Real API is 37 and CheckHookCapability() == HOOK_OK
//   B. Forced API 33, 34, 35, 36, 38 → HOOK_UNSUPPORTED_API
//   C. Forced API 37 → HOOK_OK
//   D. Forced API 33 + attempt to hook Binder → UNSUPPORTED_API,
//      target bytes untouched, Binder.getCallingUid() returns real UID
//   E. Real API 37 + attempt to hook Binder → HOOK_OK, fake fires,
//      restore returns real UID
//
// Test B is the critical honesty check: the gate must refuse every
// API level we have not verified, including 34–36 which appear in
// earlier planning documents as aspirational.

#include <jni.h>
#include <android/log.h>
#include <android/api-level.h>
#include <stdint.h>
#include <string>
#include <sstream>
#include <vector>
#include "HookRegistry.h"
#include "TestReport.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" jint CheckHookCapability();
extern "C" int  g_force_api_override;

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_RemoteService_hookBinderMethod(JNIEnv*, jobject, jobject);
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_restoreBinderMethod(JNIEnv*, jobject);
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_RemoteService_setVirtualUid(JNIEnv*, jobject, jint);

struct TestResult { std::string name; bool pass; std::string detail; };

static int32_t get_calling_uid_via_binder(JNIEnv* env) {
    jclass binderCls = env->FindClass("android/os/Binder");
    if (!binderCls) return -1;
    jmethodID m = env->GetStaticMethodID(binderCls, "getCallingUid", "()I");
    if (!m) { env->DeleteLocalRef(binderCls); return -1; }
    jint uid = env->CallStaticIntMethod(binderCls, m);
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->DeleteLocalRef(binderCls);
    return uid;
}

static TestResult test_real_api() {
    TestResult r{"real_api", false, ""};
    std::ostringstream d;
    int real = android_get_device_api_level();
    jint cap = CheckHookCapability();
    d << "real_api=" << real << " capability=" << (int)cap;
    r.pass = (real == 37) && (cap == HOOK_OK);
    r.detail = d.str();
    return r;
}

// Every API level except 37 must be refused, including 34–36.
static TestResult test_gate_refuses_unverified() {
    TestResult r{"gate_refuses_unverified", false, ""};
    std::ostringstream d;
    const int levels[] = { 33, 34, 35, 36, 38 };
    bool all_refused = true;
    for (int i = 0; i < 5; i++) {
        g_force_api_override = levels[i];
        jint cap = CheckHookCapability();
        d << "api" << levels[i] << "=" << (int)cap << " ";
        if (cap != HOOK_UNSUPPORTED_API) all_refused = false;
    }
    g_force_api_override = -1;
    r.pass = all_refused;
    r.detail = d.str();
    return r;
}

static TestResult test_gate_allows_verified() {
    TestResult r{"gate_allows_verified", false, ""};
    std::ostringstream d;
    g_force_api_override = 37;
    jint cap37 = CheckHookCapability();
    g_force_api_override = -1;
    d << "api37=" << (int)cap37;
    r.pass = (cap37 == HOOK_OK);
    r.detail = d.str();
    return r;
}

static TestResult test_hook_refused_when_gate_blocks(
        JNIEnv* env, jobject service, jobject method) {
    TestResult r{"hook_refused_when_gate_blocks", false, ""};
    std::ostringstream d;

    Java_com_ims134_hookengine_RemoteService_setVirtualUid(env, service, 10000);

    int32_t before = get_calling_uid_via_binder(env);
    d << "before=" << before;

    g_force_api_override = 33;
    jint rc = Java_com_ims134_hookengine_RemoteService_hookBinderMethod(
            env, service, method);
    g_force_api_override = -1;
    d << " rc_refused=" << (int)rc << "(want " << (int)HOOK_UNSUPPORTED_API << ")";

    int32_t after = get_calling_uid_via_binder(env);
    d << " after=" << after << "(want " << before << ")";

    r.pass = (rc == HOOK_UNSUPPORTED_API) && (after == before);
    r.detail = d.str();
    return r;
}

static TestResult test_hook_works_when_gate_allows(
        JNIEnv* env, jobject service, jobject method) {
    TestResult r{"hook_works_when_gate_allows", false, ""};
    std::ostringstream d;

    Java_com_ims134_hookengine_RemoteService_setVirtualUid(env, service, 10000);

    int32_t real_uid = get_calling_uid_via_binder(env);
    d << "real=" << real_uid;

    jint rc = Java_com_ims134_hookengine_RemoteService_hookBinderMethod(
            env, service, method);
    d << " install_rc=" << (int)rc;

    int32_t hooked_uid = get_calling_uid_via_binder(env);
    d << " hooked=" << hooked_uid << "(want 10000)";

    Java_com_ims134_hookengine_RemoteService_restoreBinderMethod(env, service);

    int32_t restored_uid = get_calling_uid_via_binder(env);
    d << " restored=" << restored_uid << "(want " << real_uid << ")";

    r.pass = (rc == HOOK_OK)
             && (hooked_uid == 10000)
             && (restored_uid == real_uid);
    r.detail = d.str();
    return r;
}

// service is unused by every callee (they all use the receiver-less internal
// signatures); passing nullptr from the harness is safe.
static int RunItem5Internal(JNIEnv* env, jobject service, jobject method,
                            std::string& out) {
    LOGD("=== Item 5 tests START ===");

    std::vector<TestResult> results;
    results.push_back(test_real_api());
    results.push_back(test_gate_refuses_unverified());
    results.push_back(test_gate_allows_verified());
    results.push_back(test_hook_refused_when_gate_blocks(env, service, method));
    results.push_back(test_hook_works_when_gate_allows(env, service, method));

    int pass = 0, fail = 0;
    std::ostringstream o;
    for (auto& r : results) {
        std::string line = std::string(r.pass ? "[PASS] " : "[FAIL] ")
                           + r.name + ": " + r.detail;
        testreport::Report(r.name.c_str(), r.pass, r.detail);
        o << line << "\n";
        if (r.pass) pass++; else fail++;
    }
    std::ostringstream tail;
    tail << "=== " << pass << " passed, " << fail << " failed ===";
    LOGD("%s", tail.str().c_str());
    o << tail.str();
    out = o.str();
    return pass;
}

extern "C" int RunItem5ForHarness(JNIEnv* env) {
    if (!env) return -1;
    jclass binderCls = env->FindClass("android/os/Binder");
    if (!binderCls) return -1;
    jmethodID m = env->GetStaticMethodID(binderCls, "getCallingUid", "()I");
    if (!m) { env->DeleteLocalRef(binderCls); return -1; }
    jobject method = env->ToReflectedMethod(binderCls, m, JNI_TRUE);
    if (!method) { env->DeleteLocalRef(binderCls); return -1; }
    std::string ignored;
    int pass = RunItem5Internal(env, nullptr, method, ignored);
    env->DeleteLocalRef(method);
    env->DeleteLocalRef(binderCls);
    return pass;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_RemoteService_runItem5Tests(
        JNIEnv* env, jobject service, jobject getCallingUidMethod) {
    std::string out;
    RunItem5Internal(env, service, getCallingUidMethod, out);
    return env->NewStringUTF(out.c_str());
}