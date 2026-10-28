#pragma once
#include <cstdint>
#include <cstddef>

enum HookResult : int {
    HOOK_OK = 0,
    HOOK_UNSUPPORTED_API,
    HOOK_UNSUPPORTED_ABI,
    HOOK_UNSUPPORTED_INSN,
    HOOK_SYMBOL_MISSING,
    HOOK_MPROTECT_FAILED,
    HOOK_ALLOC_FAILED,
    HOOK_ROLLBACK_FAILED,
    HOOK_ALREADY_HOOKED,
    HOOK_NOT_HOOKED,
    HOOK_INTERNAL_ERROR,
    HOOK_TARGET_INVALID,      // Session 42: unmapped / misaligned / non-exec target or replacement
};

enum HookType { HT_INLINE = 0, HT_GOT = 1 };
enum HookState { HS_FREE = 0, HS_INSTALLING, HS_INSTALLED, HS_REMOVING };

struct HookHandle {
    HookType  type        = HT_INLINE;
    void*     target      = nullptr;
    void*     replacement = nullptr;
    void*     saved       = nullptr;
    void*     trampoline  = nullptr;
    size_t    tramp_size  = 0;
    void*     relay       = nullptr;
    size_t    relay_size  = 0;
    int       orig_prot   = 0;
    size_t    page_size   = 0;
    int       state       = HS_FREE;
    char      name[96]    = {0};
    // Session 71 — Feature 6.2: per-guest tag. Set atomically with the
    // HS_FREE -> HS_INSTALLING transition inside RegistryAlloc (via
    // RegistrySetPendingTag + a thread-local slot). Empty string = untagged.
    // Grouping key for uninstall scope ONLY — never gates install
    // admissibility (FindHandle stays tag-blind).
    char      tag[48]     = {0};
};

HookHandle* RegistryAlloc();
void        RegistryFree(HookHandle* h);
HookHandle* FindHandle(void* target);
int         RegistrySnapshot(HookHandle* out, int max);
int         RegistryCount();

// ─── Session 71 — Feature 6.2: per-guest tags ─────────────────────────
//
// Tag semantics:
//   - "" (empty) is a valid tag; it identifies "untagged" handles.
//   - RegistryCountByTag("") counts untagged installed handles.
//   - RegistryUninstallByTag("") uninstalls untagged installed handles.
//   - Two handles installed on the same target: the second install still
//     returns HOOK_ALREADY_HOOKED regardless of tag (FindHandle unchanged).
//
// Atomicity: a handle's tag is fixed at the moment it transitions from
// HS_INSTALLING to HS_INSTALLED. RegistryUninstallByTag only ever sees
// HS_INSTALLED handles, so it can never observe a half-tagged handle.

// Sets the tag that the NEXT RegistryAlloc() on this thread will consume.
// Prefer HookGotEntryExTagged() — it wraps the set/clear dance and
// guarantees the pending tag is cleared on both success and failure.
void RegistrySetPendingTag(const char* tag);

// Tagged wrapper around HookGotEntryEx. Sets the pending tag, invokes
// the extern HookGotEntryEx, clears the pending tag on return (always).
// The tag is consumed by the RegistryAlloc inside HookGotEntryEx.
extern "C" HookResult HookGotEntryEx(void**, void*, void**, const char*, const char*, HookHandle**);
HookResult HookGotEntryExTagged(void** got_addr, void* replacement, void** orig,
                                const char* lib, const char* sym,
                                const char* tag, HookHandle** out);

// Number of HS_INSTALLED handles whose tag matches exactly.
int  RegistryCountByTag(const char* tag);
// Snapshot of HS_INSTALLED handles whose tag matches exactly, capped at max.
int  RegistrySnapshotByTag(const char* tag, HookHandle* out, int max);
// Uninstall every HS_INSTALLED handle whose tag matches exactly.
// Uses the RegistrySnapshot-then-unhook shape (never holds g_reg_mutex
// across an unhook — both UnhookGotEntry and UninstallInlineHook call
// RegistryFree which re-acquires the mutex). Returns the number of hooks
// successfully unhooked. out_failures (may be null) receives the count
// of unhook attempts that returned anything other than HOOK_OK or
// HOOK_NOT_HOOKED.
int  RegistryUninstallByTag(const char* tag, int* out_failures);
// Writes newline-separated, deduplicated tags of HS_INSTALLED handles
// into `out`. Never writes more than max_bytes - 1 bytes plus a NUL.
// Returns the number of distinct tags written, or -1 if out is null.
int  RegistryListTags(char* out, int max_bytes);

extern "C" int g_hook_fail_after_n;