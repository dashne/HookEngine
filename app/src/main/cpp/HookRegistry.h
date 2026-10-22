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
};

HookHandle* RegistryAlloc();
void        RegistryFree(HookHandle* h);
HookHandle* FindHandle(void* target);
int         RegistrySnapshot(HookHandle* out, int max);
int         RegistryCount();

extern "C" int g_hook_fail_after_n;