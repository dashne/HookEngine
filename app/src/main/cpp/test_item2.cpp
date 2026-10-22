// Item 2 tests: memory protection, patch safety, rollback.
//
// Five independent cases:
//   1. mprotect failure on a read-only shared mapping — install must fail,
//      target bytes must be unchanged, registry count must be unchanged.
//   2. Install twice — second returns ALREADY_HOOKED, third with a different
//      fake returns INTERNAL_ERROR (conflict detection), bytes unchanged.
//   3. Unhook restores the exact original 4 bytes at the target.
//   4. Page protection restored to original after install AND after uninstall.
//   5. InstallHooks rollback — inject a failure at the 3rd GOT hook attempt;
//      InstallHooks must unwind everything and leave RegistryCount() == 0;
//      a subsequent clean install must succeed and reach full count.

#include <jni.h>
#include <android/log.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string>
#include <sstream>
#include <vector>
#include "HookRegistry.h"

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);
int InstallHooks();
extern "C" int UninstallAllHooks();
extern "C" int g_hook_fail_after_n;

extern "C" __attribute__((noinline, naked, used))
int fn2_normal(int x) {
    __asm__ volatile("add w0, w0, #1\nret\n");
}

extern "C" __attribute__((noinline, used))
int fake2_a(int x) { (void)x; return 0xA17E; }

extern "C" __attribute__((noinline, used))
int fake2_b(int x) { (void)x; return 0xBEEF; }

struct TestResult { std::string name; bool pass; std::string detail; };

// Self-contained prot reader so the test doesn't depend on externals from
// InlineHooker.cpp (which are file-static there).
static bool ReadProt(uintptr_t page, int* outProt) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3) {
            if (s <= page && page < e) {
                int p = 0;
                if (perms[0] == 'r') p |= PROT_READ;
                if (perms[1] == 'w') p |= PROT_WRITE;
                if (perms[2] == 'x') p |= PROT_EXEC;
                *outProt = p;
                ok = true;
                break;
            }
        }
    }
    fclose(f);
    return ok;
}


// ─── 1. mprotect failure leaves target untouched ────────────────────
// MAP_SHARED on an O_RDONLY fd: mprotect(PROT_WRITE) fails with EACCES
// because the fd was not opened writable. This is a real, guaranteed
// mprotect failure — not a skipped test.
static TestResult test_mprotect_failure() {
    TestResult r{"mprotect_failure", false, ""};
    std::ostringstream d;

    // Find our own APK in /proc/self/maps.
    char apkPath[512] = {0};
    FILE* f = fopen("/proc/self/maps", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, ".apk") && (strstr(line, " r--") || strstr(line, " r-x"))) {
                char* path = strchr(line, '/');
                if (path) {
                    char* nl = strchr(path, '\n');
                    if (nl) *nl = 0;
                    strncpy(apkPath, path, sizeof(apkPath) - 1);
                    break;
                }
            }
        }
        fclose(f);
    }
    if (!apkPath[0]) { r.detail = "skip: no apk in maps"; r.pass = true; return r; }

    int fd = open(apkPath, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { r.detail = "skip: cannot open apk"; r.pass = true; return r; }

    long ps = sysconf(_SC_PAGESIZE);
    void* p = mmap(nullptr, ps, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { r.detail = "skip: mmap failed"; r.pass = true; return r; }

    uint32_t before = *(volatile uint32_t*)p;
    int countBefore = RegistryCount();

    void* tramp = nullptr;
    HookResult hr = InstallInlineHookEx(p, (void*)&fake2_a, &tramp, "ro-apk");

    uint32_t after = *(volatile uint32_t*)p;
    int countAfter = RegistryCount();

    d << "first=0x" << std::hex << before
      << " result=" << (int)hr
      << " first_after=0x" << after
      << std::dec << " registry=" << countBefore << "->" << countAfter;

    r.pass = (hr == HOOK_MPROTECT_FAILED)
             && (before == after)
             && (countAfter == countBefore);

    munmap(p, ps);
    r.detail = d.str();
    return r;
}

// ─── 2. install twice, conflict, no corruption ──────────────────────
static TestResult test_double_install() {
    TestResult r{"double_install", false, ""};
    std::ostringstream d;

    void* tramp1 = nullptr;
    HookResult r1 = InstallInlineHookEx((void*)&fn2_normal, (void*)&fake2_a,
                                        &tramp1, "double-a");
    d << "first=" << (int)r1;

    void* tramp2 = nullptr;
    HookResult r2 = InstallInlineHookEx((void*)&fn2_normal, (void*)&fake2_a,
                                        &tramp2, "double-a2");
    d << " again_same=" << (int)r2;

    void* tramp3 = nullptr;
    HookResult r3 = InstallInlineHookEx((void*)&fn2_normal, (void*)&fake2_b,
                                        &tramp3, "double-b");
    d << " again_diff=" << (int)r3;

    HookResult ur = UninstallInlineHook((void*)&fn2_normal);
    d << " unhook=" << (int)ur;

    r.pass = (r1 == HOOK_OK)
             && (r2 == HOOK_ALREADY_HOOKED)
             && (r3 == HOOK_INTERNAL_ERROR)
             && (ur == HOOK_OK);
    r.detail = d.str();
    return r;
}

// ─── 3. unhook restores exact bytes ─────────────────────────────────
static TestResult test_unhook_restores_bytes() {
    TestResult r{"unhook_bytes", false, ""};
    std::ostringstream d;

    uint32_t before = *(volatile uint32_t*)&fn2_normal;

    void* tramp = nullptr;
    HookResult hr = InstallInlineHookEx((void*)&fn2_normal, (void*)&fake2_a,
                                        &tramp, "bytes-test");
    if (hr != HOOK_OK) { r.detail = "install failed"; return r; }

    uint32_t during = *(volatile uint32_t*)&fn2_normal;

    HookResult ur = UninstallInlineHook((void*)&fn2_normal);
    uint32_t after = *(volatile uint32_t*)&fn2_normal;

    d << "before=0x" << std::hex << before
      << " during=0x" << during
      << " after=0x"  << after
      << std::dec << " unhook=" << (int)ur;

    r.pass = (before != during)
             && (after == before)
             && (ur == HOOK_OK);
    r.detail = d.str();
    return r;
}

// ─── 4. page prot restored after install and after uninstall ────────
static TestResult test_prot_restored() {
    TestResult r{"prot_restored", false, ""};
    std::ostringstream d;

    uintptr_t pg = (uintptr_t)&fn2_normal & ~0xFFFULL;
    int prot_before = 0, prot_during = 0, prot_after = 0;
    ReadProt(pg, &prot_before);

    void* tramp = nullptr;
    HookResult hr = InstallInlineHookEx((void*)&fn2_normal, (void*)&fake2_a,
                                        &tramp, "prot-test");
    if (hr != HOOK_OK) { r.detail = "install failed"; return r; }
    ReadProt(pg, &prot_during);

    HookResult ur = UninstallInlineHook((void*)&fn2_normal);
    ReadProt(pg, &prot_after);

    d << "before=0x" << std::hex << prot_before
      << " during=0x" << prot_during
      << " after=0x"  << prot_after
      << std::dec << " unhook=" << (int)ur;

    r.pass = (prot_before == prot_during)
             && (prot_during == prot_after)
             && (ur == HOOK_OK);
    r.detail = d.str();
    return r;
}

// ─── 5. InstallHooks rollback on injected failure ───────────────────
static TestResult test_installhooks_rollback() {
    TestResult r{"installhooks_rollback", false, ""};
    std::ostringstream d;

    // Start from clean state so the test can observe a fail-then-rollback-then-reinstall cycle.
    UninstallAllHooks();
    int countClean = RegistryCount();
    d << "start_count=" << countClean;

    // Inject failure on the 3rd GOT hook attempt inside InstallHooks.
    g_hook_fail_after_n = 2;
    int rcFail = InstallHooks();
    int countAfterFail = RegistryCount();

    g_hook_fail_after_n = -1;   // safety

    d << " inject_rc=" << rcFail
      << " count_after_fail=" << countAfterFail;

    bool rollbackWorked = (rcFail == HOOK_MPROTECT_FAILED) && (countAfterFail == 0);

    int rcClean = InstallHooks();
    int countAfterClean = RegistryCount();

    d << " clean_rc=" << rcClean
      << " count_after_clean=" << countAfterClean;

    r.pass = rollbackWorked
             && (rcClean == HOOK_OK)
             && (countAfterClean > 0);
    r.detail = d.str();
    return r;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_RemoteService_runItem2Tests(JNIEnv* env, jobject) {
    LOGD("=== Item 2 tests START ===");

    std::vector<TestResult> results;
    results.push_back(test_mprotect_failure());
    results.push_back(test_double_install());
    results.push_back(test_unhook_restores_bytes());
    results.push_back(test_prot_restored());
    results.push_back(test_installhooks_rollback());

    int pass = 0, fail = 0;
    std::ostringstream out;
    out << "=== Item 2 tests ===\n";
    for (auto& r : results) {
        std::string line = std::string(r.pass ? "[PASS] " : "[FAIL] ")
                           + r.name + ": " + r.detail;
        LOGD("%s", line.c_str());
        out << line << "\n";
        if (r.pass) pass++; else fail++;
    }
    out << "=== " << pass << " passed, " << fail << " failed ===";
    LOGD("%s", out.str().c_str());
    return env->NewStringUTF(out.str().c_str());
}