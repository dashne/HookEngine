#pragma once
#include <cstdint>
#include "HookRegistry.h"

// Feature 2.7 — Register state inspection at hook site.
//
// AttachRegInspect installs an inline hook whose stub saves ALL ARM64 GP
// registers (x0..x30), SP, NZCV, and all 32 Q (SIMD) registers to a
// per-slot snapshot buffer at every call. Reading is then safe from any
// C++ context via RegInspectRead(target, out).
//
// Two modes:
//
//   REGINSPECT_MODE_PROBE (0)
//       Capture the register state, then return immediately WITHOUT
//       executing the target body. Caller sees x0 = entry arg0 (the
//       saved x0). Useful for one-shot inspection: "what args is this
//       function being called with?" Body side effects are skipped.
//
//   REGINSPECT_MODE_PASSTHRU (1)
//       Capture the register state, then execute the body normally
//       (via a shadow trampoline). Caller sees the body's real return
//       value. Side effects preserved. This is the non-invasive mode.
//
// Snapshot layout (see RegInspectSnapshot):
//   x[0..30]    — GP registers x0..x30 as they were at target entry.
//                 x[30] is the caller's return address (LR at entry).
//   x[31]       — sp_orig (SP as it was at target entry, BEFORE our
//                 stub's own sub sp, sp, #784).
//   nzcv        — raw `mrs nzcv` value. Per ARM ARM (DDI 0487 C5.2.53),
//                 the N/Z/C/V flags are in bits 31:28; bits 27:0 are
//                 zero; bits 63:32 are zero. To read the flag field as
//                 a 4-bit integer use (nzcv >> 28) & 0xF. The capture
//                 keeps the raw register value so inspection is faithful
//                 to what the CPU actually reports.
//   pc          — target address (filled in by dispatcher, not captured).
//   slot_id     — internal slot index (diagnostic only).
//   q[0..63]    — 32 Q registers stored as low/high u64 pairs.
//                 q[2i] = Q_i low 64 bits, q[2i+1] = Q_i high 64 bits.
//
// Known limitations:
//   - x16 and x17 are clobbered by the InstallInlineHookEx relay
//     (`ldr x17, [pc, #8]; br x17`). The captured x17 equals the stub
//     address, not the caller's x17. x16 is accurate. Both are IP0/IP1
//     intra-procedure scratch — no caller can rely on them across a call.
//   - A reader racing with an in-flight writer may observe a torn
//     snapshot (memcpy of x[], q[] is not atomic). For test purposes
//     there is no concurrency (the calling thread reads its own capture).
//   - Probe mode changes the caller-visible return value from body's
//     return to arg0. Use PASSTHRU unless you specifically want to skip.
//
// Max 16 concurrent reginspect hooks.

#define REGINSPECT_MODE_PROBE     0
#define REGINSPECT_MODE_PASSTHRU  1

typedef struct {
    uint64_t x[32];     // x0..x30, then sp_orig
    uint64_t nzcv;      // raw mrs nzcv value (flags in bits 31:28)
    uint64_t pc;        // target address
    uint64_t slot_id;   // internal slot index
    uint64_t _pad;      // align q[] to 16
    uint64_t q[64];     // q0..q31, low/high u64 pairs
} RegInspectSnapshot;

extern "C" HookResult AttachRegInspect(void* target, int mode, void** outShadowTramp);
extern "C" HookResult DetachRegInspect(void* target);
extern "C" HookResult RegInspectRead(void* target, RegInspectSnapshot* out);
extern "C" int        RegInspectCount();
extern "C" void       RegInspectResetAll();