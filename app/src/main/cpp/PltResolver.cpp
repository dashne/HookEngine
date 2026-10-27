#include "PltResolver.h"
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <android/log.h>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static constexpr uint32_t kInsnAdrp = 0x90000000u;
static constexpr uint32_t kInsnAdrpMask = 0x9F000000u;
static constexpr uint32_t kInsnBtiC = 0xD503245Fu;
static constexpr uint32_t kRelAarch64JumpSlot = 1026;

struct LibFindCtx {
    const char* needle;
    uintptr_t   dlpi_addr;
    char        path[512];
    int         found;
};

static int FindLibCb(struct dl_phdr_info* info, size_t, void* data) {
    LibFindCtx* ctx = (LibFindCtx*)data;
    if (!info->dlpi_name || !info->dlpi_name[0]) return 0;
    if (!strstr(info->dlpi_name, ctx->needle)) return 0;
    ctx->dlpi_addr = (uintptr_t)info->dlpi_addr;
    strncpy(ctx->path, info->dlpi_name, sizeof(ctx->path) - 1);
    ctx->path[sizeof(ctx->path) - 1] = '\0';
    ctx->found = 1;
    return 1;
}

static bool LooksLikePltStub(uint64_t abs_addr) {
    if (abs_addr < 0x1000) return false;
    uint32_t insn0 = *(volatile uint32_t*)(uintptr_t)abs_addr;
    return (insn0 & kInsnAdrpMask) == kInsnAdrp || insn0 == kInsnBtiC;
}

extern "C" uint64_t PltResolveForSymbol(const char* lib_substr, const char* symbol) {
    if (!lib_substr || !lib_substr[0] || !symbol || !symbol[0]) return 0;

    LibFindCtx ctx = { lib_substr, 0, {0}, 0 };
    dl_iterate_phdr(FindLibCb, &ctx);
    if (!ctx.found) {
        LOGE("PltResolver: lib '%s' not found", lib_substr);
        return 0;
    }
    LOGD("PltResolver: lib '%s' path=%s bias=0x%lx",
         lib_substr, ctx.path, (unsigned long)ctx.dlpi_addr);

    int fd = open(ctx.path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGE("PltResolver: open(%s) failed errno=%d", ctx.path, errno);
        return 0;
    }
    struct stat st{};
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return 0;
    }
    void* map = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) {
        LOGE("PltResolver: mmap failed errno=%d", errno);
        return 0;
    }

    const uint8_t* base = (const uint8_t*)map;
    const Elf64_Ehdr* ehdr = (const Elf64_Ehdr*)base;

    if (memcmp(ehdr->e_ident, ELFMAG, 4) != 0 ||
        ehdr->e_ident[EI_CLASS] != ELFCLASS64 ||
        ehdr->e_ident[EI_DATA]  != ELFDATA2LSB ||
        ehdr->e_machine != EM_AARCH64) {
        LOGE("PltResolver: not 64-bit LE AARCH64");
        munmap(map, (size_t)st.st_size);
        return 0;
    }
    if (ehdr->e_shoff == 0 || ehdr->e_shnum == 0) {
        LOGE("PltResolver: no SHDR (stripped .so)");
        munmap(map, (size_t)st.st_size);
        return 0;
    }

    const Elf64_Shdr* shdrs = (const Elf64_Shdr*)(base + ehdr->e_shoff);
    const char* shstrtab = (const char*)(base + shdrs[ehdr->e_shstrndx].sh_offset);

    const Elf64_Shdr* dynsym_shdr  = nullptr;
    const Elf64_Shdr* dynstr_shdr  = nullptr;
    const Elf64_Shdr* relaplt_shdr = nullptr;
    const Elf64_Shdr* plt_shdr     = nullptr;
    const Elf64_Shdr* pltsec_shdr  = nullptr;

    for (int i = 0; i < ehdr->e_shnum; i++) {
        const char* name = shstrtab + shdrs[i].sh_name;
        if      (strcmp(name, ".dynsym")   == 0) dynsym_shdr  = &shdrs[i];
        else if (strcmp(name, ".dynstr")   == 0) dynstr_shdr  = &shdrs[i];
        else if (strcmp(name, ".rela.plt") == 0) relaplt_shdr = &shdrs[i];
        else if (strcmp(name, ".plt")      == 0) plt_shdr     = &shdrs[i];
        else if (strcmp(name, ".plt.sec")  == 0) pltsec_shdr  = &shdrs[i];
    }

    if (!dynsym_shdr || !dynstr_shdr || !relaplt_shdr) {
        LOGE("PltResolver: missing sections (dynsym=%p dynstr=%p rela.plt=%p)",
             dynsym_shdr, dynstr_shdr, relaplt_shdr);
        munmap(map, (size_t)st.st_size);
        return 0;
    }

    const Elf64_Sym* dynsym = (const Elf64_Sym*)(base + dynsym_shdr->sh_offset);
    size_t dynsym_count = dynsym_shdr->sh_size / sizeof(Elf64_Sym);
    const char* dynstr = (const char*)(base + dynstr_shdr->sh_offset);

    const Elf64_Rela* relas = (const Elf64_Rela*)(base + relaplt_shdr->sh_offset);
    size_t rela_count = relaplt_shdr->sh_size / sizeof(Elf64_Rela);

    LOGD("PltResolver: %zu relocs, .plt=%p .plt.sec=%p",
         rela_count, plt_shdr, pltsec_shdr);

    if (!pltsec_shdr && !plt_shdr) {
        LOGE("PltResolver: no .plt or .plt.sec section");
        munmap(map, (size_t)st.st_size);
        return 0;
    }

    // .plt.sec is BTI-enabled — has only symbol entries, no PLT0 header.
    // .plt (when .plt.sec absent) has PLT0 (32 bytes) then per-symbol entries.
    // The exact offset of the first real entry in .plt (32 vs 48) depends on
    // the linker. Try both; verify by reading the first instruction word.
    size_t plt_index = 0;
    for (size_t i = 0; i < rela_count; i++) {
        const Elf64_Rela* r = &relas[i];
        if (ELF64_R_TYPE(r->r_info) != kRelAarch64JumpSlot) continue;
        uint32_t sym_idx = ELF64_R_SYM(r->r_info);
        if (sym_idx == 0 || sym_idx >= dynsym_count) { plt_index++; continue; }
        const char* sym_name = dynstr + dynsym[sym_idx].st_name;
        if (strcmp(sym_name, symbol) != 0) { plt_index++; continue; }

        // Found the symbol. Compute PLT entry address.
        if (pltsec_shdr) {
            // BTI layout — offset is exactly plt_index * 16 from .plt.sec base.
            uint64_t off = (uint64_t)plt_index * 16;
            if (off + 16 > pltsec_shdr->sh_size) {
                LOGE("PltResolver: .plt.sec offset %lu exceeds size %lu",
                     (unsigned long)off, (unsigned long)pltsec_shdr->sh_size);
                munmap(map, (size_t)st.st_size);
                return 0;
            }
            uint64_t abs_addr = (uint64_t)ctx.dlpi_addr + pltsec_shdr->sh_addr + off;
            LOGD("PltResolver: '%s' plt_index=%zu .plt.sec+0x%lx = 0x%lx",
                 symbol, plt_index, (unsigned long)off, (unsigned long)abs_addr);
            munmap(map, (size_t)st.st_size);
            return abs_addr;
        }

        // .plt fallback. Try base offsets 32 (PLT[1] is first real entry)
        // and 48 (PLT[1] reserved, PLT[2] is first real entry). Validate by
        // reading the first instruction word and requiring adrp or bti c.
        const uint64_t base_candidates[] = { 32, 48 };
        for (uint64_t base_off : base_candidates) {
            uint64_t off = base_off + (uint64_t)plt_index * 16;
            if (off + 16 > plt_shdr->sh_size) continue;
            uint64_t abs_addr = (uint64_t)ctx.dlpi_addr + plt_shdr->sh_addr + off;
            if (LooksLikePltStub(abs_addr)) {
                LOGD("PltResolver: '%s' plt_index=%zu .plt+0x%lx (base=%lu) = 0x%lx",
                     symbol, plt_index, (unsigned long)off,
                     (unsigned long)base_off, (unsigned long)abs_addr);
                munmap(map, (size_t)st.st_size);
                return abs_addr;
            }
            LOGD("PltResolver: candidate .plt+0x%lx (base=%lu) failed opcode check",
                 (unsigned long)off, (unsigned long)base_off);
        }
        LOGE("PltResolver: '%s' no valid PLT stub at either .plt offset", symbol);
        munmap(map, (size_t)st.st_size);
        return 0;
    }

    LOGD("PltResolver: '%s' not in .rela.plt", symbol);
    munmap(map, (size_t)st.st_size);
    return 0;
}