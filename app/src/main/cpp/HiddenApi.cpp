#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <elf.h>
#include <mutex>

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" bool InstallInlineHook(void*, void*, void**);

static uint32_t Fake_Allow() { return 0; }

// ─── GNU hash helper (unchanged) ───────────────────────────────────
static uint32_t GnuHashSymCount(uint32_t* h) {
    uint32_t nbuckets  = h[0];
    uint32_t symoffset = h[1];
    uint32_t bloomSize = h[2];
    if (!nbuckets || !bloomSize) return 0;
    uintptr_t p = (uintptr_t)h + 16;
    p += (uintptr_t)bloomSize * sizeof(uint64_t);
    uint32_t* buckets = (uint32_t*)p;
    p += (uintptr_t)nbuckets * sizeof(uint32_t);
    uint32_t* chain = (uint32_t*)p;
    uint32_t maxSym = 0;
    for (uint32_t i = 0; i < nbuckets; i++) if (buckets[i] > maxSym) maxSym = buckets[i];
    if (maxSym < symoffset) return symoffset;
    uint32_t idx = maxSym;
    uint32_t cap = symoffset + 1000000;
    while (idx < cap && !(chain[idx - symoffset] & 1)) idx++;
    return idx + 1;
}

struct FindCtx {
    const char* lib;
    const char* needle;
    void* addr;
    int scanned;
};

static int FindInLib(struct dl_phdr_info* info, size_t, void* data) {
    FindCtx* ctx = (FindCtx*)data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, ctx->lib)) return 0;
    ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return 1;
    const char* strtab = nullptr;
    ElfW(Sym)* symtab = nullptr;
    size_t strsz = 0;
    ElfW(Addr) gnuHash = 0, sysvHash = 0;
    for (ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB:   strtab   = (const char*)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_SYMTAB:   symtab   = (ElfW(Sym)*)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_STRSZ:    strsz    = d->d_un.d_val; break;
            case DT_HASH:     sysvHash = info->dlpi_addr + d->d_un.d_ptr; break;
            case DT_GNU_HASH: gnuHash  = info->dlpi_addr + d->d_un.d_ptr; break;
        }
    }
    if (!strtab || !symtab || !strsz) return 1;
    uint32_t count = gnuHash ? GnuHashSymCount((uint32_t*)gnuHash)
                             : (sysvHash ? ((uint32_t*)sysvHash)[1] : 0);
    if (count == 0 || count > 500000) return 1;
    for (uint32_t i = 1; i < count; i++) {
        ctx->scanned++;
        if (symtab[i].st_name == 0 || symtab[i].st_name >= strsz) continue;
        const char* name = strtab + symtab[i].st_name;
        if (strstr(name, ctx->needle) && symtab[i].st_value) {
            ctx->addr = (void*)(info->dlpi_addr + symtab[i].st_value);
            return 1;
        }
    }
    return 1;
}

// ─── NEW: on-disk .symtab scan of libart.so ──────────────────────────
//
// API 33+ removed GetMemberAction / ShouldDenyAccessToMember from libart's
// .dynsym (exported symbols). They live only in .symtab (local). Parse the
// file, compute runtime address via load_bias + sym.st_value - vaddr_base.
static std::mutex g_symtab_mutex;
static uint64_t   g_libart_runtime_base  = 0;
static uint64_t   g_libart_vaddr_base    = 0;
static bool       g_libart_base_resolved = false;

static int FindLibartRuntimeBase(struct dl_phdr_info* info, size_t, void*) {
    if (info->dlpi_name && strstr(info->dlpi_name, "libart.so")) {
        g_libart_runtime_base = info->dlpi_addr;
        return 1;
    }
    return 0;
}

static bool ResolveLibartBase() {
    if (g_libart_base_resolved) return g_libart_runtime_base != 0;
    g_libart_base_resolved = true;
    dl_iterate_phdr(FindLibartRuntimeBase, nullptr);
    return g_libart_runtime_base != 0;
}

static void* FindSymbolInElfFile(const char* path, const char* needle) {
    if (!ResolveLibartBase()) {
        LOGE("HiddenApi: libart runtime base unknown");
        return nullptr;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) { LOGE("HiddenApi: open(%s) failed errno=%d", path, errno); return nullptr; }
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) { close(fd); return nullptr; }
    void* map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { LOGE("HiddenApi: mmap failed"); return nullptr; }

    const uint8_t* base = (const uint8_t*)map;
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)base;
    if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA]  != ELFDATA2LSB ||
        ehdr->e_machine != EM_AARCH64) {
        LOGE("HiddenApi: file is not 64-bit LE AARCH64");
        munmap(map, (size_t)st.st_size);
        return nullptr;
    }

    uint64_t vaddr_base = UINT64_MAX;
    for (int i = 0; i < ehdr->e_phnum; i++) {
        const Elf64_Phdr* ph = (const Elf64_Phdr*)(base + ehdr->e_phoff + i * ehdr->e_phentsize);
        if (ph->p_type == PT_LOAD && ph->p_vaddr < vaddr_base) vaddr_base = ph->p_vaddr;
    }
    if (vaddr_base == UINT64_MAX) {
        LOGE("HiddenApi: no PT_LOAD in file");
        munmap(map, (size_t)st.st_size);
        return nullptr;
    }

    const Elf64_Shdr* shdrs = (const Elf64_Shdr*)(base + ehdr->e_shoff);
    const char* shstrtab = (const char*)(base + shdrs[ehdr->e_shstrndx].sh_offset);

    const Elf64_Sym* symtab = nullptr;
    const char* strtab = nullptr;
    size_t sym_count = 0;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char* nm = shstrtab + shdrs[i].sh_name;
        if (strcmp(nm, ".symtab") == 0) {
            symtab = (const Elf64_Sym*)(base + shdrs[i].sh_offset);
            sym_count = shdrs[i].sh_size / sizeof(Elf64_Sym);
        } else if (strcmp(nm, ".strtab") == 0) {
            strtab = (const char*)(base + shdrs[i].sh_offset);
        }
    }

    if (!symtab || !strtab || sym_count == 0) {
        LOGE("HiddenApi: no .symtab/.strtab (stripped?); sym_count=%zu", sym_count);
        munmap(map, (size_t)st.st_size);
        return nullptr;
    }
    LOGD("HiddenApi: .symtab scanned, %zu symbols, looking for '%s'", sym_count, needle);

    void* result = nullptr;
    for (size_t i = 1; i < sym_count; i++) {
        if (symtab[i].st_name == 0 || symtab[i].st_value == 0) continue;
        if (ELF64_ST_TYPE(symtab[i].st_info) != STT_FUNC) continue;
        const char* nm = strtab + symtab[i].st_name;
        if (strstr(nm, needle)) {
            result = (void*)(g_libart_runtime_base + symtab[i].st_value - vaddr_base);
            LOGD("HiddenApi: symtab match '%s' -> %p", nm, result);
            break;
        }
    }
    munmap(map, (size_t)st.st_size);
    return result;
}

static void* FindArtSymbol(const char* needle, FindCtx* outCtx) {
    // 1) dlsym
    void* p = dlsym(RTLD_DEFAULT, needle);
    if (p) { LOGD("HiddenApi: dlsym '%s' = %p", needle, p); return p; }

    // 2) .dynsym walk
    FindCtx ctx = { "libart.so", needle, nullptr, 0 };
    dl_iterate_phdr(FindInLib, &ctx);
    if (outCtx) *outCtx = ctx;
    if (ctx.addr) return ctx.addr;

    // 3) .symtab on disk (API 33+ path)
    std::lock_guard<std::mutex> lock(g_symtab_mutex);
    p = FindSymbolInElfFile("/system/lib64/libart.so", needle);
    if (p) return p;
    p = FindSymbolInElfFile("/apex/com.android.art/lib64/libart.so", needle);
    return p;
}

extern "C" int HiddenApi_Probe() {
    int found = 0;
    const char* needles[] = {
            "GetMemberAction",
            "ShouldDenyAccessToMember",
            nullptr
    };
    for (int i = 0; needles[i]; i++) {
        void* p = FindArtSymbol(needles[i], nullptr);
        LOGD("HiddenApi probe [%s] -> %p", needles[i], p);
        if (p) found++;
    }
    return found;
}

extern "C" int HiddenApi_Install() {
    const char* targets[] = {
            "GetMemberAction",
            "ShouldDenyAccessToMember",
            nullptr
    };
    for (int i = 0; targets[i]; i++) {
        void* target = FindArtSymbol(targets[i], nullptr);
        if (!target) { LOGE("HiddenApi: '%s' not found", targets[i]); continue; }
        void* orig = nullptr;
        if (InstallInlineHook(target, (void*)Fake_Allow, &orig)) {
            LOGD("HiddenApi: hooked '%s' at %p -> allow", targets[i], target);
            return 1;
        }
        LOGE("HiddenApi: InstallInlineHook failed for '%s' at %p", targets[i], target);
    }
    LOGE("HiddenApi: no target hookable");
    return 0;
}