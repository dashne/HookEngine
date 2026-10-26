#include "RegInspect.h"
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
void* AllocateMemoryNear(void* target, size_t size);   // C++ linkage from InlineHooker.cpp

#define MAX_REGI_SLOTS 16
#define REGI_FRAME_SIZE 784

struct RegInspectSlot {
    bool    active       = false;
    void*   target       = nullptr;
    void*   shadow_tramp = nullptr;
    int     mode         = 0;
    int     stub_slot    = -1;
};

static RegInspectSlot      g_slots[MAX_REGI_SLOTS];
static RegInspectSnapshot  g_snapshots[MAX_REGI_SLOTS];
static std::mutex          g_mutex;

// ─── Dispatcher (C++, called from every stub) ──────────────────────
//
// frame layout (matches the stub's SP):
//   frame[0..30]    x0..x30
//   frame[31]       sp_orig
//   frame[32]       nzcv
//   frame[33]       slot_id
//   frame[34..97]   q0..q31 (low/high u64 pairs)
//
// On return:
//   frame[32] = mode flag (0 = probe, 1 = passthru) — read by stub into x16
//   frame[33] = trampoline address (unused in probe mode) — read by stub into x17
extern "C" __attribute__((visibility("hidden")))
void reginspect_dispatch_helper(uint64_t* frame) {
    uint64_t slot_id = frame[33];

    std::lock_guard<std::mutex> lk(g_mutex);

    for (auto& s : g_slots) {
        if (s.stub_slot == (int)slot_id) {
            // Publish snapshot (x[], sp_orig, nzcv, pc, slot_id, q[])
            RegInspectSnapshot* snap = &g_snapshots[slot_id];
            memcpy(snap->x, frame, 31 * sizeof(uint64_t));       // x0..x30
            snap->x[31]   = frame[31];                            // sp_orig
            snap->nzcv    = frame[32];
            snap->pc      = (uint64_t)s.target;
            snap->slot_id = (uint64_t)slot_id;
            memcpy(snap->q, &frame[34], 64 * sizeof(uint64_t));   // q0..q31

            // Tell the stub what to do next.
            frame[32] = s.active ? (uint64_t)(s.mode == REGINSPECT_MODE_PASSTHRU ? 1 : 0) : 0;
            frame[33] = s.active ? (uint64_t)s.shadow_tramp : 0;
            return;
        }
    }
    // Slot not found (detach race) — safe fallback: skip body, ret with arg0.
    frame[32] = 0;
    frame[33] = 0;
}

// ─── Per-slot naked stubs (16 copies, slot id baked as immediate) ──
//
// At entry via InstallInlineHookEx relay:
//   SP   = caller's SP
//   x0..x15, x18..x30 = caller's values
//   x16  = caller's x16 (unchanged by relay)
//   x17  = stub address (clobbered by relay `ldr x17, [pc, #8]; br x17`)
//   x30  = caller's LR
//
// Prologue saves everything to a 784-byte frame, calls dispatcher,
// restores, branches based on mode.
#define DEFINE_REGI_STUB(N) \
extern "C" __attribute__((naked, used)) void reginspect_stub_##N() { \
    __asm__ volatile( \
        "sub sp, sp, #784\n" \
        "stp x0,  x1,  [sp, #0]\n" \
        "stp x2,  x3,  [sp, #16]\n" \
        "stp x4,  x5,  [sp, #32]\n" \
        "stp x6,  x7,  [sp, #48]\n" \
        "stp x8,  x9,  [sp, #64]\n" \
        "stp x10, x11, [sp, #80]\n" \
        "stp x12, x13, [sp, #96]\n" \
        "stp x14, x15, [sp, #112]\n" \
        "stp x16, x17, [sp, #128]\n" \
        "stp x18, x19, [sp, #144]\n" \
        "stp x20, x21, [sp, #160]\n" \
        "stp x22, x23, [sp, #176]\n" \
        "stp x24, x25, [sp, #192]\n" \
        "stp x26, x27, [sp, #208]\n" \
        "stp x28, x29, [sp, #224]\n" \
        "add x16, sp, #784\n" \
        "stp x30, x16, [sp, #240]\n" \
        "mrs x16, nzcv\n" \
        "str x16, [sp, #256]\n" \
        "mov x16, #" #N "\n" \
        "str x16, [sp, #264]\n" \
        "stp q0,  q1,  [sp, #272]\n" \
        "stp q2,  q3,  [sp, #304]\n" \
        "stp q4,  q5,  [sp, #336]\n" \
        "stp q6,  q7,  [sp, #368]\n" \
        "stp q8,  q9,  [sp, #400]\n" \
        "stp q10, q11, [sp, #432]\n" \
        "stp q12, q13, [sp, #464]\n" \
        "stp q14, q15, [sp, #496]\n" \
        "stp q16, q17, [sp, #528]\n" \
        "stp q18, q19, [sp, #560]\n" \
        "stp q20, q21, [sp, #592]\n" \
        "stp q22, q23, [sp, #624]\n" \
        "stp q24, q25, [sp, #656]\n" \
        "stp q26, q27, [sp, #688]\n" \
        "stp q28, q29, [sp, #720]\n" \
        "stp q30, q31, [sp, #752]\n" \
        "mov x0, sp\n" \
        "adrp x16, reginspect_dispatch_helper\n" \
        "add  x16, x16, :lo12:reginspect_dispatch_helper\n" \
        "blr  x16\n" \
        "ldr x16, [sp, #256]\n" \
        "ldr x17, [sp, #264]\n" \
        "ldp x0,  x1,  [sp, #0]\n" \
        "ldp x2,  x3,  [sp, #16]\n" \
        "ldp x4,  x5,  [sp, #32]\n" \
        "ldp x6,  x7,  [sp, #48]\n" \
        "ldp x8,  x9,  [sp, #64]\n" \
        "ldp x10, x11, [sp, #80]\n" \
        "ldp x12, x13, [sp, #96]\n" \
        "ldp x14, x15, [sp, #112]\n" \
        "ldp x18, x19, [sp, #144]\n" \
        "ldp x20, x21, [sp, #160]\n" \
        "ldp x22, x23, [sp, #176]\n" \
        "ldp x24, x25, [sp, #192]\n" \
        "ldp x26, x27, [sp, #208]\n" \
        "ldp x28, x29, [sp, #224]\n" \
        "ldr x30, [sp, #240]\n" \
        "ldp q0,  q1,  [sp, #272]\n" \
        "ldp q2,  q3,  [sp, #304]\n" \
        "ldp q4,  q5,  [sp, #336]\n" \
        "ldp q6,  q7,  [sp, #368]\n" \
        "ldp q8,  q9,  [sp, #400]\n" \
        "ldp q10, q11, [sp, #432]\n" \
        "ldp q12, q13, [sp, #464]\n" \
        "ldp q14, q15, [sp, #496]\n" \
        "ldp q16, q17, [sp, #528]\n" \
        "ldp q18, q19, [sp, #560]\n" \
        "ldp q20, q21, [sp, #592]\n" \
        "ldp q22, q23, [sp, #624]\n" \
        "ldp q24, q25, [sp, #656]\n" \
        "ldp q26, q27, [sp, #688]\n" \
        "ldp q28, q29, [sp, #720]\n" \
        "ldp q30, q31, [sp, #752]\n" \
        "add sp, sp, #784\n" \
        "cbnz x16, 1f\n" \
        "ret\n" \
        "1: br x17\n" \
    ); \
}

DEFINE_REGI_STUB(0)
DEFINE_REGI_STUB(1)
DEFINE_REGI_STUB(2)
DEFINE_REGI_STUB(3)
DEFINE_REGI_STUB(4)
DEFINE_REGI_STUB(5)
DEFINE_REGI_STUB(6)
DEFINE_REGI_STUB(7)
DEFINE_REGI_STUB(8)
DEFINE_REGI_STUB(9)
DEFINE_REGI_STUB(10)
DEFINE_REGI_STUB(11)
DEFINE_REGI_STUB(12)
DEFINE_REGI_STUB(13)
DEFINE_REGI_STUB(14)
DEFINE_REGI_STUB(15)

static void* stubFor(int slot) {
    static void* fns[] = {
            (void*)&reginspect_stub_0,  (void*)&reginspect_stub_1,  (void*)&reginspect_stub_2,  (void*)&reginspect_stub_3,
            (void*)&reginspect_stub_4,  (void*)&reginspect_stub_5,  (void*)&reginspect_stub_6,  (void*)&reginspect_stub_7,
            (void*)&reginspect_stub_8,  (void*)&reginspect_stub_9,  (void*)&reginspect_stub_10, (void*)&reginspect_stub_11,
            (void*)&reginspect_stub_12, (void*)&reginspect_stub_13, (void*)&reginspect_stub_14, (void*)&reginspect_stub_15,
    };
    if (slot < 0 || slot >= (int)(sizeof(fns) / sizeof(fns[0]))) return nullptr;
    return fns[slot];
}

// ─── Shadow trampoline (built BEFORE install, same pattern as ReturnMod)
static void* BuildShadow(void* target) {
    long ps = sysconf(_SC_PAGESIZE);
    void* tramp = AllocateMemoryNear(target, ps);
    if (!tramp) return nullptr;

    uint32_t orig = *(volatile uint32_t*)target;
    uint32_t out[16] = {0};
    int n = EmitRelocated(orig, (uint64_t)target, (uint64_t)tramp, out, 16);
    if (n == 0) {
        munmap(tramp, ps);
        return nullptr;
    }
    uint32_t ret;
    if (!EncodeBranch(&ret, (uint64_t)tramp + n * 4, (uint64_t)target + 4, false)) {
        munmap(tramp, ps);
        return nullptr;
    }
    out[n] = ret;
    memcpy(tramp, out, (n + 1) * 4);
    __builtin___clear_cache((char*)tramp, (char*)tramp + (n + 1) * 4);
    if (mprotect(tramp, ps, PROT_READ | PROT_EXEC) != 0) {
        munmap(tramp, ps);
        return nullptr;
    }
    return tramp;
}

// ─── Public API ────────────────────────────────────────────────────
extern "C" HookResult AttachRegInspect(void* target, int mode, void** outShadowTramp) {
    if (!target) return HOOK_INTERNAL_ERROR;
    if (mode != REGINSPECT_MODE_PROBE && mode != REGINSPECT_MODE_PASSTHRU)
        return HOOK_INTERNAL_ERROR;
    if (((uintptr_t)target & 3u) != 0) return HOOK_TARGET_INVALID;

    std::lock_guard<std::mutex> lk(g_mutex);

    for (auto& s : g_slots) {
        if (s.active && s.target == target) return HOOK_ALREADY_HOOKED;
    }

    int slot = -1;
    for (int i = 0; i < MAX_REGI_SLOTS; i++) {
        if (!g_slots[i].active) { slot = i; break; }
    }
    if (slot < 0) return HOOK_ALLOC_FAILED;

    void* stub = stubFor(slot);
    if (!stub) return HOOK_INTERNAL_ERROR;

    void* shadow = BuildShadow(target);
    if (!shadow) return HOOK_ALLOC_FAILED;

    // Publish slot state BEFORE install (eliminates dispatcher race).
    g_slots[slot].target       = target;
    g_slots[slot].shadow_tramp = shadow;
    g_slots[slot].mode         = mode;
    g_slots[slot].stub_slot    = slot;
    __atomic_store_n(&g_slots[slot].active, true, __ATOMIC_RELEASE);

    void* unused = nullptr;
    HookResult r = InstallInlineHookEx(target, stub, &unused, "reginspect");
    if (r != HOOK_OK) {
        __atomic_store_n(&g_slots[slot].active, false, __ATOMIC_RELEASE);
        g_slots[slot] = RegInspectSlot{};
        munmap(shadow, (size_t)sysconf(_SC_PAGESIZE));
        return r;
    }

    if (outShadowTramp) *outShadowTramp = shadow;
    LOGD("AttachRegInspect: %p slot=%d mode=%d", target, slot, mode);
    return HOOK_OK;
}

extern "C" HookResult DetachRegInspect(void* target) {
    if (!target) return HOOK_INTERNAL_ERROR;
    std::lock_guard<std::mutex> lk(g_mutex);

    for (auto& s : g_slots) {
        if (!s.active || s.target != target) continue;
        int slot = s.stub_slot;
        void* shadow = s.shadow_tramp;

        HookResult r = UninstallInlineHook(target);
        if (r != HOOK_OK && r != HOOK_NOT_HOOKED) return r;

        __atomic_store_n(&s.active, false, __ATOMIC_RELEASE);
        s = RegInspectSlot{};

        if (shadow) munmap(shadow, (size_t)sysconf(_SC_PAGESIZE));
        LOGD("DetachRegInspect: %p slot=%d — removed", target, slot);
        return HOOK_OK;
    }
    return HOOK_NOT_HOOKED;
}

extern "C" HookResult RegInspectRead(void* target, RegInspectSnapshot* out) {
    if (!target || !out) return HOOK_INTERNAL_ERROR;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& s : g_slots) {
        if (!s.active || s.target != target) continue;
        *out = g_snapshots[s.stub_slot];
        return HOOK_OK;
    }
    return HOOK_NOT_HOOKED;
}

extern "C" int RegInspectCount() {
    std::lock_guard<std::mutex> lk(g_mutex);
    int n = 0;
    for (auto& s : g_slots) if (s.active) n++;
    return n;
}

extern "C" void RegInspectResetAll() {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& s : g_slots) {
        if (!s.active) continue;
        UninstallInlineHook(s.target);
        if (s.shadow_tramp) munmap(s.shadow_tramp, (size_t)sysconf(_SC_PAGESIZE));
        s = RegInspectSlot{};
    }
}