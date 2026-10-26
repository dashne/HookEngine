#include "MemoryOps.h"
#include <sys/mman.h>
#include <unistd.h>
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <cerrno>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── Page protection lookup ──────────────────────────────────────────
extern "C" bool MemGetPageProt(uintptr_t page, int* out_prot) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[512];
    bool ok = false;
    while (fgets(line, sizeof(line), f)) {
        unsigned long s, e;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3) {
            if (s <= page && page < e) {
                int p = 0;
                if (perms[0] == 'r') p |= PROT_READ;
                if (perms[1] == 'w') p |= PROT_WRITE;
                if (perms[2] == 'x') p |= PROT_EXEC;
                *out_prot = p;
                ok = true;
                break;
            }
        }
    }
    fclose(f);
    return ok;
}

// ─── Internal: validate a range ──────────────────────────────────────
//
// Walks each page in [addr, addr+len) once. For each page, records orig
// prot into out_prots (if non-null). If any page is unmapped or missing
// `required_perm`, returns HOOK_TARGET_INVALID.
//
// If out_n_pages is non-null, fills it with the number of pages checked.
static HookResult ValidateRange(uintptr_t addr, size_t len, int required_perm,
                                uintptr_t* out_pages, int* out_prots,
                                int* out_n_pages) {
    if (len == 0) return HOOK_INTERNAL_ERROR;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t first = addr & ~((uintptr_t)ps - 1);
    uintptr_t last  = (addr + len - 1) & ~((uintptr_t)ps - 1);

    int idx = 0;
    for (uintptr_t p = first; p <= last; p += ps) {
        if (idx >= MEMOPS_MAX_PAGES) return HOOK_INTERNAL_ERROR;
        int prot = 0;
        if (!MemGetPageProt(p, &prot)) {
            LOGE("MemOps: unmapped page %p in range [%p,+%zu)",
                 (void*)p, (void*)addr, len);
            return HOOK_TARGET_INVALID;
        }
        if ((prot & required_perm) != required_perm) {
            LOGE("MemOps: page %p has prot 0x%x, needs 0x%x",
                 (void*)p, prot, required_perm);
            return HOOK_TARGET_INVALID;
        }
        if (out_pages)  out_pages[idx]  = p;
        if (out_prots)  out_prots[idx]  = prot;
        idx++;
    }
    if (out_n_pages) *out_n_pages = idx;
    return HOOK_OK;
}

// ─── MemRead ─────────────────────────────────────────────────────────
extern "C" HookResult MemRead(uintptr_t addr, void* out, size_t len) {
    if (!addr || !out) return HOOK_INTERNAL_ERROR;
    HookResult r = ValidateRange(addr, len, PROT_READ, nullptr, nullptr, nullptr);
    if (r != HOOK_OK) return r;
    memcpy(out, (void*)addr, len);
    return HOOK_OK;
}

// ─── MemWrite ────────────────────────────────────────────────────────
extern "C" HookResult MemWrite(uintptr_t addr, const void* data, size_t len) {
    if (!addr || !data) return HOOK_INTERNAL_ERROR;

    uintptr_t pages[MEMOPS_MAX_PAGES];
    int orig_prots[MEMOPS_MAX_PAGES];
    int n_pages = 0;

    // Step 1: validate every page is mapped; record orig prot.
    // Note: we do NOT require PROT_WRITE here — mprotect will add it.
    // But we DO require the page to be present. If a page is present
    // but has no PROT_READ, mprotect can still add PROT_WRITE on some
    // kernels — but as a sanity check we require at least readable.
    HookResult r = ValidateRange(addr, len, PROT_READ,
                                 pages, orig_prots, &n_pages);
    if (r != HOOK_OK) return r;

    // Step 2: mprotect each page to orig | PROT_WRITE.
    int modified = 0;
    long ps = sysconf(_SC_PAGESIZE);
    for (int i = 0; i < n_pages; i++) {
        if (mprotect((void*)pages[i], (size_t)ps,
                     orig_prots[i] | PROT_WRITE) != 0) {
            LOGE("MemWrite: mprotect RW failed on %p errno=%d",
                 (void*)pages[i], errno);
            for (int j = 0; j < modified; j++) {
                mprotect((void*)pages[j], (size_t)ps, orig_prots[j]);
            }
            return HOOK_MPROTECT_FAILED;
        }
        modified++;
    }

    // Step 3: write.
    memcpy((void*)addr, data, len);
    __builtin___clear_cache((char*)addr, (char*)addr + len);

    // Step 4: restore orig prot for each page.
    int restore_failed = 0;
    for (int i = 0; i < n_pages; i++) {
        if (mprotect((void*)pages[i], (size_t)ps, orig_prots[i]) != 0) {
            LOGE("MemWrite: restore prot failed on %p errno=%d",
                 (void*)pages[i], errno);
            restore_failed = 1;
        }
    }

    return restore_failed ? HOOK_MPROTECT_FAILED : HOOK_OK;
}

// ─── MemProbe* ───────────────────────────────────────────────────────
extern "C" HookResult MemProbeReadable(uintptr_t addr, size_t len) {
    if (!addr) return HOOK_INTERNAL_ERROR;
    return ValidateRange(addr, len, PROT_READ, nullptr, nullptr, nullptr);
}

extern "C" HookResult MemProbeWritable(uintptr_t addr, size_t len) {
    if (!addr) return HOOK_INTERNAL_ERROR;
    return ValidateRange(addr, len, PROT_WRITE, nullptr, nullptr, nullptr);
}

// ─── MemScan ─────────────────────────────────────────────────────────
extern "C" HookResult MemScan(uintptr_t start, size_t len,
                              const uint8_t* pattern, const uint8_t* mask,
                              size_t plen,
                              uintptr_t* out_addrs, size_t max_out,
                              size_t* out_count) {
    if (!start || !pattern || !out_count || plen == 0) {
        return HOOK_INTERNAL_ERROR;
    }
    *out_count = 0;

    if (len < plen) return HOOK_OK;   // nothing to scan, valid empty result

    HookResult r = ValidateRange(start, len, PROT_READ, nullptr, nullptr, nullptr);
    if (r != HOOK_OK) return r;

    const uint8_t* hay = (const uint8_t*)start;
    size_t found = 0;
    for (size_t i = 0; i + plen <= len; i++) {
        bool match = true;
        for (size_t j = 0; j < plen; j++) {
            uint8_t m = mask ? mask[j] : 0xFF;
            if ((hay[i + j] & m) != (pattern[j] & m)) { match = false; break; }
        }
        if (match) {
            if (out_addrs && found < max_out) out_addrs[found] = start + i;
            found++;
        }
    }
    *out_count = found;
    return HOOK_OK;
}