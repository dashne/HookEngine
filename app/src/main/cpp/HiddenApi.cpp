#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <string.h>
#include <stdint.h>

#define LOG_TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

extern "C" bool InstallInlineHook(void*, void*, void**);

static uint32_t Fake_Allow() { return 0; }

// -------- GNU hash helper: compute symbol count from DT_GNU_HASH --------
// Header (word size = 4 bytes):
//   [0] nbuckets
//   [1] symoffset
//   [2] bloom_size   (in 8-byte bloom words on 64-bit)
//   [3] bloom_shift
// Then: bloom[bloom_size], buckets[nbuckets], chain[]
// chain size = max(bucket) - symoffset + (run of set bits)

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
    for (uint32_t i = 0; i < nbuckets; i++)
        if (buckets[i] > maxSym) maxSym = buckets[i];
    if (maxSym < symoffset) return symoffset;

    uint32_t idx = maxSym;
    // Guard against runaway: cap at 1M symbols.
    uint32_t cap = symoffset + 1000000;
    while (idx < cap && !(chain[idx - symoffset] & 1)) idx++;
    return idx + 1;
}

struct FindCtx {
    const char* lib;
    const char* needle;
    void* addr;
    int scanned;
    int hiddenapiCount;
    char firstHiddenapi[128];
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
    ElfW(Addr) gnuHash = 0;
    ElfW(Addr) sysvHash = 0;
    size_t relasz = 0;
    ElfW(Addr) rela = 0;

    for (ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_STRTAB:  strtab  = (const char*)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_SYMTAB:  symtab  = (ElfW(Sym)*)(info->dlpi_addr + d->d_un.d_ptr); break;
            case DT_STRSZ:   strsz   = d->d_un.d_val; break;
            case DT_HASH:    sysvHash = info->dlpi_addr + d->d_un.d_ptr; break;
            case DT_GNU_HASH: gnuHash = info->dlpi_addr + d->d_un.d_ptr; break;
            case DT_RELA:    rela = info->dlpi_addr + d->d_un.d_ptr; break;
            case DT_RELASZ:  relasz = d->d_un.d_val; break;
        }
    }
    if (!strtab || !symtab || !strsz) {
        LOGE("HiddenApi: missing .dynstr/.dynsym in %s", info->dlpi_name);
        return 1;
    }

    uint32_t count = 0;
    if (gnuHash) {
        count = GnuHashSymCount((uint32_t*)gnuHash);
    } else if (sysvHash) {
        count = ((uint32_t*)sysvHash)[1];   // nchain
    }
    if (count == 0) {
        LOGE("HiddenApi: cannot determine symtab size in %s (no hash)", info->dlpi_name);
        return 1;
    }
    if (count > 500000) count = 500000;   // hard cap

    int firstHiddenapiHit = 0;
    for (uint32_t i = 1; i < count; i++) {
        ctx->scanned++;
        size_t name_off = symtab[i].st_name;
        if (name_off == 0 || name_off >= strsz) continue;
        const char* name = strtab + name_off;
        if (!name[0]) continue;

        // Count hiddenapi-related symbols for the report
        if (!firstHiddenapiHit && strstr(name, "iddenapi")) {
            firstHiddenapiHit = 1;
            ctx->hiddenapiCount++;
            strncpy(ctx->firstHiddenapi, name, sizeof(ctx->firstHiddenapi) - 1);
        } else if (firstHiddenapiHit && strstr(name, "iddenapi")) {
            ctx->hiddenapiCount++;
        }

        if (!ctx->addr && strstr(name, ctx->needle) && symtab[i].st_value) {
            ctx->addr = (void*)(info->dlpi_addr + symtab[i].st_value);
            LOGD("HiddenApi: matched '%s' -> %p", name, ctx->addr);
        }
    }
    return 1;
}

static void* FindArtSymbol(const char* needle, FindCtx* outCtx) {
    // 1) dlsym on RTLD_DEFAULT — only finds exported symbols, but cheap.
    void* p = dlsym(RTLD_DEFAULT, needle);
    if (p) {
        LOGD("HiddenApi: dlsym(RTLD_DEFAULT, '%s') = %p", needle, p);
        return p;
    }

    // 2) Walk libart.so's .dynsym (bounded by GNU or SysV hash).
    FindCtx ctx = { "libart.so", needle, nullptr, 0, 0, {0} };
    dl_iterate_phdr(FindInLib, &ctx);
    if (outCtx) *outCtx = ctx;
    return ctx.addr;
}

extern "C" int HiddenApi_Probe() {
    int found = 0;
    const char* needles[] = {
            "_ZN3art9hiddenapi15GetMemberAction",
            "_ZN3art9hiddenapi25ShouldDenyAccessToMember",
            "GetMemberAction",
            "ShouldDenyAccessToMember",
            nullptr
    };
    FindCtx report{};
    for (int i = 0; needles[i]; i++) {
        FindCtx local{};
        void* p = FindArtSymbol(needles[i], &local);
        LOGD("HiddenApi probe [%s] -> %p (scanned=%d, hiddenapi=%d)",
             needles[i], p, local.scanned, local.hiddenapiCount);
        if (local.scanned > report.scanned) report = local;
        if (p) found++;
    }
    LOGD("HiddenApi report: scanned=%d symbols, %d hiddenapi-related",
         report.scanned, report.hiddenapiCount);
    if (report.firstHiddenapi[0])
        LOGD("HiddenApi report: first hiddenapi symbol = '%s'", report.firstHiddenapi);
    return found;
}

extern "C" int HiddenApi_Install() {
    const char* preferred[] = {
            "GetMemberAction",
            "ShouldDenyAccessToMember",
            nullptr
    };
    for (int i = 0; preferred[i]; i++) {
        void* target = FindArtSymbol(preferred[i], nullptr);
        if (!target) continue;
        void* orig = nullptr;
        if (InstallInlineHook(target, (void*)Fake_Allow, &orig)) {
            LOGD("HiddenApi: hooked '%s' at %p -> allow", preferred[i], target);
            return 1;
        }
        LOGE("HiddenApi: InstallInlineHook failed for '%s' at %p", preferred[i], target);
    }
    LOGE("HiddenApi: no hookable enforcement symbol found");
    return 0;
}