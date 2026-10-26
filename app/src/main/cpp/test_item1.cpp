#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <string>
#include <sstream>
#include <vector>
#include "HookRegistry.h"
#include "TestReport.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);

static const int SENTINEL = 0xA17E;

extern "C" __attribute__((used, visibility("hidden")))
int g_adrp_value = 42;

extern "C" __attribute__((noinline, used, visibility("hidden")))
int fn_b_dest(int x) { return x + 100; }

extern "C" __attribute__((noinline, used, visibility("hidden")))
int fn_bl_helper(int x) { return x * 3; }

extern "C" __attribute__((noinline, naked, used))
int fn_normal(int x) {
    __asm__ volatile(
            "add w0, w0, #1\n"
            "ret\n"
            );
}

extern "C" __attribute__((noinline, naked, used))
int fn_adrp() {
    __asm__ volatile(
            "adrp x1, g_adrp_value\n"
            "add  x1, x1, :lo12:g_adrp_value\n"
            "ldr  w0, [x1]\n"
            "ret\n"
            );
}

extern "C" __attribute__((noinline, naked, used))
int fn_b(int x) {
    __asm__ volatile("b fn_b_dest\n");
}

// BL as the first instruction cannot be *called* cleanly — `bl` overwrites LR
// with fn_bl+4, and the only return path is `ret` at fn_bl+4, which jumps back
// to itself. Real code never starts a function with `bl`; thunks use `b`.
// We test BL structurally (install/verify/uninstall) without invoking it.
extern "C" __attribute__((noinline, naked, used))
int fn_bl(int x) {
    __asm__ volatile(
            "bl fn_bl_helper\n"
            "ret\n"
            );
}

extern "C" __attribute__((noinline, naked, used))
int fn_bcond() {
    __asm__ volatile(
            "b.eq 1f\n"
            "mov w0, #1\n"
            "ret\n"
            "1: mov w0, #2\n"
            "ret\n"
            );
}

extern "C" __attribute__((noinline, naked, used))
int fn_ldr_lit() {
    __asm__ volatile(
            "ldr w0, 1f\n"
            "ret\n"
            "1: .word 99\n"
            );
}

extern "C" __attribute__((noinline, used))
int fake_int_int(int x) { (void)x; return SENTINEL; }

extern "C" __attribute__((noinline, used))
int fake_void_int() { return SENTINEL; }

struct TestResult { std::string name; bool pass; std::string detail; };

template<typename Fn, typename Fake, typename... CallArgs>
static TestResult test_supported(const char* name, Fn fn, Fake fake, CallArgs... args) {
    TestResult r{name, false, ""};
    std::ostringstream d;

    int orig = fn(args...);
    d << "orig=" << orig;

    Fn tramp = nullptr;
    HookResult hr = InstallInlineHookEx((void*)fn, (void*)fake,
                                        (void**)&tramp, name);
    d << " install=" << (hr == HOOK_OK ? "OK" : "FAILED");
    if (hr != HOOK_OK) { r.detail = d.str(); return r; }

    int hooked = fn(args...);
    d << " hooked=" << hooked << "(want " << SENTINEL << ")";

    int via_tramp = tramp ? tramp(args...) : -1;
    d << " tramp=" << via_tramp << "(want " << orig << ")";

    HookResult ur = UninstallInlineHook((void*)fn);
    d << " unhook=" << (int)ur;

    int restored = fn(args...);
    d << " restored=" << restored << "(want " << orig << ")";

    r.pass = (hooked == SENTINEL)
             && (via_tramp == orig)
             && (ur == HOOK_OK)
             && (restored == orig);
    r.detail = d.str();
    return r;
}

static TestResult test_bl_structural() {
    TestResult r{"BL_structural", false, ""};
    std::ostringstream d;

    uint32_t before = *(volatile uint32_t*)&fn_bl;
    void* tramp = nullptr;
    HookResult hr = InstallInlineHookEx((void*)&fn_bl, (void*)&fake_int_int,
                                        &tramp, "BL");
    uint32_t after = *(volatile uint32_t*)&fn_bl;

    d << "before=0x" << std::hex << before
      << " install=" << (hr == HOOK_OK ? "OK" : "FAILED")
      << " after=0x" << after;
    if (hr != HOOK_OK) { r.detail = d.str(); return r; }

    HookResult ur = UninstallInlineHook((void*)&fn_bl);
    uint32_t restored = *(volatile uint32_t*)&fn_bl;
    d << std::dec << " unhook=" << (int)ur
      << " restored=0x" << std::hex << restored;

    r.pass = (before != after) && (ur == HOOK_OK) && (restored == before);
    r.detail = d.str();
    return r;
}

// B_COND structural — fn_bcond's return depends on the ambient Z flag, which
// can't be controlled from C. Rather than fight that in a call-through test,
// verify the byte lifecycle: install rewrites the first instruction to a B,
// uninstall restores 0x54000060 exactly. Behavioral coverage (inverted-skip
// + B target) lives in RelocTest at the instruction level — 44 assertions
// across reloc_emit_bcond_two_insns and reloc_emit_bcond_ne_inverts_to_eq.
static TestResult test_bcond_structural() {
    TestResult r{"B_COND", false, ""};
    std::ostringstream d;

    uint32_t before = *(volatile uint32_t*)&fn_bcond;
    void* tramp = nullptr;
    HookResult hr = InstallInlineHookEx((void*)&fn_bcond, (void*)&fake_void_int,
                                        &tramp, "B_COND");
    uint32_t after = *(volatile uint32_t*)&fn_bcond;

    d << "before=0x" << std::hex << before
      << " install=" << (hr == HOOK_OK ? "OK" : "FAILED")
      << " after=0x" << after;
    if (hr != HOOK_OK) { r.detail = d.str(); return r; }

    HookResult ur = UninstallInlineHook((void*)&fn_bcond);
    uint32_t restored = *(volatile uint32_t*)&fn_bcond;
    d << std::dec << " unhook=" << (int)ur
      << " restored=0x" << std::hex << restored;

    r.pass = (before != after) && (ur == HOOK_OK) && (restored == before);
    r.detail = d.str();
    return r;
}

static int RunItem1Internal(std::string& out) {
    LOGD("=== Item 1 tests START ===");

    std::vector<TestResult> results;
    results.push_back(test_supported("NORMAL", &fn_normal, &fake_int_int, 10));
    results.push_back(test_supported("ADRP",   &fn_adrp,   &fake_void_int));
    results.push_back(test_supported("B",      &fn_b,      &fake_int_int, 5));
    results.push_back(test_bl_structural());
    results.push_back(test_bcond_structural());
    results.push_back(test_supported("LDR_LITERAL", &fn_ldr_lit, &fake_void_int));

    int pass = 0, fail = 0;
    std::ostringstream o;
    o << "=== Item 1 tests ===\n";
    for (auto& r : results) {
        std::string line = std::string(r.pass ? "[PASS] " : "[FAIL] ")
                           + r.name + ": " + r.detail;
        testreport::Report(r.name.c_str(), r.pass, r.detail);
        o << line << "\n";
        if (r.pass) pass++; else fail++;
    }
    o << "=== " << pass << " passed, " << fail << " failed ===";
    out = o.str();
    return pass;
}

extern "C" int RunItem1ForHarness() {
    std::string ignored;
    return RunItem1Internal(ignored);
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_RemoteService_runItem1Tests(JNIEnv* env, jobject) {
    std::string out;
    RunItem1Internal(out);
    LOGD("%s", out.c_str());
    return env->NewStringUTF(out.c_str());
}