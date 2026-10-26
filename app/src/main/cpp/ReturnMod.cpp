#include "ReturnMod.h"
#include "arm64_reloc.h"
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>
#include <cstring>
#include <cstdio>
#include <mutex>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);

// From InlineHooker.cpp — C++ linkage (not extern "C"), declared with
// matching signature so the linker resolves the symbol.
void* AllocateMemoryNear(void* target, size_t size);

#define MAX_RETMODS 16

struct RetModSlot {
    bool             active       = false;
    void*            target       = nullptr;
    void*            shadow_tramp = nullptr;
    ReturnModifierFn modifier     = nullptr;
    int              stub_slot    = -1;
};

static RetModSlot  g_retmods[MAX_RETMODS];
static std::mutex  g_retmod_mutex;

// Trampoline table read by the naked stubs via adrp+add. Must have
// hidden visibility — the PIC linker refuses R_AARCH64_ADR_PREL_PG_HI21
// against a preemptible (default-visibility) symbol. Same constraint as
// chain_dispatch_helper in ChainHook.cpp.
extern "C" __attribute__((visibility("hidden")))
void* g_retmod_tramps[MAX_RETMODS] = {nullptr};

// Called by every stub after the original body returns. Dispatches to
// the per-slot modifier under the retmod mutex and returns the value
// the caller sees. Lock released before invoking modifier so the
// modifier can re-enter retmod (e.g., on a different target).
extern "C" __attribute__((visibility("hidden")))
uint64_t retmod_dispatch_helper(uint64_t* saved_args, uint64_t orig_ret, int slot) {
    ReturnModifierFn fn = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_retmod_mutex);
        if (slot >= 0 && slot < MAX_RETMODS && g_retmods[slot].active) {
            fn = g_retmods[slot].modifier;
        }
    }
    if (!fn) return orig_ret;
    return fn(saved_args, orig_ret);
}

// ─── Per-slot stubs (naked asm, macro-generated) ────────────────────
//
// Frame layout (80 bytes, 16-byte aligned):
//   sp+0   : x0, x1
//   sp+16  : x2, x3
//   sp+32  : x4, x5
//   sp+48  : x6, x7
//   sp+64  : x30 (link register — restored on the way out)
//   sp+72  : x9  (callee-scratch we use for slot id — saved defensively)
//
// Sequence:
//   save args + LR
//   → load shadow trampoline from g_retmod_tramps[N]
//   → blr x16 (body runs, x0 = orig_ret)
//   → x1 = orig_ret, x0 = &saved_args, x2 = slot
//   → blr retmod_dispatch_helper (x0 = new_return)
//   → restore LR
//   → ret (x0 = new_return)
//
// The stub is installed via InstallInlineHookEx as the fakeFunc. The
// inline hook's relay does `br x17` (indirect — no LR change), so LR at
// stub entry still points to target's caller. Our `blr x16` overwrites
// LR, so we save the original at entry and restore before the final ret.
#define DEFINE_RETMOD_STUB(N) \
extern "C" __attribute__((naked, used)) void retmod_stub_##N() { \
    __asm__ volatile( \
        "sub sp, sp, #80\n" \
        "stp x0, x1, [sp, #0]\n" \
        "stp x2, x3, [sp, #16]\n" \
        "stp x4, x5, [sp, #32]\n" \
        "stp x6, x7, [sp, #48]\n" \
        "str x30, [sp, #64]\n" \
        "str x9,  [sp, #72]\n" \
        "mov x9, #" #N "\n" \
        "adrp x16, g_retmod_tramps\n" \
        "add  x16, x16, :lo12:g_retmod_tramps\n" \
        "ldr  x16, [x16, x9, lsl #3]\n" \
        "blr  x16\n" \
        "mov x1, x0\n" \
        "mov x0, sp\n" \
        "mov x2, #" #N "\n" \
        "adrp x16, retmod_dispatch_helper\n" \
        "add  x16, x16, :lo12:retmod_dispatch_helper\n" \
        "blr  x16\n" \
        "ldr x30, [sp, #64]\n" \
        "ldr x9,  [sp, #72]\n" \
        "add sp, sp, #80\n" \
        "ret\n" \
    ); \
}

DEFINE_RETMOD_STUB(0)
DEFINE_RETMOD_STUB(1)
DEFINE_RETMOD_STUB(2)
DEFINE_RETMOD_STUB(3)
DEFINE_RETMOD_STUB(4)
DEFINE_RETMOD_STUB(5)
DEFINE_RETMOD_STUB(6)
DEFINE_RETMOD_STUB(7)
DEFINE_RETMOD_STUB(8)
DEFINE_RETMOD_STUB(9)
DEFINE_RETMOD_STUB(10)
DEFINE_RETMOD_STUB(11)
DEFINE_RETMOD_STUB(12)
DEFINE_RETMOD_STUB(13)
DEFINE_RETMOD_STUB(14)
DEFINE_RETMOD_STUB(15)

static void* retmodStubFor(int slot) {
    static void* fns[] = {
            (void*)&retmod_stub_0,  (void*)&retmod_stub_1,  (void*)&retmod_stub_2,  (void*)&retmod_stub_3,
            (void*)&retmod_stub_4,  (void*)&retmod_stub_5,  (void*)&retmod_stub_6,  (void*)&retmod_stub_7,
            (void*)&retmod_stub_8,  (void*)&retmod_stub_9,  (void*)&retmod_stub_10, (void*)&retmod_stub_11,
            (void*)&retmod_stub_12, (void*)&retmod_stub_13, (void*)&retmod_stub_14, (void*)&retmod_stub_15,
    };
    if (slot < 0 || slot >= (int)(sizeof(fns) / sizeof(fns[0]))) return nullptr;
    return fns[slot];
}

// ─── Shadow trampoline ──────────────────────────────────────────────
//
// Same logic as InlineHooker.cpp::BuildTrampoline, but exposed here so
// retmod can publish its trampoline into g_retmod_tramps[] BEFORE the
// inline hook is installed. Eliminates the install→populate race window
// that would exist if we reused InstallInlineHookEx's trampoline.
static void* BuildRetModShadow(void* target) {
    long ps = sysconf(_SC_PAGESIZE);
    void* tramp = AllocateMemoryNear(target, ps);
    if (!tramp) return nullptr;

    uint32_t orig = *(volatile uint32_t*)target;
    uint32_t out[16] = {0};
    int n = EmitRelocated(orig, (uint64_t)target, (uint64_t)tramp, out, 16);
    if (n == 0) {
        LOGE("BuildRetModShadow: cannot relocate insn 0x%08x at %p", orig, target);
        munmap(tramp, ps);
        return nullptr;
    }
    uint32_t ret;
    if (!EncodeBranch(&ret, (uint64_t)tramp + n * 4, (uint64_t)target + 4, false)) {
        LOGE("BuildRetModShadow: return branch out of range");
        munmap(tramp, ps);
        return nullptr;
    }
    out[n] = ret;
    memcpy(tramp, out, (n + 1) * 4);
    __builtin___clear_cache((char*)tramp, (char*)tramp + (n + 1) * 4);
    if (mprotect(tramp, ps, PROT_READ | PROT_EXEC) != 0) {
        LOGE("BuildRetModShadow: mprotect RX failed errno=%d", errno);
        munmap(tramp, ps);
        return nullptr;
    }
    return tramp;
}

static bool RetModReadProt(uintptr_t page, int* out) {
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

// ─── Public API ─────────────────────────────────────────────────────
extern "C" HookResult AttachRetMod(void* target, ReturnModifierFn modifier, void** outTrampoline) {
    if (!target || !modifier) return HOOK_INTERNAL_ERROR;
    if (((uintptr_t)target & 3u) != 0) return HOOK_TARGET_INVALID;

    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)target & ~((uintptr_t)ps - 1);
    int prot = 0;
    if (!RetModReadProt(page, &prot)) return HOOK_TARGET_INVALID;
    if ((prot & PROT_EXEC) == 0) return HOOK_TARGET_INVALID;

    std::lock_guard<std::mutex> lk(g_retmod_mutex);

    for (auto& s : g_retmods) {
        if (s.active && s.target == target) return HOOK_ALREADY_HOOKED;
    }

    int slot = -1;
    for (int i = 0; i < MAX_RETMODS; i++) {
        if (!g_retmods[i].active) { slot = i; break; }
    }
    if (slot < 0) return HOOK_ALLOC_FAILED;

    void* stub = retmodStubFor(slot);
    if (!stub) return HOOK_INTERNAL_ERROR;

    void* shadow = BuildRetModShadow(target);
    if (!shadow) return HOOK_ALLOC_FAILED;

    // Publish slot state BEFORE the hook installs. Once target[0] is
    // patched, any thread entering the stub sees a valid trampoline.
    g_retmod_tramps[slot]           = shadow;
    g_retmods[slot].modifier        = modifier;
    g_retmods[slot].active          = true;
    g_retmods[slot].target          = target;
    g_retmods[slot].shadow_tramp    = shadow;
    g_retmods[slot].stub_slot       = slot;

    void* unused_tramp = nullptr;
    HookResult r = InstallInlineHookEx(target, stub, &unused_tramp, "retmod");
    if (r != HOOK_OK) {
        LOGE("AttachRetMod: InstallInlineHookEx failed r=%d", (int)r);
        g_retmod_tramps[slot] = nullptr;
        g_retmods[slot]       = RetModSlot{};
        munmap(shadow, (size_t)ps);
        return r;
    }

    if (outTrampoline) *outTrampoline = shadow;
    LOGD("AttachRetMod: %p slot=%d shadow=%p (new retmod)", target, slot, shadow);
    return HOOK_OK;
}

extern "C" HookResult DetachRetMod(void* target) {
    if (!target) return HOOK_INTERNAL_ERROR;
    std::lock_guard<std::mutex> lk(g_retmod_mutex);

    for (auto& s : g_retmods) {
        if (!s.active || s.target != target) continue;
        int slot = s.stub_slot;
        void* shadow = s.shadow_tramp;

        HookResult r = UninstallInlineHook(target);
        if (r != HOOK_OK && r != HOOK_NOT_HOOKED) return r;

        // UninstallInlineHook's 100ms deferred-free grace covers any
        // in-flight stub execution. By the time it returns, no stub is
        // in the middle of `blr shadow`, so both the tramp-table clear
        // and the shadow munmap below are safe.
        g_retmod_tramps[slot] = nullptr;
        s = RetModSlot{};

        if (shadow) munmap(shadow, (size_t)sysconf(_SC_PAGESIZE));
        LOGD("DetachRetMod: %p slot=%d — removed", target, slot);
        return HOOK_OK;
    }
    return HOOK_NOT_HOOKED;
}

extern "C" int RetModCount() {
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    int n = 0;
    for (auto& s : g_retmods) if (s.active) n++;
    return n;
}

extern "C" void RetModResetAll() {
    std::lock_guard<std::mutex> lk(g_retmod_mutex);
    for (int i = 0; i < MAX_RETMODS; i++) {
        auto& s = g_retmods[i];
        if (!s.active) continue;
        UninstallInlineHook(s.target);   // HOOK_NOT_HOOKED is fine — UninstallAllHooks may have beaten us
        if (s.shadow_tramp) munmap(s.shadow_tramp, (size_t)sysconf(_SC_PAGESIZE));
        g_retmod_tramps[i] = nullptr;
        s = RetModSlot{};
    }
}