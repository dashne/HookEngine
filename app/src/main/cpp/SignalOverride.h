#pragma once
#include <signal.h>
#include "HookRegistry.h"

// Feature 3.6 — Signal handling override.
//
// Installs a sigaction handler for a chosen signal number. When the signal
// fires, the handler runs in signal-safe context (no malloc, no locks, no
// JNI, no unbounded work). Its return value determines what happens next:
//
//   SIGOVR_PASS_THROUGH  chain to the previous sa_sigaction (or SIG_DFL /
//                        SIG_IGN). If the previous action was SIG_DFL, this
//                        restores default and re-raises — typically terminates.
//   SIGOVR_SUPPRESS      swallow. Do NOT forward to any previous handler.
//                        For most signals this means the process continues
//                        as if the signal never fired. Not useful for
//                        unrecoverable signals like a real SIGSEGV fault,
//                        but perfectly valid for user-raiseable signals.
//
// Per-slot independence: multiple signal numbers can be overridden at once.
// Each slot has its own handler, fire count, and saved previous action.
//
// Constraints:
//   - Cannot install on SIGKILL or SIGSTOP (kernel refuses).
//   - Max one active override per signal number.
//   - The user handler runs in signal context — it must be async-signal-safe.
//     Reading siginfo fields and ucontext->uc_mcontext is safe. Writing to
//     atomics is safe. Anything else is not.

typedef enum {
    SIGOVR_PASS_THROUGH = 0,
    SIGOVR_SUPPRESS     = 1,
} SignalOverrideAction;

typedef SignalOverrideAction (*SignalOverrideHandler)(
    int signo, siginfo_t* info, void* ucv, void* user_ctx);

extern "C" HookResult SignalOverrideInstall(int signo, SignalOverrideHandler handler, void* user_ctx);
extern "C" HookResult SignalOverrideUninstall(int signo);
extern "C" int        SignalOverrideCount();
extern "C" int        SignalOverrideFireCount(int signo);
extern "C" void       SignalOverrideResetAll();