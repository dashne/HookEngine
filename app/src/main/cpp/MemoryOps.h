#pragma once
#include <cstdint>
#include <cstddef>
#include "HookRegistry.h"

// Feature 2.1 / 2.2 / 2.3 — Memory read / write / scan primitives.
//
// All three operations validate addresses against /proc/self/maps at entry
// and refuse unmapped regions with HOOK_TARGET_INVALID. They do NOT keep
// any state — no slots, no dispatch tables. Every call is one-shot.
//
// MemRead:
//   Reads `len` bytes from `addr` into `out`. Every page in [addr, addr+len)
//   must be mapped with PROT_READ. If any page is unmapped or non-readable,
//   HOOK_TARGET_INVALID is returned and `out` is not modified.
//
// MemWrite:
//   Writes `len` bytes from `data` into `addr`. Every page in [addr, addr+len)
//   must be mapped. For each page, the original prot is recorded, PROT_WRITE
//   is added, the bytes are written, __builtin___clear_cache is invoked,
//   and the original prot is restored. If any mprotect fails, all pages
//   already modified are rolled back to their original prot before returning
//   HOOK_MPROTECT_FAILED. Rule 10 preserved on every path.
//
// MemScan:
//   Scans `len` bytes starting at `start` for a masked pattern. For each
//   candidate offset i, matches if for all j in [0, plen):
//       (haystack[i+j] & mask[j]) == (pattern[j] & mask[j])
//   `mask` may be null (treated as all-0xFF). `pattern` may not be null.
//   `plen` must be > 0. `len` must be >= plen (else returns count=0, OK).
//   Entire scan region must be mapped with PROT_READ. Found addresses are
//   written to out_addrs[0..min(count, max_out)) — if out_addrs is null
//   the scan still counts without filling. out_count is required.
//
// MemProbeReadable / MemProbeWritable:
//   Fast "is this region readable/writable" checks. Same validation as
//   MemRead/MemWrite but without touching memory. Return HOOK_OK if every
//   page is mapped with the requested perm, HOOK_TARGET_INVALID otherwise.
//
// MemGetPageProt:
//   Public query of the maps walk. Returns true and fills `out_prot` if
//   the page is mapped; false otherwise. `out_prot` follows PROT_* bits.
//
// Known limitations:
//   - MemWrite is NOT signal-safe. Do not call from signal handlers.
//   - MemRead on a mapped page that another thread unmaps mid-read can
//     SIGSEGV. Validation happens at entry only. Callers must ensure the
//     region is stable for the duration of the read.
//   - MemWrite refuses regions spanning more than 8 pages to bound the
//     internal mprotect rollback loop. Longer writes must be chunked.
//   - __builtin___clear_cache on a data-only page is a no-op; kept for
//     symmetry with inline-hook installs that write executable pages.

#define MEMOPS_MAX_PAGES 8

extern "C" HookResult MemRead(uintptr_t addr, void* out, size_t len);
extern "C" HookResult MemWrite(uintptr_t addr, const void* data, size_t len);
extern "C" HookResult MemProbeReadable(uintptr_t addr, size_t len);
extern "C" HookResult MemProbeWritable(uintptr_t addr, size_t len);
extern "C" HookResult MemScan(uintptr_t start, size_t len,
                              const uint8_t* pattern, const uint8_t* mask,
                              size_t plen,
                              uintptr_t* out_addrs, size_t max_out,
                              size_t* out_count);
extern "C" bool MemGetPageProt(uintptr_t page, int* out_prot);