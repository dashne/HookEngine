#include "SvcHook.h"
#include <android/log.h>
#include <signal.h>
#include <ucontext.h>
#include <sys/mman.h>
#include <unistd.h>
#include <link.h>
#include <cstdio>
#include <cstring>
#include <cerrno>
#include <atomic>
#include <mutex>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

#define SVC_ZERO     0xD4000001u        // svc #0
// BRK encoding: 1101_0100_001_imm16_00000
//              0xD4200000 | (imm16 << 5)
// For imm16 = 0x4242:
//   0xD4200000 | (0x4242 << 5) = 0xD4200000 | 0x84840 = 0xD4284840
// NOT 0xD4208484 — that value has bits 4:0 = 00100, which makes it an
// UNDEFINED instruction on ARM64, raising SIGILL (not SIGTRAP). That was
// the Session-47 crash: every hook fire got SIGILL, killed the process.
#define BRK_MAGIC    0xD4284840u        // brk #0x4242
#define BRK_ANY      0xD4200000u        // brk #imm16 (any)
#define BRK_MASK     0xFFE0001Fu        // matches BRK class + low bits
#define BRK_IMM16(i) (((uint32_t)(i) >> 5) & 0xFFFFu)

#define MAX_SVC_SLOTS 16

struct SvcSlot {
    volatile bool    active      = false;
    void*            site        = nullptr;
    uint32_t         orig_insn   = 0;
    int              expected_nr = -1;
    SyscallHandlerFn handler     = nullptr;
};

static SvcSlot          g_slots[MAX_SVC_SLOTS];
static std::mutex       g_slot_mutex;
static struct sigaction g_old_sigtrap          = {};
static std::atomic<bool> g_handler_installed{false};
static __thread int     g_in_handler           = 0;

// ─── Naked helper: issue the real syscall with args already in scope ───
//
// Called from SIGTRAP handler with (nr, a0..a5). Rearranges to kernel ABI
// (x8=nr, x0..x5=args), executes svc #0, returns x0.
extern "C" __attribute__((naked, used))
uint64_t svc_issue_raw(uint64_t nr, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5) {
    __asm__ volatile(
            "mov x8, x0\n"
            "mov x0, x1\n"
            "mov x1, x2\n"
            "mov x2, x3\n"
            "mov x3, x4\n"
            "mov x4, x5\n"
            "mov x5, x6\n"
            "svc #0\n"
            "ret\n"
            );
}

// ─── Chain to previous SIGTRAP handler ─────────────────────────────────
static void ChainOldSigtrap(int sig, siginfo_t* info, void* ucv) {
    if (g_old_sigtrap.sa_flags & SA_SIGINFO) {
        if (g_old_sigtrap.sa_sigaction) {
            g_old_sigtrap.sa_sigaction(sig, info, ucv);
            return;
        }
    } else {
        if (g_old_sigtrap.sa_handler == SIG_DFL ||
            g_old_sigtrap.sa_handler == SIG_IGN) {
            // Fall through to default reset + re-raise
        } else if (g_old_sigtrap.sa_handler) {
            g_old_sigtrap.sa_handler(sig);
            return;
        }
    }
    sigaction(SIGTRAP, &g_old_sigtrap, nullptr);
    raise(SIGTRAP);
}

// ─── SIGTRAP handler ───────────────────────────────────────────────────
static void svc_sigtrap_handler(int sig, siginfo_t* info, void* ucv) {
    ucontext_t* uc = (ucontext_t*)ucv;
    uint64_t pc = uc->uc_mcontext.pc;
    uint32_t insn = *(volatile uint32_t*)pc;

    // Only our magic BRK is handled. Anything else chains to the old handler.
    if ((insn & BRK_MASK) != BRK_ANY || BRK_IMM16(insn) != 0x4242u) {
        ChainOldSigtrap(sig, info, ucv);
        return;
    }

    uint64_t x0 = uc->uc_mcontext.regs[0];
    uint64_t x1 = uc->uc_mcontext.regs[1];
    uint64_t x2 = uc->uc_mcontext.regs[2];
    uint64_t x3 = uc->uc_mcontext.regs[3];
    uint64_t x4 = uc->uc_mcontext.regs[4];
    uint64_t x5 = uc->uc_mcontext.regs[5];
    uint64_t x8 = uc->uc_mcontext.regs[8];

    if (g_in_handler) {
        uint64_t ret = svc_issue_raw(x8, x0, x1, x2, x3, x4, x5);
        uc->uc_mcontext.regs[0] = ret;
        uc->uc_mcontext.pc = pc + 4;
        return;
    }

    g_in_handler = 1;

    uint64_t ret = svc_issue_raw(x8, x0, x1, x2, x3, x4, x5);

    SyscallHandlerFn fn = nullptr;
    int expected_nr = -1;
    for (int i = 0; i < MAX_SVC_SLOTS; i++) {
        if (__atomic_load_n(&g_slots[i].active, __ATOMIC_ACQUIRE) &&
            g_slots[i].site == (void*)pc) {
            fn = g_slots[i].handler;
            expected_nr = g_slots[i].expected_nr;
            break;
        }
    }

    if (fn && (expected_nr < 0 || (uint64_t)expected_nr == x8)) {
        ret = fn(x8, x0, x1, x2, x3, x4, x5, ret);
    }

    g_in_handler = 0;

    uc->uc_mcontext.regs[0] = ret;
    uc->uc_mcontext.pc = pc + 4;
}

// ─── Page protection reader ────────────────────────────────────────────
static bool SvcReadProt(uintptr_t page, int* out) {
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
                *out = p; ok = true; break;
            }
        }
    }
    fclose(f);
    return ok;
}

// ─── Public API ────────────────────────────────────────────────────────
extern "C" HookResult SvcHookInit() {
    if (g_handler_installed.load(std::memory_order_acquire)) return HOOK_OK;

    struct sigaction sa = {};
    sa.sa_sigaction = svc_sigtrap_handler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGTRAP);

    if (sigaction(SIGTRAP, &sa, &g_old_sigtrap) != 0) {
        LOGE("SvcHookInit: sigaction failed errno=%d", errno);
        return HOOK_INTERNAL_ERROR;
    }
    g_handler_installed.store(true, std::memory_order_release);
    LOGD("SvcHookInit: SIGTRAP handler installed (old sa_handler=%p)",
         (void*)g_old_sigtrap.sa_handler);
    return HOOK_OK;
}

extern "C" HookResult SvcHookAttach(void* svc_addr, int expected_nr,
                                    SyscallHandlerFn handler) {
    if (!svc_addr || !handler) return HOOK_INTERNAL_ERROR;
    if (((uintptr_t)svc_addr & 3u) != 0) return HOOK_TARGET_INVALID;

    HookResult r = SvcHookInit();
    if (r != HOOK_OK) return r;

    uint32_t orig = *(volatile uint32_t*)svc_addr;
    if (orig != SVC_ZERO) {
        LOGE("SvcHookAttach: not svc #0 at %p (0x%08x)", svc_addr, orig);
        return HOOK_UNSUPPORTED_INSN;
    }

    std::lock_guard<std::mutex> lk(g_slot_mutex);

    for (auto& s : g_slots) {
        if (s.active && s.site == svc_addr) return HOOK_ALREADY_HOOKED;
    }

    int slot = -1;
    for (int i = 0; i < MAX_SVC_SLOTS; i++) {
        if (!g_slots[i].active) { slot = i; break; }
    }
    if (slot < 0) return HOOK_ALLOC_FAILED;

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)svc_addr & ~((uintptr_t)ps - 1);
    int prot = 0;
    if (!SvcReadProt(page, &prot)) return HOOK_TARGET_INVALID;
    if ((prot & PROT_EXEC) == 0) return HOOK_TARGET_INVALID;

    if (mprotect((void*)page, ps, prot | PROT_WRITE) != 0) {
        LOGE("SvcHookAttach: mprotect RW failed errno=%d", errno);
        return HOOK_MPROTECT_FAILED;
    }
    *(volatile uint32_t*)svc_addr = BRK_MAGIC;
    __builtin___clear_cache((char*)svc_addr, (char*)svc_addr + 4);
    if (mprotect((void*)page, ps, prot) != 0) {
        LOGE("SvcHookAttach: mprotect restore failed errno=%d", errno);
        return HOOK_MPROTECT_FAILED;
    }

    g_slots[slot].site        = svc_addr;
    g_slots[slot].orig_insn   = orig;
    g_slots[slot].expected_nr = expected_nr;
    g_slots[slot].handler     = handler;
    __atomic_store_n(&g_slots[slot].active, true, __ATOMIC_RELEASE);

    LOGD("SvcHookAttach: %p slot=%d nr=%d (patched to 0x%08x)",
         svc_addr, slot, expected_nr, BRK_MAGIC);
    return HOOK_OK;
}

extern "C" HookResult SvcHookDetach(void* svc_addr) {
    if (!svc_addr) return HOOK_INTERNAL_ERROR;

    {
        std::lock_guard<std::mutex> lk(g_slot_mutex);
        bool found = false;
        for (auto& s : g_slots) {
            if (s.active && s.site == svc_addr) {
                __atomic_store_n(&s.active, false, __ATOMIC_RELEASE);
                found = true;
                break;
            }
        }
        if (!found) return HOOK_NOT_HOOKED;
    }

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)svc_addr & ~((uintptr_t)ps - 1);
    int prot = 0;
    if (!SvcReadProt(page, &prot)) return HOOK_TARGET_INVALID;

    if (mprotect((void*)page, ps, prot | PROT_WRITE) != 0) {
        LOGE("SvcHookDetach: mprotect RW failed errno=%d", errno);
        return HOOK_MPROTECT_FAILED;
    }
    *(volatile uint32_t*)svc_addr = SVC_ZERO;
    __builtin___clear_cache((char*)svc_addr, (char*)svc_addr + 4);
    if (mprotect((void*)page, ps, prot) != 0) {
        LOGE("SvcHookDetach: mprotect restore failed errno=%d", errno);
        return HOOK_MPROTECT_FAILED;
    }

    LOGD("SvcHookDetach: %p restored", svc_addr);
    return HOOK_OK;
}

extern "C" int SvcHookCount() {
    std::lock_guard<std::mutex> lk(g_slot_mutex);
    int n = 0;
    for (auto& s : g_slots) if (s.active) n++;
    return n;
}

extern "C" int SvcHookScanLib(const char* lib_substr) {
    if (!lib_substr || !lib_substr[0]) return -1;

    struct Ctx {
        const char* substr;
        int         count;
    } ctx = { lib_substr, 0 };

    dl_iterate_phdr([](struct dl_phdr_info* info, size_t, void* data) -> int {
        Ctx* c = (Ctx*)data;
        if (!info->dlpi_name || !strstr(info->dlpi_name, c->substr)) return 0;

        for (int i = 0; i < info->dlpi_phnum; i++) {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type != PT_LOAD) continue;
            if (!(ph.p_flags & PF_X)) continue;

            uintptr_t start = info->dlpi_addr + ph.p_vaddr;
            uintptr_t end   = start + ph.p_memsz;
            for (uintptr_t addr = (start + 3) & ~(uintptr_t)3;
                 addr + 4 <= end; addr += 4) {
                if (*(volatile uint32_t*)addr == SVC_ZERO) c->count++;
            }
        }
        return 0;
    }, &ctx);

    LOGD("SvcHookScanLib(%s): %d svc #0 sites found", lib_substr, ctx.count);
    return ctx.count;
}

extern "C" void SvcHookResetAll() {
    std::lock_guard<std::mutex> lk(g_slot_mutex);
    for (auto& s : g_slots) {
        if (!s.active) continue;
        void* site = s.site;
        __atomic_store_n(&s.active, false, __ATOMIC_RELEASE);

        long ps = sysconf(_SC_PAGESIZE);
        uintptr_t page = (uintptr_t)site & ~((uintptr_t)ps - 1);
        int prot = 0;
        if (SvcReadProt(page, &prot)) {
            if (mprotect((void*)page, ps, prot | PROT_WRITE) == 0) {
                *(volatile uint32_t*)site = s.orig_insn;
                __builtin___clear_cache((char*)site, (char*)site + 4);
                mprotect((void*)page, ps, prot);
            }
        }
    }
}