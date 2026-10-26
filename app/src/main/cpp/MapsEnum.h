#pragma once
#include <cstdint>
#include <cstddef>
#include "HookRegistry.h"

// Feature 2.5 — Full maps enumeration.
//
// Exposes /proc/self/maps as a structured array. Each entry carries the
// full record: start, end, prot bits, offset, dev major/minor, inode,
// and pathname (may be empty for anonymous mappings). Path is truncated
// to 255 characters + null terminator.
//
// Cap: MAPS_MAX_ENTRIES entries per call. Refuses beyond that to bound
// internal memory use. A typical process has 100–400 entries; 8192 is a
// heavy margin, safe even for test processes with ML Kit + ART JIT + a
// booted Viber guest.

#define MAPS_MAX_ENTRIES 8192

// prot bits match <sys/mman.h> on Android: PROT_READ=1, PROT_WRITE=2,
// PROT_EXEC=4. Any other permission flags (PROT_NONE etc.) map to 0.
typedef struct {
    uintptr_t start;
    uintptr_t end;
    int       prot;
    uint64_t  offset;
    uint32_t  dev_major;
    uint32_t  dev_minor;
    uint64_t  inode;
    char      path[256];   // "" for anonymous; [heap] / [stack] / [anon:*] preserved
} MemMapEntry;

// Enumerates all mappings, writing up to `max` entries to `out`. Fills
// `out_count` with the actual number written. Returns HOOK_OK on success
// (out_count may be < max if the process has fewer regions). Returns
// HOOK_INTERNAL_ERROR if out/out_count is null, max==0, or the maps file
// cannot be opened.
extern "C" HookResult MemEnumMaps(MemMapEntry* out, size_t max, size_t* out_count);

// Enumerates mappings matching a permission filter. prot_mask == 0 means
// "no filter". Any nonzero mask requires (entry.prot & prot_mask) ==
// prot_mask — i.e., all requested bits must be set.
extern "C" HookResult MemEnumMapsFilter(int prot_mask,
                                        MemMapEntry* out, size_t max,
                                        size_t* out_count);

// Finds the single region containing `addr`. Returns HOOK_OK and fills
// `out` on match; HOOK_TARGET_INVALID if `addr` falls in a gap. O(n) walk
// over the maps file — no caching.
extern "C" HookResult MemFindRegionContaining(uintptr_t addr, MemMapEntry* out);

// Diagnostic — writes a human-readable dump of the first 3 maps lines
// (raw bytes + parse result) into `out`. Used by MapsEnumTest to surface
// parse behavior when a path-expectation test fails. Returns HOOK_OK on
// success, HOOK_INTERNAL_ERROR on error.
extern "C" HookResult MemEnumDebugDump(char* out, size_t out_size, size_t* out_used);