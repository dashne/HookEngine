#include "Backtrace.h"
#include <unistd.h>
#include <sys/mman.h>
#include <cstring>
#include <cstdio>
#include <dlfcn.h>
#include <android/log.h>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

static bool ReadPageProt(uintptr_t page, int* out) {
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
                *out = p;
                ok = true;
                break;
            }
        }
    }
    fclose(f);
    return ok;
}

static bool PcIsExecutable(uint64_t pc) {
    if (pc < 0x1000) return false;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t page = (uintptr_t)pc & ~(uintptr_t)(ps - 1);
    int prot = 0;
    if (!ReadPageProt(page, &prot)) return false;
    return (prot & PROT_EXEC) != 0;
}

static bool FpReadable16(uint64_t fp) {
    if (fp < 0x1000) return false;
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t p1 = (uintptr_t)fp & ~(uintptr_t)(ps - 1);
    uintptr_t p2 = (uintptr_t)(fp + 8) & ~(uintptr_t)(ps - 1);
    int prot = 0;
    if (!ReadPageProt(p1, &prot)) return false;
    if ((prot & PROT_READ) == 0) return false;
    if (p2 != p1) {
        int prot2 = 0;
        if (!ReadPageProt(p2, &prot2)) return false;
        if ((prot2 & PROT_READ) == 0) return false;
    }
    return true;
}

extern "C" int BacktraceCaptureFromContext(uint64_t fp, uint64_t pc,
                                           BtFrame* out, int max_frames) {
    if (!out || max_frames <= 0) return 0;
    if (max_frames > BT_MAX_FRAMES) max_frames = BT_MAX_FRAMES;

    int n = 0;
    uint64_t cur_pc = pc;
    uint64_t cur_fp = fp;
    uint64_t prev_fp = 0;

    while (n < max_frames && cur_pc != 0) {
        if (!PcIsExecutable(cur_pc)) break;

        out[n].pc = cur_pc;
        out[n].fp = cur_fp;
        n++;

        if (!FpReadable16(cur_fp)) break;
        if ((cur_fp & 0xF) != 0) break;

        uint64_t next_fp = *(uint64_t*)(uintptr_t)cur_fp;
        uint64_t saved_lr = *(uint64_t*)(uintptr_t)(cur_fp + 8);

        if (saved_lr == 0) break;
        if (next_fp == 0) break;
        if (next_fp <= cur_fp) break;   // cycle or downward move (impossible on ARM64)
        if (next_fp <= prev_fp) break;  // monotonicity guard

        prev_fp = cur_fp;
        cur_fp = next_fp;
        cur_pc = saved_lr;
    }
    return n;
}

extern "C" int BacktraceCaptureHere(BtFrame* out, int max_frames) {
    // __builtin_frame_address(0) forces the compiler to establish a frame
    // (stp x29, x30, [sp, #-16]!; mov x29, sp) so reading x29 is meaningful.
    void* my_fp = __builtin_frame_address(0);
    void* my_pc;
    __asm__ volatile("adr %0, ." : "=r"(my_pc));
    return BacktraceCaptureFromContext((uint64_t)(uintptr_t)my_fp,
                                       (uint64_t)(uintptr_t)my_pc,
                                       out, max_frames);
}

extern "C" int BacktraceFilterOutLib(BtFrame* frames, int n, const char* lib_substr) {
    if (!frames || n <= 0 || !lib_substr) return n;
    int w = 0;
    for (int i = 0; i < n; i++) {
        Dl_info info;
        bool skip = false;
        if (dladdr((void*)(uintptr_t)frames[i].pc, &info) && info.dli_fname) {
            if (strstr(info.dli_fname, lib_substr)) skip = true;
        }
        if (!skip) frames[w++] = frames[i];
    }
    return w;
}