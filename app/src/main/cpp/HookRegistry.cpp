#include "HookRegistry.h"
#include <cstring>
#include <cstdio>
#include <mutex>
#include <vector>

// Unhook dispatch helpers — implemented in elf_hooker.cpp and
// InlineHooker.cpp respectively. Declared here rather than in HookRegistry.h
// because they are implementation details of the uninstall path, not part of
// the registry's public API. RegistryUninstallByTag needs them because it
// dispatches by handle type (HT_GOT vs HT_INLINE). S71 compile fix — the
// S70 probe-only version referenced these without declaring them.
extern "C" HookResult UnhookGotEntry(void*);
extern "C" HookResult UninstallInlineHook(void*);

static constexpr int kMaxHooks = 512;
static constexpr int kTagLen   = 48;
static HookHandle g_slots[kMaxHooks];
static std::mutex  g_reg_mutex;

// Session 71 — Feature 6.2: thread-local pending tag consumed by the next
// RegistryAlloc() on this thread. Set via RegistrySetPendingTag (or
// HookGotEntryExTagged, which wraps it). Cleared by RegistryAlloc on
// consumption and unconditionally by HookGotEntryExTagged on return.
// Rationale for TLS rather than a trailing argument on every install
// function: it keeps HookGotEntryEx / InstallInlineHookEx / ChainAttach /
// AttachRetMod source-compatible with every existing call site while
// still placing the tag inside the same critical section as the
// HS_FREE -> HS_INSTALLING transition. See §6.2 for why the tag MUST be
// fixed while the handle is HS_INSTALLING (a post-hoc setter would race
// with RegistryUninstallByTag's HS_INSTALLED filter).
static thread_local char g_pendingTag[kTagLen] = {0};

extern "C" int g_hook_fail_after_n = -1;

HookHandle* RegistryAlloc() {
    std::lock_guard<std::mutex> l(g_reg_mutex);
    for (auto& s : g_slots) {
        if (s.state == HS_FREE) {
            s.state = HS_INSTALLING;
            // Consume the pending tag atomically with the state flip.
            // Both operations happen under g_reg_mutex, and any
            // RegistryUninstallByTag walker also takes g_reg_mutex, so
            // there is no window where a half-tagged HS_INSTALLED handle
            // exists.
            if (g_pendingTag[0]) {
                memcpy(s.tag, g_pendingTag, kTagLen);
                s.tag[kTagLen - 1] = 0;
            } else {
                s.tag[0] = 0;
            }
            return &s;
        }
    }
    return nullptr;
}

void RegistryFree(HookHandle* h) {
    if (!h) return;
    std::lock_guard<std::mutex> l(g_reg_mutex);
    h->type = HT_INLINE;
    h->target = h->replacement = h->saved = h->trampoline = nullptr;
    h->relay = nullptr;
    h->tramp_size = 0;
    h->relay_size = 0;
    h->orig_prot = 0;
    h->page_size = 0;
    h->name[0] = 0;
    h->tag[0]  = 0;
    h->state = HS_FREE;
}

HookHandle* FindHandle(void* target) {
    std::lock_guard<std::mutex> l(g_reg_mutex);
    for (auto& s : g_slots)
        if (s.state == HS_INSTALLED && s.target == target) return &s;
    return nullptr;
}

int RegistrySnapshot(HookHandle* out, int max) {
    std::lock_guard<std::mutex> l(g_reg_mutex);
    int n = 0;
    for (auto& s : g_slots)
        if (s.state == HS_INSTALLED && n < max) out[n++] = s;
    return n;
}

int RegistryCount() {
    std::lock_guard<std::mutex> l(g_reg_mutex);
    int n = 0;
    for (auto& s : g_slots) if (s.state == HS_INSTALLED) n++;
    return n;
}

// ─── Session 71 — Feature 6.2: per-guest tags ─────────────────────────

void RegistrySetPendingTag(const char* tag) {
    if (tag && tag[0]) {
        size_t n = strlen(tag);
        if (n >= (size_t)kTagLen) n = kTagLen - 1;
        memcpy(g_pendingTag, tag, n);
        g_pendingTag[n] = 0;
    } else {
        g_pendingTag[0] = 0;
    }
}

// Wrapper that guarantees the pending tag is cleared on both success
// and failure paths. Any new tagged-install caller MUST use this rather
// than RegistrySetPendingTag directly, otherwise a failed install leaves
// the pending tag live across the next install on the same thread.
//
// HookGotEntryEx is a C-style function returning HookResult — it does
// not throw — so the direct set/call/clear form is functionally correct.
// If a future refactor introduces exception-throwing intermediate code,
// wrap this in an RAII guard (ScopedPendingTag).
HookResult HookGotEntryExTagged(void** got_addr, void* replacement, void** orig,
                                const char* lib, const char* sym,
                                const char* tag, HookHandle** out) {
    RegistrySetPendingTag(tag);
    HookResult r = HookGotEntryEx(got_addr, replacement, orig, lib, sym, out);
    RegistrySetPendingTag(nullptr);
    return r;
}

int RegistryCountByTag(const char* tag) {
    if (!tag) tag = "";
    std::lock_guard<std::mutex> l(g_reg_mutex);
    int n = 0;
    for (auto& s : g_slots) {
        if (s.state == HS_INSTALLED && strcmp(s.tag, tag) == 0) n++;
    }
    return n;
}

int RegistrySnapshotByTag(const char* tag, HookHandle* out, int max) {
    if (!out || max <= 0) return 0;
    if (!tag) tag = "";
    std::lock_guard<std::mutex> l(g_reg_mutex);
    int n = 0;
    for (auto& s : g_slots) {
        if (s.state == HS_INSTALLED && strcmp(s.tag, tag) == 0 && n < max) {
            out[n++] = s;
        }
    }
    return n;
}

int RegistryUninstallByTag(const char* tag, int* out_failures) {
    if (out_failures) *out_failures = 0;
    if (!tag) tag = "";

    // Capture-then-unhook. NEVER hold g_reg_mutex across the unhook loop —
    // both UnhookGotEntry and UninstallInlineHook call RegistryFree, which
    // re-acquires g_reg_mutex; a lock_guard spanning the loop would self-
    // deadlock on the first iteration.
    //
    // Capturing (target, type) tuples rather than full HookHandle copies
    // avoids a ~114 KB stack array (HookHandle is ~224 bytes post-tag,
    // kMaxHooks=512). The two fields are all the unhook loop needs.
    struct UnhookTarget { void* target; HookType type; };
    std::vector<UnhookTarget> targets;
    {
        std::lock_guard<std::mutex> l(g_reg_mutex);
        for (auto& s : g_slots) {
            if (s.state == HS_INSTALLED && strcmp(s.tag, tag) == 0) {
                targets.push_back({ s.target, s.type });
            }
        }
    }

    int unhooked = 0;
    int failures = 0;
    for (auto& t : targets) {
        HookResult r = HOOK_INTERNAL_ERROR;
        if (t.type == HT_GOT) {
            r = UnhookGotEntry(t.target);
        } else if (t.type == HT_INLINE) {
            r = UninstallInlineHook(t.target);
        }
        if (r == HOOK_OK) {
            unhooked++;
        } else if (r != HOOK_NOT_HOOKED) {
            failures++;
        }
        // HOOK_NOT_HOOKED is benign — the handle may already have been
        // removed by a concurrent caller. Count it as neither success
        // nor failure.
    }
    if (out_failures) *out_failures = failures;
    return unhooked;
}

int RegistryListTags(char* out, int max_bytes) {
    if (!out || max_bytes <= 0) return -1;
    out[0] = 0;
    int written = 0;
    int distinct = 0;
    // Fixed dedupe buffer. kMaxHooks = 512 distinct tags is the theoretical
    // maximum, but in practice we expect <10. Cap the dedupe table at 32 —
    // if more than 32 distinct tags are ever in flight, that's a design
    // smell, and truncating the LIST is safer than running off the buffer.
    char seen[32][kTagLen];
    int  seenCount = 0;

    std::lock_guard<std::mutex> l(g_reg_mutex);
    for (auto& s : g_slots) {
        if (s.state != HS_INSTALLED) continue;
        bool already = false;
        for (int k = 0; k < seenCount; k++) {
            if (strcmp(seen[k], s.tag) == 0) { already = true; break; }
        }
        if (already) continue;
        if (seenCount < 32) {
            memcpy(seen[seenCount], s.tag, kTagLen);
            seen[seenCount][kTagLen - 1] = 0;
            seenCount++;
        }
        size_t need = strlen(s.tag) + 1;
        if ((size_t)written + need >= (size_t)max_bytes) break;
        memcpy(out + written, s.tag, strlen(s.tag));
        written += (int)strlen(s.tag);
        out[written++] = '\n';
        distinct++;
    }
    if (written > 0 && written < max_bytes) out[written] = 0;
    return distinct;
}