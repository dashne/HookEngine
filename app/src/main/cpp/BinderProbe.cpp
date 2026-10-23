#include <jni.h>
#include <android/log.h>
#include <link.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static uint32_t GnuHashSymCount(uint32_t* h) {
    uint32_t nbuckets = h[0], symoffset = h[1], bloomSize = h[2];
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

struct ScanCtx {
    const char* libSubstr;
    const char* needle;
    void*       result;
};

static int ScanLibCallback(struct dl_phdr_info* info, size_t, void* data) {
    ScanCtx* ctx = (ScanCtx*)data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, ctx->libSubstr)) return 0;

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
        if (symtab[i].st_name == 0 || symtab[i].st_name >= strsz) continue;
        const char* name = strtab + symtab[i].st_name;
        if (strstr(name, ctx->needle) && symtab[i].st_value) {
            ctx->result = (void*)(info->dlpi_addr + symtab[i].st_value);
            // Log the full mangled name so we know what to reference later.
            LOGD("  full symbol: %s", name);
            return 1;
        }
    }
    return 1;
}

static void* ScanDynsym(const char* libSubstr, const char* needle) {
    ScanCtx ctx = { libSubstr, needle, nullptr };
    dl_iterate_phdr(ScanLibCallback, &ctx);
    return ctx.result;
}

extern "C" int BinderProbe() {
    struct Probe { const char* name; const char* needle; };
    static const Probe kProbes[] = {
            // BinderProxy::transact — the target for tnFIN
            {"BinderProxy::transact",     "BinderProxy7transact"},
            {"BinderProxy::transact alt", "BinderProxy6transact"},

            // Parcel I/O — needed inside the hook body to read/write
            {"Parcel::readInt32",          "6Parcel9readInt32"},
            {"Parcel::writeInt32",         "6Parcel10writeInt32"},
            {"Parcel::readString16",       "6Parcel12readString16"},
            {"Parcel::writeString16",      "6Parcel13writeString16"},
            {"Parcel::readStrongBinder",   "6Parcel16readStrongBinder"},
            {"Parcel::writeStrongBinder",  "6Parcel17writeStrongBinder"},
            {"Parcel::data",               "6Parcel4data"},
            {"Parcel::dataSize",           "6Parcel8dataSize"},
            {"Parcel::dataPosition",       "6Parcel12dataPosition"},
            {"Parcel::setDataPosition",    "6Parcel15setDataPosition"},

            // Alternative hook targets if BinderProxy::transact doesn't land
            {"IPCThreadState::transact",   "14IPCThreadState8transact"},
            {"ServiceManager::getService", "14ServiceManager10getService"},

            {nullptr, nullptr}
    };

    int found = 0;
    for (int i = 0; kProbes[i].name; i++) {
        LOGD("BinderProbe [%s]", kProbes[i].name);
        void* addr = ScanDynsym("libbinder.so", kProbes[i].needle);
        LOGD("  -> %p", addr);
        if (addr) found++;
    }
    LOGD("BinderProbe: %d / %d symbols found in libbinder.so",
         found, (int)(sizeof(kProbes)/sizeof(kProbes[0]) - 1));
    return found;
}