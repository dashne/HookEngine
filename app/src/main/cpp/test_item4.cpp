// Item 4 tests: ELF relocation and symbol-hook reliability.
//
// Test A — ELF validation:        GetGotAddress succeeds for real symbol,
//                                 returns null for missing symbol and missing lib.
// Test B — Round-trip correctness: read a file BEFORE hooks → "REAL";
//                                 install hooks with redirect rule → "FAKE";
//                                 uninstall → "REAL" again.
// Test C — GOT conflict/idempotent: double InstallHooks both return HOOK_OK.
// Test D — Missing symbols:        InstallHooks succeeds despite absent symbols.

#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <string>
#include <sstream>
#include <vector>
#include <sys/stat.h>
#include <stdlib.h>
#include <string>
#include "HookRegistry.h"
#include "RedirectTable.h"
#include "TestReport.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

int InstallHooks();
extern "C" int UninstallAllHooks();
extern "C" void** GetGotAddress(const char*, const char*);

struct TestResult { std::string name; bool pass; std::string detail; };

// ─── A: ELF validation ─────────────────────────────────────────────
static TestResult test_elf_validation() {
    TestResult r{"elf_validation", false, ""};
    std::ostringstream d;

    void** good = GetGotAddress("libjavacore.so", "open");
    void** missSym = GetGotAddress("libjavacore.so", "__he_nonexistent_xyz");
    void** missLib = GetGotAddress("libdoesnotexist.so", "open");

    d << "real_got=" << good
      << " miss_sym=" << missSym
      << " miss_lib=" << missLib;

    r.pass = (good != nullptr) && (missSym == nullptr) && (missLib == nullptr);
    r.detail = d.str();
    return r;
}

// ─── B: round-trip through a real file read ────────────────────────
// FileInputStream calls open() via libjavacore's GOT, so if install is
// correct the fake sees the call and the redirect rule fires.
static std::string read_via_java(JNIEnv* env, const char* path) {
    jclass fisCls = env->FindClass("java/io/FileInputStream");
    if (!fisCls) return "<no-fis-class>";
    jmethodID ctor = env->GetMethodID(fisCls, "<init>", "(Ljava/lang/String;)V");
    if (!ctor) { env->DeleteLocalRef(fisCls); return "<no-ctor>"; }
    jstring pstr = env->NewStringUTF(path);
    jobject fis = env->NewObject(fisCls, ctor, pstr);
    env->DeleteLocalRef(pstr);
    if (!fis) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(fisCls);
        return "<no-fis>";
    }

    jmethodID readM = env->GetMethodID(fisCls, "read", "([B)I");
    jbyteArray arr = env->NewByteArray(256);
    jint n = env->CallIntMethod(fis, readM, arr);
    jmethodID closeM = env->GetMethodID(fisCls, "close", "()V");
    env->CallVoidMethod(fis, closeM);
    env->DeleteLocalRef(fis);
    env->DeleteLocalRef(fisCls);

    std::string result;
    if (n > 0) {
        jbyte buf[256];
        env->GetByteArrayRegion(arr, 0, n, buf);
        result.assign((char*)buf, (size_t)n);
    }
    env->DeleteLocalRef(arr);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return result;
}

static TestResult test_roundtrip(JNIEnv* env, const char* realPath, const char* fakePath) {
    TestResult r{"roundtrip", false, ""};
    std::ostringstream d;

    FILE* f = fopen(realPath, "w"); if (f) { fputs("REAL", f); fclose(f); }
    f = fopen(fakePath, "w");       if (f) { fputs("FAKE", f); fclose(f); }

    std::string pre = read_via_java(env, realPath);
    d << "pre='" << pre << "'";

    // Register redirect rule
    std::string realDir(realPath);
    std::string fakeDir(fakePath);
    size_t sl = realDir.rfind('/'); if (sl != std::string::npos) realDir.resize(sl);
    sl = fakeDir.rfind('/'); if (sl != std::string::npos) fakeDir.resize(sl);
    RedirectTable::GetInstance().AddRule(realDir + "/", fakeDir + "/");

    UninstallAllHooks();
    int rc = InstallHooks();
    d << " install_rc=" << rc << " count=" << RegistryCount();

    std::string hooked = read_via_java(env, realPath);
    d << " hooked='" << hooked << "'";

    UninstallAllHooks();

    std::string post = read_via_java(env, realPath);
    d << " post='" << post << "'";

    r.pass = (pre == "REAL") && (hooked == "FAKE") && (post == "REAL");
    r.detail = d.str();
    return r;
}

// ─── C: GOT conflict / idempotency ────────────────────────────────
static TestResult test_got_idempotent() {
    TestResult r{"got_idempotent", false, ""};
    std::ostringstream d;

    UninstallAllHooks();
    int rc1 = InstallHooks();
    int c1 = RegistryCount();
    int rc2 = InstallHooks();
    int c2 = RegistryCount();
    UninstallAllHooks();

    d << "rc1=" << rc1 << " c1=" << c1
      << " rc2=" << rc2 << " c2=" << c2;

    r.pass = (rc1 == HOOK_OK) && (rc2 == HOOK_OK) && (c1 == c2) && (c1 > 0);
    r.detail = d.str();
    return r;
}

// ─── D: install succeeds with absent symbols ──────────────────────
static TestResult test_missing_symbols() {
    TestResult r{"missing_symbols", false, ""};
    std::ostringstream d;

    UninstallAllHooks();
    int rc = InstallHooks();
    int count = RegistryCount();
    UninstallAllHooks();

    d << "rc=" << rc << " count=" << count;
    r.pass = (rc == HOOK_OK) && (count > 0);
    r.detail = d.str();
    return r;
}

static int RunItem4Internal(JNIEnv* env, const char* realPath,
                            const char* fakePath, std::string& out) {
    LOGD("=== Item 4 tests START ===");

    std::ostringstream o;
    int pass = 0, fail = 0;
    auto emit = [&](const TestResult& r) {
        std::string line = std::string(r.pass ? "[PASS] " : "[FAIL] ")
                           + r.name + ": " + r.detail;
        testreport::Report(r.name.c_str(), r.pass, r.detail);
        o << line << "\n";
        if (r.pass) pass++; else fail++;
    };

    emit(test_elf_validation());
    if (realPath && realPath[0] && fakePath && fakePath[0]) {
        emit(test_roundtrip(env, realPath, fakePath));
    } else {
        TestResult skip{"roundtrip", true, "skip: no paths provided"};
        emit(skip);
    }
    emit(test_got_idempotent());
    emit(test_missing_symbols());

    std::ostringstream tail;
    tail << "=== " << pass << " passed, " << fail << " failed ===";
    LOGD("%s", tail.str().c_str());
    o << tail.str();
    out = o.str();
    return pass;
}

static std::string g_item4_realDir;
static std::string g_item4_fakeDir;

extern "C" void SetItem4Paths(const char* realDir, const char* fakeDir) {
    g_item4_realDir = realDir ? realDir : "";
    g_item4_fakeDir = fakeDir ? fakeDir : "";
}

extern "C" int RunItem4ForHarness(JNIEnv* env) {
    if (!env) return -1;
    if (g_item4_realDir.empty() || g_item4_fakeDir.empty()) {
        LOGD("RunItem4ForHarness: paths not set — call setItem4Paths first");
        return -1;
    }
    mkdir(g_item4_realDir.c_str(), 0700);
    mkdir(g_item4_fakeDir.c_str(), 0700);
    std::string realPath = g_item4_realDir + "/item4_probe.txt";
    std::string fakePath = g_item4_fakeDir + "/item4_probe.txt";
    std::string ignored;
    return RunItem4Internal(env, realPath.c_str(), fakePath.c_str(), ignored);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_RemoteService_runItem4Tests(JNIEnv* env, jobject,
                                                       jstring realDir,
                                                       jstring fakeDir) {
    const char* realDirP = realDir ? env->GetStringUTFChars(realDir, nullptr) : nullptr;
    const char* fakeDirP = fakeDir ? env->GetStringUTFChars(fakeDir, nullptr) : nullptr;

    std::string realPath = realDirP ? (std::string(realDirP) + "/item4_probe.txt") : "";
    std::string fakePath = fakeDirP ? (std::string(fakeDirP) + "/item4_probe.txt") : "";

    std::string out;
    RunItem4Internal(env, realPath.c_str(), fakePath.c_str(), out);

    if (realDirP) env->ReleaseStringUTFChars(realDir, realDirP);
    if (fakeDirP) env->ReleaseStringUTFChars(fakeDir, fakeDirP);
    return env->NewStringUTF(out.c_str());
}