// CppParcelRewriter.cpp
//
// Parses the AIDL request prelude on the caller thread using direct field
// access to android::Parcel's data buffer. Only depends on 4 simple getters:
//   data()            → mData
//   dataSize()        → max(mDataSize, mDataPos)
//   dataPosition()    → mDataPos
//   setDataPosition() → sets mDataPos
//
// All four are trivial leaf functions resolvable via exact mangling.
// readInt32 / readString16Inplace are avoided — signature ambiguity on this
// ROM (IDA shows the no-arg variant decompiled), and manual reads are
// simpler and correct.

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <cstdint>
#include <cstring>
#include <cstddef>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

typedef const uint8_t* (*parcel_data_t)(const void*);
typedef size_t         (*parcel_dataSize_t)(const void*);
typedef size_t         (*parcel_getPos_t)(const void*);
typedef void           (*parcel_setPos_t)(void*, size_t);

static parcel_data_t     g_data     = nullptr;
static parcel_dataSize_t g_dataSize = nullptr;
static parcel_getPos_t   g_getPos   = nullptr;
static parcel_setPos_t   g_setPos   = nullptr;

static bool     g_ready      = false;
static char16_t g_ourPkg[256] = {0};
static size_t   g_ourPkgLen  = 0;

static constexpr int32_t kMagic    = 0x53595354;  // "SYST"
static constexpr int32_t kInt32Max = 0x7FFFFFFF;

// ─── Dynsym scan ─────────────────────────────────────────────────────
struct ScanCtx { const char* lib; const char* needle; void* result; };

static int ScanCb(struct dl_phdr_info* info, size_t, void* data) {
    ScanCtx* c = (ScanCtx*)data;
    if (!info->dlpi_name || !strstr(info->dlpi_name, c->lib)) return 0;
    ElfW(Dyn)* dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (ElfW(Dyn)*)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn) return 1;
    const char* strtab = nullptr; ElfW(Sym)* symtab = nullptr;
    size_t strsz = 0; ElfW(Addr) gnuHash = 0, sysvHash = 0;
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
    uint32_t count = 0;
    if (gnuHash) {
        uint32_t* h = (uint32_t*)gnuHash;
        uint32_t nb = h[0], so = h[1], bs = h[2];
        if (!nb || !bs) return 1;
        uintptr_t p = (uintptr_t)h + 16 + (uintptr_t)bs * 8;
        uint32_t* bkts = (uint32_t*)p; p += (uintptr_t)nb * 4;
        uint32_t* chain = (uint32_t*)p;
        uint32_t mx = 0;
        for (uint32_t i = 0; i < nb; i++) if (bkts[i] > mx) mx = bkts[i];
        if (mx < so) count = so;
        else { uint32_t i = mx; while (i < so + 1000000 && !(chain[i - so] & 1)) i++; count = i + 1; }
    } else if (sysvHash) count = ((uint32_t*)sysvHash)[1];
    if (count == 0 || count > 500000) return 1;
    for (uint32_t i = 1; i < count; i++) {
        if (symtab[i].st_name == 0 || symtab[i].st_name >= strsz) continue;
        const char* name = strtab + symtab[i].st_name;
        if (strcmp(name, c->needle) == 0 && symtab[i].st_value) {
            c->result = (void*)(info->dlpi_addr + symtab[i].st_value);
            return 1;
        }
    }
    return 1;
}

static void* Scan(const char* lib, const char* needle) {
    ScanCtx c = { lib, needle, nullptr };
    dl_iterate_phdr(ScanCb, &c);
    return c.result;
}

// ─── Position helpers ────────────────────────────────────────────────
static void SafeSetPos(void* p, size_t pos) {
    if (g_setPos) { g_setPos(p, pos); return; }
    // Fallback — IDA-verified offsets for API 37 libbinder.so:
    // mDataPos=+32, mType=+88, mNextObjectHint=+64, mParseHeader=+81
    *(size_t*)((uint8_t*)p + 32) = pos;
    if (*(int32_t*)((uint8_t*)p + 88) == 0) {
        *(size_t*)((uint8_t*)p + 64) = 0;
        *(uint8_t*)((uint8_t*)p + 81) = 0;
    }
}

static bool SafeReadInt32(void* p, int32_t* out) {
    if (!g_data || !g_dataSize || !g_getPos) return false;
    size_t total = g_dataSize(p);
    size_t pos   = g_getPos(p);
    if (pos + 4 > total) return false;
    const uint8_t* mData = g_data(p);
    if (!mData) return false;
    *out = *(const int32_t*)(mData + pos);
    SafeSetPos(p, pos + 4);
    return true;
}

static const char16_t* SafeReadString16(void* p, size_t* outLen) {
    *outLen = 0;
    if (!g_data || !g_dataSize || !g_getPos) return nullptr;
    size_t total = g_dataSize(p);
    size_t pos   = g_getPos(p);
    if (pos + 4 > total) return nullptr;
    const uint8_t* mData = g_data(p);
    if (!mData) return nullptr;
    int32_t len = *(const int32_t*)(mData + pos);
    SafeSetPos(p, pos + 4);
    if (len < 0 || len >= kInt32Max) return nullptr;
    size_t bytes   = (size_t)len * 2 + 2;
    size_t aligned = (bytes + 3) & ~(size_t)3;
    size_t newPos  = pos + 4 + aligned;
    if (newPos > total) return nullptr;
    const char16_t* str = (const char16_t*)(mData + pos + 4);
    if (str[len] != 0) return nullptr;
    *outLen = (size_t)len;
    SafeSetPos(p, newPos);
    return str;
}

// ─── Init ────────────────────────────────────────────────────────────
extern "C" bool ParcelRewriter_Init(JNIEnv* env) {
    if (g_ready) return true;
    if (!env) return false;

    if (!g_data)
        g_data = (parcel_data_t)Scan("libbinder.so", "_ZNK7android6Parcel4dataEv");
    if (!g_dataSize)
        g_dataSize = (parcel_dataSize_t)Scan("libbinder.so", "_ZNK7android6Parcel8dataSizeEv");
    if (!g_getPos)
        g_getPos = (parcel_getPos_t)Scan("libbinder.so", "_ZNK7android6Parcel12dataPositionEv");
    if (!g_setPos)
        g_setPos = (parcel_setPos_t)Scan("libbinder.so", "_ZNK7android6Parcel15setDataPositionEm");

    LOGD("ParcelRewriter: data=%p dataSize=%p getPos=%p setPos=%p",
         (void*)g_data, (void*)g_dataSize, (void*)g_getPos, (void*)g_setPos);

    if (!g_data || !g_dataSize || !g_getPos) {
        LOGE("ParcelRewriter: core primitives missing");
        return false;
    }

    // Read ParcelRewriter.ourPackageName via @JvmStatic helper
    jclass rc = env->FindClass("com/ims134/hookengine/ParcelRewriter");
    if (!rc) { env->ExceptionClear(); LOGE("ParcelRewriter: class missing"); return false; }
    jmethodID m = env->GetStaticMethodID(rc, "getOurPackageNameForCpp",
                                         "()Ljava/lang/String;");
    jstring pkg = nullptr;
    if (m) {
        pkg = (jstring)env->CallStaticObjectMethod(rc, m);
        if (env->ExceptionCheck()) { env->ExceptionClear(); pkg = nullptr; }
    } else {
        env->ExceptionClear();
    }
    env->DeleteLocalRef(rc);
    if (!pkg) { LOGE("ParcelRewriter: ourPackageName null"); return false; }

    jsize n = env->GetStringLength(pkg);
    if (n <= 0 || (size_t)n >= sizeof(g_ourPkg)/sizeof(g_ourPkg[0])) {
        env->DeleteLocalRef(pkg); return false;
    }
    const jchar* ch = env->GetStringChars(pkg, nullptr);
    if (!ch) { env->DeleteLocalRef(pkg); return false; }
    memcpy(g_ourPkg, ch, (size_t)n * sizeof(char16_t));
    g_ourPkg[n] = 0; g_ourPkgLen = (size_t)n;
    env->ReleaseStringChars(pkg, ch);
    env->DeleteLocalRef(pkg);

    LOGD("ParcelRewriter: init OK pkgLen=%zu", g_ourPkgLen);
    g_ready = true;
    return true;
}

// ─── Prelude match ───────────────────────────────────────────────────
extern "C" bool ParcelRewriter_MatchesRequest(int code, jlong dataPtr) {
    if (!g_ready) return false;
    if (code != 3 && code != 36) return false;
    if (!dataPtr) return false;

    void* parcel = (void*)dataPtr;
    size_t saved = g_getPos(parcel);
    SafeSetPos(parcel, 0);

    bool ok = false;
    do {
        int32_t strict, worksource, magic;
        if (!SafeReadInt32(parcel, &strict))     break;
        if (!SafeReadInt32(parcel, &worksource)) break;
        if (!SafeReadInt32(parcel, &magic))      break;
        if (magic != kMagic) break;

        size_t ifaceLen = 0;
        const char16_t* iface = SafeReadString16(parcel, &ifaceLen);
        if (!iface) break;
        static const char16_t kIface[] = u"android.content.pm.IPackageManager";
        constexpr size_t kIfaceLen = sizeof(kIface)/sizeof(char16_t) - 1;
        if (ifaceLen != kIfaceLen ||
            memcmp(iface, kIface, kIfaceLen * sizeof(char16_t)) != 0) break;

        if (code == 3) {
            size_t pkgLen = 0;
            const char16_t* pkg = SafeReadString16(parcel, &pkgLen);
            if (!pkg) break;
            if (pkgLen != g_ourPkgLen ||
                memcmp(pkg, g_ourPkg, pkgLen * sizeof(char16_t)) != 0) break;
        }
        ok = true;
    } while (0);

    SafeSetPos(parcel, saved);
    return ok;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_com_ims134_hookengine_NativeTestBridge_parcelRewriterReady(
        JNIEnv*, jobject) {
    return g_ready ? JNI_TRUE : JNI_FALSE;
}

extern "C" const uint8_t* ParcelRewriter_GetData(jlong parcelPtr) {
    if (!parcelPtr || !g_data) return nullptr;
    return g_data((void*)parcelPtr);
}

extern "C" size_t ParcelRewriter_GetSize(jlong parcelPtr) {
    if (!parcelPtr || !g_dataSize) return 0;
    return g_dataSize((void*)parcelPtr);
}

extern "C" size_t ParcelRewriter_GetPos(jlong parcelPtr) {
    if (!parcelPtr || !g_getPos) return 0;
    return g_getPos((void*)parcelPtr);
}