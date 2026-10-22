#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>
#include <errno.h>

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define ARM64_BRANCH_OFFSET(offset) (0x14000000 | (((offset) >> 2) & 0x03FFFFFF))
#define ARM64_LDR_X17_8 0x58000051
#define ARM64_BR_X17    0xD61F0220

void* AllocateMemoryNear(void* target, size_t size) {
    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t targetAddr = (uintptr_t)target;
    const uintptr_t MAX_DIST = 120UL * 1024 * 1024;

    auto tryOffset = [&](uintptr_t off) -> void* {
        uintptr_t cand[2];
        int n = 0;
        if (targetAddr >= off)       cand[n++] = (targetAddr - off) & ~(pageSize - 1);
        if (targetAddr + off > off)  cand[n++] = (targetAddr + off) & ~(pageSize - 1);

        for (int k = 0; k < n; k++) {
            void* hint = (void*)cand[k];
            // RWX up front — caller writes the relay immediately
            void* m = mmap(hint, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
            if (m == MAP_FAILED) continue;
            if (m != hint) { munmap(m, size); continue; }
            uintptr_t got = (uintptr_t)m;
            if (got > targetAddr + MAX_DIST || targetAddr > got + MAX_DIST) {
                munmap(m, size); continue;
            }
            return m;
        }
        return nullptr;
    };

    for (int i = 1; i <= 1024; i++) {
        void* m = tryOffset((uintptr_t)i * pageSize);
        if (m) return m;
    }
    for (int i = 1; i < 120; i++) {
        void* m = tryOffset((uintptr_t)i * 1024 * 1024);
        if (m) return m;
    }
    return nullptr;
}

extern "C" bool InstallInlineHook(void* targetFunc, void* fakeFunc, void** originalTrampoline) {
    if (!targetFunc || !fakeFunc) return false;
    uint32_t* targetCode = (uint32_t*)targetFunc;
    long pageSize = sysconf(_SC_PAGESIZE);

    // Refuse PC-relative first instructions (naive trampoline would break them)
    uint32_t insn = targetCode[0];
    if ((insn & 0xFC000000) == 0x14000000) { LOGE("target starts with B — refusing"); return false; }
    if ((insn & 0xFC000000) == 0x94000000) { LOGE("target starts with BL — refusing"); return false; }
    if ((insn & 0x9F000000) == 0x10000000) { LOGE("target starts with ADR — refusing"); return false; }
    if ((insn & 0x9F000000) == 0x90000000) { LOGE("target starts with ADRP — refusing"); return false; }
    if ((insn & 0x3B000000) == 0x18000000) { LOGE("target starts with LDR-literal — refusing"); return false; }

    void* relayPage = AllocateMemoryNear(targetFunc, pageSize);
    if (!relayPage) { LOGE("no relay page within 120MB of %p", targetFunc); return false; }

    uint32_t* relay = (uint32_t*)relayPage;
    relay[0] = ARM64_LDR_X17_8;
    relay[1] = ARM64_BR_X17;
    *(uint64_t*)&relay[2] = (uint64_t)fakeFunc;
    __builtin___clear_cache((char*)relay, (char*)relay + 16);

    void* tramp = mmap(NULL, pageSize, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tramp == MAP_FAILED) { munmap(relayPage, pageSize); return false; }

    uint32_t* tc = (uint32_t*)tramp;
    tc[0] = insn;
    tc[1] = ARM64_LDR_X17_8;
    tc[2] = ARM64_BR_X17;
    *(uint64_t*)&tc[3] = (uint64_t)targetFunc + 4;
    if (mprotect(tramp, pageSize, PROT_READ | PROT_EXEC) != 0) {
        munmap(tramp, pageSize); munmap(relayPage, pageSize); return false;
    }
    __builtin___clear_cache((char*)tramp, (char*)tramp + 28);

    uintptr_t tStart = (uintptr_t)targetFunc & ~(pageSize - 1);
    if (mprotect((void*)tStart, pageSize, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        LOGE("mprotect RWX on target failed: %d", errno);
        munmap(tramp, pageSize); munmap(relayPage, pageSize); return false;
    }

    int64_t off = (int64_t)relayPage - (int64_t)targetFunc;
    uint32_t branch = ARM64_BRANCH_OFFSET(off);
    targetCode[0] = branch;
    __builtin___clear_cache((char*)targetFunc, (char*)targetFunc + 4);

    if (targetCode[0] != branch) {
        LOGE("patch verify failed at %p", targetFunc);
        targetCode[0] = insn;
        __builtin___clear_cache((char*)targetFunc, (char*)targetFunc + 4);
        mprotect((void*)tStart, pageSize, PROT_READ | PROT_EXEC);   // restore even on failure
        munmap(tramp, pageSize); munmap(relayPage, pageSize);
        return false;
    }

    // ---- CRITICAL: restore target page to RX (never leave RWX) ----
    // Prevents BTI/MTE rejection on API 34+ and execmem SELinux audits.
    mprotect((void*)tStart, pageSize, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char*)targetFunc, (char*)targetFunc + 4);
    // ----------------------------------------------------------------

    *originalTrampoline = tramp;
    LOGD("inline hook %p OK: relay %p, dist 0x%lx",
         targetFunc, relayPage,
         (unsigned long)((intptr_t)relayPage - (intptr_t)targetFunc));
    return true;
}