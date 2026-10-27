#pragma once
#include <cstdint>

#define BT_MAX_FRAMES 32

struct BtFrame {
    uint64_t pc;
    uint64_t fp;
};

// Walk the ARM64 frame pointer chain starting from (fp, pc).
// For each emitted frame:
//   frame[n].pc = current pc
//   frame[n].fp = current fp
// Then read *(fp+0) → next fp, *(fp+8) → saved LR (becomes next pc).
//
// Safety:
//   - Every pc must be in a PROT_EXEC map region.
//   - Every fp must be 16-byte aligned (ARM64 ABI) and readable.
//   - fps must be strictly increasing (descending stack → parent at higher addr).
//   - Cycle detection: next fp must be > prev fp.
//   - Bounded to max_frames (capped at BT_MAX_FRAMES).
//
// Returns frame count. 0 on invalid input. Never crashes — all reads are
// guarded by a /proc/self/maps page-protection check first.
extern "C" int BacktraceCaptureFromContext(uint64_t fp, uint64_t pc,
                                           BtFrame* out, int max_frames);

// Convenience: capture the current function's frame and walk from there.
// Forces frame establishment via __builtin_frame_address(0). The returned
// frame[0] is inside this function; frame[1] is inside the caller.
extern "C" int BacktraceCaptureHere(BtFrame* out, int max_frames);

// Filter out frames whose PC resolves to a library whose path contains
// `lib_substr` (e.g. "libhookengine.so"). Returns the new count.
extern "C" int BacktraceFilterOutLib(BtFrame* frames, int n, const char* lib_substr);