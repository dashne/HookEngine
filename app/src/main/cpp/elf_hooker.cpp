#include <link.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/mman.h>
#include <android/log.h>

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

struct FindSymbolData {
    const char *TargetLibName;
    const char *TargetSymbolName;
    void **GotAddress;
};

static int FindSymbolCallback(struct dl_phdr_info *info, size_t size, void *data) {
    auto *findData = (FindSymbolData *) data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, findData->TargetLibName)) return 0;

    ElfW(Dyn) *dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (ElfW(Dyn) *) (info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return 1;

    const char *strtab = nullptr;
    ElfW(Sym) *symtab = nullptr;
    ElfW(Rela) *jmprel = nullptr;
    size_t pltrelsz = 0;
    ElfW(Rela) *relaDyn = nullptr;
    size_t relaDynSz = 0;

    for (ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB:   strtab    = (const char *)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_SYMTAB:   symtab    = (ElfW(Sym) *)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_JMPREL:   jmprel    = (ElfW(Rela) *)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_PLTRELSZ: pltrelsz  = d->d_un.d_val; break;
            case DT_RELA:     relaDyn   = (ElfW(Rela) *)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_RELASZ:   relaDynSz = d->d_un.d_val; break;
        }
    }
    if (!strtab || !symtab) return 1;

    auto processRela = [&](ElfW(Rela) *relArr, size_t arrSz) -> bool {
        if (!relArr) return false;
        size_t count = arrSz / sizeof(ElfW(Rela));
        for (size_t i = 0; i < count; i++) {
            ElfW(Rela) *rel = &relArr[i];
            size_t symIdx = ELF64_R_SYM(rel->r_info);
            ElfW(Sym) *sym = &symtab[symIdx];
            const char *symName = strtab + sym->st_name;
            if (symName && strcmp(symName, findData->TargetSymbolName) == 0) {
                findData->GotAddress = (void **) (info->dlpi_addr + rel->r_offset);
                return true;
            }
        }
        return false;
    };

    if (processRela(jmprel, pltrelsz)) return 1;
    if (processRela(relaDyn, relaDynSz)) return 1;
    return 1;
}

extern "C" void **GetGotAddress(const char *libName, const char *symbolName) {
    FindSymbolData data{};
    data.TargetLibName = libName;
    data.TargetSymbolName = symbolName;
    data.GotAddress = nullptr;
    dl_iterate_phdr(FindSymbolCallback, &data);
    return data.GotAddress;
}

// IDEMPOTENT + CRITICAL FIX: restore page as RX, never R-only.
// .got.plt shares pages with .plt on ARM64 Android. If we restore to R only,
// every subsequent call through the .plt stub crashes with SIGSEGV_ACCERR.
extern "C" bool HookGotEntry(void **gotAddress, void *newFunction, void **originalFunction) {
    if (!gotAddress) return false;
    if (*gotAddress == newFunction) return false;
    if (originalFunction && !*originalFunction) *originalFunction = *gotAddress;

    long pageSize = sysconf(_SC_PAGESIZE);
    uintptr_t pageStart = (uintptr_t) gotAddress & ~(pageSize - 1);
    if (mprotect((void *) pageStart, pageSize, PROT_READ | PROT_WRITE) != 0) {
        LOGE("mprotect RW failed on GOT %p: errno=%d", gotAddress, errno);
        return false;
    }
    *gotAddress = newFunction;
    __builtin___clear_cache((char*)pageStart, (char*)(pageStart + pageSize));
    // .got.plt is data — restore to R (its original state). NOT RX.
    if (mprotect((void *) pageStart, pageSize, PROT_READ) != 0) {
        LOGE("mprotect R failed on GOT %p: errno=%d", gotAddress, errno);
        return false;
    }
    return true;
}