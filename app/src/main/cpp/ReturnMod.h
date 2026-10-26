#pragma once
#include <cstdint>
#include "HookRegistry.h"

// Feature 1.9 — Return-value modification.
//
// AttachRetMod installs an inline hook on `target` such that after the
// target's body executes, the caller-visible return value (x0 on ARM64)
// is passed through `modifier` along with the entry-time x0..x7. The
// modifier returns the value the caller actually sees. The target body
// still executes fully — side effects are preserved.
//
// Contrast with Feature 1.7 (chaining): chaining observes; return value
// is preserved. Feature 1.9 is post-call rewriting.
//
// Contrast with Feature 1.3 (ArtMethod replace): replace swaps the whole
// body. Feature 1.9 preserves the body but post-processes the return.
//
// modifier signature: uint64_t (*)(const uint64_t* x0_x7, uint64_t orig_ret)
//   x0_x7    — read-only view of x0..x7 as they were at target entry.
//   orig_ret — the target's natural return value (x0 after the body).
//   returns  — the caller-visible return value (x0).
//
// Constraints:
//   - target must be 4-byte aligned and in an executable page.
//   - one modifier per target; a second AttachRetMod on the same target
//     returns HOOK_ALREADY_HOOKED.
//   - max 16 simultaneous retmods.
//   - modifier must not throw — the stub is naked asm and cannot unwind.
//   - target must not be an ArtMethod or fast-JNI native (use 1.3/1.4).
//   - out_trampoline (if non-null) is the shadow trampoline; valid only
//     until DetachRetMod is called.

typedef uint64_t (*ReturnModifierFn)(const uint64_t* x0_x7, uint64_t orig_ret);

extern "C" HookResult AttachRetMod(void* target, ReturnModifierFn modifier, void** outTrampoline);
extern "C" HookResult DetachRetMod(void* target);
extern "C" int        RetModCount();
extern "C" void       RetModResetAll();