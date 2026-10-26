#include "MapsEnum.h"
#include <android/log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#define TAG "DualCore"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGDBG(...) __android_log_print(ANDROID_LOG_DEBUG, "MapsDebug", __VA_ARGS__)

// Parse one /proc/self/maps line.
//
// Format:  <start>-<end> <perms> <offset> <dev> <inode>   <pathname?>
//
// Manual parse via strtoul for each numeric field. sscanf-based versions
// (both "%lu%n" and "%lu %255[^\n]") failed on-device in Sessions 50 v1/v2
// to populate the pathname field, even though the first 7 fields parsed
// correctly. Manual parse sidesteps the sscanf/scanset semantics entirely.
static bool ParseMapsLine(const char* line, MemMapEntry* out) {
    const char* eol = line + strcspn(line, "\r\n");
    const char* p = line;
    char* endp = nullptr;

    // 1. start (hex)
    unsigned long start_u = strtoul(p, &endp, 16);
    if (endp == p || endp >= eol || *endp != '-') return false;
    p = endp + 1;

    // 2. end (hex)
    unsigned long end_u = strtoul(p, &endp, 16);
    if (endp == p || endp >= eol || *endp != ' ') return false;
    p = endp + 1;

    // 3. perms (4 chars: r/w/x/(p|s))
    if (p + 4 > eol) return false;
    char p0 = p[0], p1 = p[1], p2 = p[2], p3 = p[3];
    if (p3 != 'p' && p3 != 's') return false;
    p += 4;
    if (p >= eol || *p != ' ') return false;
    p += 1;

    // 4. offset (hex)
    unsigned long offset_u = strtoul(p, &endp, 16);
    if (endp == p || endp >= eol || *endp != ' ') return false;
    p = endp + 1;

    // 5. dev major:minor (hex:hex)
    unsigned int major = (unsigned int)strtoul(p, &endp, 16);
    if (endp == p || endp >= eol || *endp != ':') return false;
    p = endp + 1;
    unsigned int minor = (unsigned int)strtoul(p, &endp, 16);
    if (endp == p || endp >= eol || *endp != ' ') return false;
    p = endp + 1;

    // 6. inode (dec). May be last field if no pathname.
    unsigned long inode = strtoul(p, &endp, 10);
    if (endp == p || endp > eol) return false;
    p = endp;

    // 7. skip whitespace padding, copy remaining chars as path
    while (p < eol && (*p == ' ' || *p == '\t')) p++;
    size_t plen = (size_t)(eol - p);
    if (plen >= sizeof(out->path)) plen = sizeof(out->path) - 1;
    memcpy(out->path, p, plen);
    out->path[plen] = '\0';

    out->start      = (uintptr_t)start_u;
    out->end        = (uintptr_t)end_u;
    int prot = 0;
    if (p0 == 'r') prot |= 1;   // PROT_READ
    if (p1 == 'w') prot |= 2;   // PROT_WRITE
    if (p2 == 'x') prot |= 4;   // PROT_EXEC
    out->prot       = prot;
    out->offset     = (uint64_t)offset_u;
    out->dev_major  = major;
    out->dev_minor  = minor;
    out->inode      = (uint64_t)inode;
    return true;
}

extern "C" HookResult MemEnumMaps(MemMapEntry* out, size_t max, size_t* out_count) {
    if (!out || !out_count || max == 0) return HOOK_INTERNAL_ERROR;
    *out_count = 0;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        LOGE("MemEnumMaps: fopen failed");
        return HOOK_INTERNAL_ERROR;
    }

    char line[2048];
    size_t i = 0;
    while (i < max && fgets(line, sizeof(line), f)) {
        if (ParseMapsLine(line, &out[i])) i++;
    }
    fclose(f);
    *out_count = i;
    return HOOK_OK;
}

extern "C" HookResult MemEnumMapsFilter(int prot_mask,
                                        MemMapEntry* out, size_t max,
                                        size_t* out_count) {
    if (!out || !out_count || max == 0) return HOOK_INTERNAL_ERROR;
    *out_count = 0;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        LOGE("MemEnumMapsFilter: fopen failed");
        return HOOK_INTERNAL_ERROR;
    }

    char line[2048];
    size_t i = 0;
    while (i < max && fgets(line, sizeof(line), f)) {
        MemMapEntry e = {};
        if (!ParseMapsLine(line, &e)) continue;
        if (prot_mask != 0 && (e.prot & prot_mask) != prot_mask) continue;
        out[i++] = e;
    }
    fclose(f);
    *out_count = i;
    return HOOK_OK;
}

extern "C" HookResult MemFindRegionContaining(uintptr_t addr, MemMapEntry* out) {
    if (!out) return HOOK_INTERNAL_ERROR;

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        LOGE("MemFindRegionContaining: fopen failed");
        return HOOK_INTERNAL_ERROR;
    }

    char line[2048];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        MemMapEntry e = {};
        if (!ParseMapsLine(line, &e)) continue;
        if (e.start <= addr && addr < e.end) {
            *out = e;
            found = true;
            break;
        }
    }
    fclose(f);
    return found ? HOOK_OK : HOOK_TARGET_INVALID;
}

extern "C" HookResult MemEnumDebugDump(char* out, size_t out_size, size_t* out_used) {
    if (!out || out_size == 0 || !out_used) return HOOK_INTERNAL_ERROR;
    *out_used = 0;
    out[0] = '\0';

    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) {
        int w = snprintf(out, out_size, "fopen failed\n");
        *out_used = (w > 0) ? (size_t)w : 0;
        return HOOK_INTERNAL_ERROR;
    }

    char line[2048];
    size_t pos = 0;
    int n = 0;
    while (n < 3 && fgets(line, sizeof(line), f)) {
        size_t raw_len = strcspn(line, "\r\n");
        int w = snprintf(out + pos, out_size - pos,
                         "RAW%d(len=%zu):[%.*s]\n", n, raw_len,
                         (int)raw_len, line);
        if (w > 0) pos += (size_t)w;

        MemMapEntry e = {};
        bool ok = ParseMapsLine(line, &e);
        w = snprintf(out + pos, out_size - pos,
                     "PARSE%d: ok=%d s=0x%lx e=0x%lx pr=%d off=0x%lx "
                     "dev=%x:%x ino=%lu path_len=%zu path=[%s]\n",
                     n, ok ? 1 : 0,
                     (unsigned long)e.start, (unsigned long)e.end,
                     e.prot, (unsigned long)e.offset,
                     e.dev_major, e.dev_minor, (unsigned long)e.inode,
                     strlen(e.path), e.path);
        if (w > 0) pos += (size_t)w;
        LOGDBG("%.*s", (int)(out_size - pos - w), out + pos - w);
        n++;
    }
    fclose(f);
    *out_used = pos;
    return HOOK_OK;
}