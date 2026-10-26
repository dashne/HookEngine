#pragma once
#include <cstdint>
#include "HookRegistry.h"

// Feature 1.6 — SVC instruction hooking.
//
// Intercepts syscalls below the libc wrapper layer by patching individual
// `svc #0` instructions to `brk #0x4242` and dispatching from a SIGTRAP
// handler. Handles syscalls at the SVC boundary, not at libc wrappers —
// syscalls that bypass libc entirely are still intercepted if they route
// through a patched SVC site.
//
// Handler signature:
//   uint64_t (*)(uint64_t nr,
//                uint64_t a0, uint64_t a1, uint64_t a2,
//                uint64_t a3, uint64_t a4, uint64_t a5,
//                uint64_t orig_ret)
//
//   nr        — syscall number (x8 at the SVC).
//   a0..a5    — first six syscall args (x0..x5 at the SVC).
//   orig_ret  — the syscall's real return value (already issued by the
//               handler on the caller's behalf).
//   returns   — the caller-visible return value (x0).
//
// Constraints:
//   - Only `svc #0` (0xD4000001) sites are hookable. `svc #1`/`#2` etc.
//     have different kernel entry semantics; refused with
//     HOOK_UNSUPPORTED_INSN.
//   - Site must be 4-byte aligned in an executable page.
//   - The SIGTRAP handler is installed once (SvcHookInit) and stays
//     installed for the process lifetime; it chains to any previous
//     handler for non-magic BRKs.
//   - The user handler runs in signal context. It must be async-signal-safe:
//     no malloc, no stdio, no lock acquisition, no unbounded work. Reading
//     globals, arithmetic, atomics (arm64 lock-free for <=64 bits), and
//     raw syscalls via `syscall()` from libc are safe.
//   - Max 16 concurrent SVC sites.
//
// expected_nr semantics:
//   -1  → wildcard. Every SVC at this site dispatches to `handler` regardless
//         of x8. Useful when the site's syscall number is data-dependent.
//   >=0 → strict. Only dispatches when x8 == expected_nr; other nr values
//         are raw-reissued (syscall completes normally, handler does not
//         fire). This is the normal case — SVC sites in compiled code
//         load x8 with a constant.
//
// Uninstall safety: SvcHookDetach is safe from any thread, including one
// hitting the same site mid-flight. Concurrent detach calls are serialized
// by an internal mutex but the detach path is NOT safe to call from inside
// a signal handler (that mutex could deadlock).

typedef uint64_t (*SyscallHandlerFn)(uint64_t nr,
                                     uint64_t a0, uint64_t a1, uint64_t a2,
                                     uint64_t a3, uint64_t a4, uint64_t a5,
                                     uint64_t orig_ret);

extern "C" HookResult SvcHookInit();
extern "C" HookResult SvcHookAttach(void* svc_addr, int expected_nr,
                                    SyscallHandlerFn handler);
extern "C" HookResult SvcHookDetach(void* svc_addr);
extern "C" int        SvcHookCount();

// Diagnostic — count of `svc #0` instructions in the first executable
// PT_LOAD segment of any loaded library whose path contains `lib_substr`.
// Used by SvcHookTest to prove discovery works, and by future sessions
// that want to scan libc's SVC sites without patching them.
extern "C" int SvcHookScanLib(const char* lib_substr);

extern "C" void SvcHookResetAll();