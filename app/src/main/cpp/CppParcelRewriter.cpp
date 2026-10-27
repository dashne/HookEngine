// CppParcelRewriter.cpp
//
// AIDL prelude rewriter for IStorageManager + IContentProvider + ServiceManager lookups.
//
// Session 23 — Wall B (IStorageManager callingPackage rewrite).
// Session 28 — Wall C2 (IContentProvider pkg+uid rewrite for viber).
// Session 29 — Wall C3 diagnostic (miui.* ifaces, capped).
// Session 32 — uid resolved via getuid() at call time.
// Session 33 — syscall() for real pid/uid + skip-if-correct.
// Session 51 — Feature 3.4: ServiceManager lookup interception.
//              v4: String16 byte-pattern search instead of prelude parse.
//              v5: Dropped code filter. On-device diag showed SM transacts
//                  use code=4 (not 1/2 as on stock AOSP), so the previous
//                  filter short-circuited on every real getService call.
//                  Pattern match itself is a sufficient discriminator — a
//                  String16 encoding of a registered name is unique enough
//                  that accidental matches in unrelated SM parcels are
//                  astronomically unlikely.
//              v6 (51b): Null override retired from reply-mutation
//                  (mprotect+memset on PROT_READ binder reply mapping =
//                  EACCES; mmap-swap did not propagate to caller's
//                  readException path) and reimplemented as a REQUEST
//                  rewrite: overridden name → '_NOEXIST_SM_OVERRIDE'
//                  sentinel. Driver's natural "unknown service" reply is
//                  exceptionCode=0 + null flat_binder_object — exactly
//                  what the caller expects for "not found". Both null and
//                  alias paths now increment their own hit counter so
//                  Rule-12 test assertions can prove each mechanism
//                  fired (or didn't, in the null-wins-over-alias case).

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <atomic>
#include <cerrno>
#include <sys/mman.h>

#ifndef CP_REWRITE_UPDATE_LEADING_INT32
#define CP_REWRITE_UPDATE_LEADING_INT32 0
#endif

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

static int g_miui_dump_count = 0;
static int g_cp_diag_count   = 0;

static constexpr int32_t kMagic    = 0x53595354;
static constexpr int32_t kInt32Max = 0x7FFFFFFF;

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

static void SafeSetPos(void* p, size_t pos) {
    if (g_setPos) { g_setPos(p, pos); return; }
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

extern "C" bool ParcelRewriter_TryRewriteCallerIdentity(jlong dataPtr, int code) {
    if (!g_ready || !g_data || !g_dataSize || !g_getPos) return false;
    if (!dataPtr) return false;

    const int32_t realUid = (int32_t)syscall(SYS_getuid);
    const int32_t realPid = (int32_t)syscall(SYS_getpid);

    void* parcel = (void*)dataPtr;
    size_t saved = g_getPos(parcel);
    SafeSetPos(parcel, 0);

    bool rewritten = false;
    bool bufferSwapped = false;

    do {
        int32_t strict, worksource, magic;
        if (!SafeReadInt32(parcel, &strict))     break;
        if (!SafeReadInt32(parcel, &worksource)) break;
        if (!SafeReadInt32(parcel, &magic))      break;
        if (magic != kMagic) break;

        size_t ifaceLen = 0;
        const char16_t* iface = SafeReadString16(parcel, &ifaceLen);
        if (!iface) break;

        if (g_miui_dump_count < 3 && ifaceLen > 0 && ifaceLen < 128) {
            char ifaceAscii[128] = {0};
            size_t copyLen = ifaceLen < 127 ? ifaceLen : 127;
            for (size_t i = 0; i < copyLen; i++) {
                char16_t c = iface[i];
                ifaceAscii[i] = (c < 0x80) ? (char)c : '?';
            }
            bool isMiui = (strstr(ifaceAscii, "ProcessManager") != nullptr) ||
                          (strncmp(ifaceAscii, "miui.", 5) == 0);
            if (isMiui) {
                size_t dumpStart = g_getPos(parcel);
                size_t totalSize = g_dataSize(parcel);
                const uint8_t* dumpData = g_data(parcel);
                size_t dumpLen = 0;
                if (dumpData && dumpStart < totalSize) {
                    dumpLen = (totalSize - dumpStart < 128)
                              ? (totalSize - dumpStart) : 128;
                }
                char hexbuf[400];
                int hpos = snprintf(hexbuf, sizeof(hexbuf),
                                    "MIUI prelude dump: iface='%s' code=%d pos=%zu len=%zu: ",
                                    ifaceAscii, code, dumpStart, dumpLen);
                for (size_t i = 0; i < dumpLen && hpos < 360; i++) {
                    hpos += snprintf(hexbuf + hpos, sizeof(hexbuf) - hpos,
                                     "%02x", dumpData[dumpStart + i]);
                }
                LOGD("%s", hexbuf);
                g_miui_dump_count++;
            }
        }

        static const char16_t kStorageIface[] = u"android.os.storage.IStorageManager";
        constexpr size_t kStorageIfaceLen = sizeof(kStorageIface)/sizeof(char16_t) - 1;
        static const char16_t kContentProviderIface[] = u"android.content.IContentProvider";
        constexpr size_t kContentProviderIfaceLen = sizeof(kContentProviderIface)/sizeof(char16_t) - 1;

        static const char16_t kOld[] = u"com.viber.voip";
        static const char16_t kNew[] = u"com.ims134.hookengine";
        constexpr size_t kOldLen = sizeof(kOld)/sizeof(char16_t) - 1;
        constexpr size_t kNewLen = sizeof(kNew)/sizeof(char16_t) - 1;
        constexpr size_t kOldTotal = 4 + 2*kOldLen + 2 + 2;
        constexpr size_t kNewTotal = 4 + 2*kNewLen + 2 + 0;
        constexpr size_t kDelta    = kNewTotal - kOldTotal;

        if (ifaceLen == kStorageIfaceLen &&
            memcmp(iface, kStorageIface, kStorageIfaceLen * sizeof(char16_t)) == 0) {
            int32_t userId = 0;
            if (!SafeReadInt32(parcel, &userId)) break;

            size_t cpLen = 0;
            const char16_t* cp = SafeReadString16(parcel, &cpLen);
            if (!cp) break;

            if (cpLen != kOldLen ||
                memcmp(cp, kOld, kOldLen * sizeof(char16_t)) != 0) {
                char ascii[64] = {0};
                size_t n = cpLen < 63 ? cpLen : 63;
                for (size_t i = 0; i < n; i++) ascii[i] = (char)cp[i];
                LOGD("STORAGE iface matched but callingPackage='%s' (code=%d)", ascii, code);
                break;
            }

            const uint8_t* pData = g_data(parcel);
            if (!pData) break;
            ptrdiff_t cpOffset = (const uint8_t*)cp - pData;
            if (cpOffset < 4) break;
            size_t oldStringStart = (size_t)(cpOffset - 4);

            size_t dataSize = g_dataSize(parcel);
            if (dataSize < oldStringStart + kOldTotal) break;

            size_t newSize = dataSize + kDelta;
            uint8_t* newBuf = (uint8_t*)malloc(newSize);
            if (!newBuf) break;

            memcpy(newBuf, pData, oldStringStart);
            *(int32_t*)(newBuf + oldStringStart) = (int32_t)kNewLen;
            memcpy(newBuf + oldStringStart + 4, kNew, kNewLen * sizeof(char16_t));
            *(char16_t*)(newBuf + oldStringStart + 4 + kNewLen * sizeof(char16_t)) = 0;
            memcpy(newBuf + oldStringStart + kNewTotal,
                   pData + oldStringStart + kOldTotal,
                   dataSize - oldStringStart - kOldTotal);

            void** dataField = (void**)((uint8_t*)parcel + 8);
            uint8_t* oldBuf  = (uint8_t*)*dataField;
            *dataField = newBuf;
            *(size_t*)((uint8_t*)parcel + 16) = newSize;
            *(size_t*)((uint8_t*)parcel + 24) = newSize;
            if (oldBuf) free(oldBuf);
            bufferSwapped = true;

            size_t newPos = (saved > oldStringStart) ? (saved + kDelta) : saved;
            SafeSetPos(parcel, newPos);

            LOGD("STORAGE rewrite fired: code=%d userId=%d size=%zu->%zu delta=%zu",
                 code, userId, dataSize, newSize, kDelta);
            rewritten = true;

        } else if (ifaceLen == kContentProviderIfaceLen &&
                   memcmp(iface, kContentProviderIface, kContentProviderIfaceLen * sizeof(char16_t)) == 0) {
            size_t aSStart = g_getPos(parcel);
            size_t dataSize = g_dataSize(parcel);
            const uint8_t* pData = g_data(parcel);
            if (!pData || aSStart + 20 > dataSize) break;

            int32_t leading = 0;
            if (!SafeReadInt32(parcel, &leading)) break;
            int32_t f4 = 0;
            if (!SafeReadInt32(parcel, &f4)) break;
            int32_t f8 = 0;
            if (!SafeReadInt32(parcel, &f8)) break;
            int32_t tagLen = 0;
            if (!SafeReadInt32(parcel, &tagLen)) break;
            (void)tagLen;

            size_t cpLen = 0;
            const char16_t* cp = SafeReadString16(parcel, &cpLen);
            if (!cp) {
                char hexbuf[512];
                int hpos = snprintf(hexbuf, sizeof(hexbuf),
                                    "CP iface matched but pkg read failed aS=%zu bytes: ", aSStart);
                size_t dumpLen = (dataSize - aSStart < 32) ? (dataSize - aSStart) : 32;
                for (size_t i = 0; i < dumpLen && hpos < 460; i++) {
                    hpos += snprintf(hexbuf + hpos, sizeof(hexbuf) - hpos,
                                     "%02x", pData[aSStart + i]);
                }
                LOGD("%s", hexbuf);
                break;
            }

            char ascii[64] = {0};
            size_t n = cpLen < 63 ? cpLen : 63;
            for (size_t i = 0; i < n; i++) ascii[i] = (char)cp[i];

            bool pkgIsViber = (cpLen == kOldLen &&
                               memcmp(cp, kOld, kOldLen * sizeof(char16_t)) == 0);
            bool pkgIsOwn = (cpLen == g_ourPkgLen &&
                             memcmp(cp, g_ourPkg, g_ourPkgLen * sizeof(char16_t)) == 0);

            if (pkgIsOwn && g_cp_diag_count < 5) {
                char hexbuf[512];
                int hpos = snprintf(hexbuf, sizeof(hexbuf),
                                    "CP diag (own-pkg): code=%d aS=%zu dataSize=%zu "
                                    "leading=%d f4=%d f8=%d pkgLen=%zu pkg='%s' bytes: ",
                                    code, aSStart, dataSize, leading, f4, f8, cpLen, ascii);
                size_t dumpLen = (dataSize - aSStart < 64) ? (dataSize - aSStart) : 64;
                for (size_t i = 0; i < dumpLen && hpos < 440; i++) {
                    hpos += snprintf(hexbuf + hpos, sizeof(hexbuf) - hpos,
                                     "%02x", pData[aSStart + i]);
                }
                LOGD("%s", hexbuf);
                g_cp_diag_count++;
            }

            constexpr size_t kPkgLenOffsetInAS = 16;
            constexpr size_t kSuffixStartInAS  = kPkgLenOffsetInAS + kOldTotal;

            if (pkgIsOwn) {
                if (f4 == realPid && f8 == realUid) {
                    LOGD("CP own-pkg already correct (pid=%d uid=%d) — skip rewrite (code=%d)",
                         realPid, realUid, code);
                    break;
                }
                size_t newSize = dataSize;
                uint8_t* newBuf = (uint8_t*)malloc(newSize);
                if (!newBuf) break;

                memcpy(newBuf, pData, aSStart + 4);
                *(int32_t*)(newBuf + aSStart + 4) = realPid;
                *(int32_t*)(newBuf + aSStart + 8) = realUid;
                memcpy(newBuf + aSStart + 12, pData + aSStart + 12,
                       dataSize - aSStart - 12);

                void** dataField = (void**)((uint8_t*)parcel + 8);
                uint8_t* oldBuf  = (uint8_t*)*dataField;
                *dataField = newBuf;
                if (oldBuf) free(oldBuf);
                bufferSwapped = true;

                SafeSetPos(parcel, saved);

                const uint8_t* verifyData = g_data(parcel);
                int32_t v4 = verifyData ? *(const int32_t*)(verifyData + aSStart + 4) : -1;
                int32_t v8 = verifyData ? *(const int32_t*)(verifyData + aSStart + 8) : -1;
                LOGD("CP own-pkg rewrite fired: code=%d pid=%d->%d uid=%d->%d "
                     "pkg='com.ims134.hookengine' size=%zu verify4=%d verify8=%d",
                     code, f4, realPid, f8, realUid, newSize, v4, v8);
                rewritten = true;
                break;

            } else if (pkgIsViber) {
                size_t pkgLenOffset = aSStart + kPkgLenOffsetInAS;
                size_t suffixStart  = aSStart + kSuffixStartInAS;
                if (dataSize < suffixStart) break;

                size_t newSize = dataSize + kDelta;
                uint8_t* newBuf = (uint8_t*)malloc(newSize);
                if (!newBuf) break;

                memcpy(newBuf, pData, aSStart + 4);
#if CP_REWRITE_UPDATE_LEADING_INT32
                *(int32_t*)(newBuf + aSStart) = *(const int32_t*)(pData + aSStart) + (int32_t)kDelta;
#endif
                *(int32_t*)(newBuf + aSStart + 4) = realUid;
                *(int32_t*)(newBuf + aSStart + 8) = realPid;
                memcpy(newBuf + aSStart + 12, pData + aSStart + 12, kPkgLenOffsetInAS - 12);
                *(int32_t*)(newBuf + pkgLenOffset) = (int32_t)kNewLen;
                memcpy(newBuf + pkgLenOffset + 4, kNew, kNewLen * sizeof(char16_t));
                *(char16_t*)(newBuf + pkgLenOffset + 4 + kNewLen * sizeof(char16_t)) = 0;
                memcpy(newBuf + pkgLenOffset + kNewTotal,
                       pData + suffixStart,
                       dataSize - suffixStart);

                void** dataField = (void**)((uint8_t*)parcel + 8);
                uint8_t* oldBuf  = (uint8_t*)*dataField;
                *dataField = newBuf;
                *(size_t*)((uint8_t*)parcel + 16) = newSize;
                *(size_t*)((uint8_t*)parcel + 24) = newSize;
                if (oldBuf) free(oldBuf);
                bufferSwapped = true;

                size_t newPos = (saved >= aSStart) ? (saved + kDelta) : saved;
                SafeSetPos(parcel, newPos);

                LOGD("CP viber rewrite fired: code=%d uid=%d->%d pid=%d->%d "
                     "pkg='com.viber.voip'->'com.ims134.hookengine' "
                     "size=%zu->%zu delta=%zu leading=%d(%s)",
                     code, f4, realUid, f8, realPid, dataSize, newSize,
                     kDelta, leading,
                     CP_REWRITE_UPDATE_LEADING_INT32 ? "updated" : "preserved");
                rewritten = true;
                break;

            } else {
                LOGD("CP iface matched pkg='%s' f4=%d f8=%d — unknown pkg, not rewriting (code=%d)",
                     ascii, f4, f8, code);
                break;
            }
        } else {
            break;
        }
    } while (0);

    if (!bufferSwapped) {
        SafeSetPos(parcel, saved);
    }
    return rewritten;
}

// ─── Feature 3.4 — ServiceManager override tables ─────────────────

#define MAX_SM_OVERRIDES 16
#define MAX_SM_NAME_LEN  128

struct SmNullOverride {
    bool used;
    char name[MAX_SM_NAME_LEN];
};

struct SmAliasOverride {
    bool used;
    char from[MAX_SM_NAME_LEN];
    char to[MAX_SM_NAME_LEN];
};

static SmNullOverride  g_sm_nulls[MAX_SM_OVERRIDES]   = {};
static SmAliasOverride g_sm_aliases[MAX_SM_OVERRIDES] = {};
static std::mutex      g_sm_mutex;
static std::atomic<int> g_sm_null_hits{0};
static std::atomic<int> g_sm_alias_hits{0};

static bool IsNullOverriddenUnlocked(const char* name) {
    for (auto& o : g_sm_nulls) {
        if (o.used && strcmp(o.name, name) == 0) return true;
    }
    return false;
}

static bool FindAliasUnlocked(const char* name, char* out_target, size_t out_size) {
    for (auto& a : g_sm_aliases) {
        if (a.used && strcmp(a.from, name) == 0) {
            strncpy(out_target, a.to, out_size - 1);
            out_target[out_size - 1] = '\0';
            return true;
        }
    }
    return false;
}

// Scan the parcel buffer for the canonical String16 byte pattern of `name`:
//   [int32 len=nameLen][char16*nameLen ascii][char16 null]
// returns pointer to the first content byte (right after the len prefix),
// or nullptr if not present. 4-byte stride matches Parcel's alignment.
static const char16_t* FindServiceNamePattern(void* parcel, const char* name) {
    if (!g_data || !g_dataSize || !name || !name[0]) return nullptr;
    size_t nameLen = strlen(name);
    if (nameLen >= MAX_SM_NAME_LEN) return nullptr;

    const uint8_t* data = g_data(parcel);
    size_t total = g_dataSize(parcel);
    size_t needed = 4 + nameLen * 2 + 2;
    if (!data || total < needed) return nullptr;

    char16_t pattern[MAX_SM_NAME_LEN];
    for (size_t i = 0; i < nameLen; i++) {
        pattern[i] = (char16_t)(uint8_t)name[i];
    }
    pattern[nameLen] = 0;

    int32_t lenPrefix = (int32_t)nameLen;
    size_t maxOffset = total - (nameLen * 2 + 2);
    for (size_t off = 4; off <= maxOffset; off += 4) {
        int32_t lp = *(const int32_t*)(data + off - 4);
        if (lp != lenPrefix) continue;
        if (memcmp(data + off, pattern, nameLen * 2) != 0) continue;
        if (*(const char16_t*)(data + off + nameLen * 2) != 0) continue;
        return (const char16_t*)(data + off);
    }
    return nullptr;
}

extern "C" bool ParcelRewriter_IsNullOverridden(const char* name) {
    if (!name || !name[0]) return false;
    std::lock_guard<std::mutex> lk(g_sm_mutex);
    return IsNullOverriddenUnlocked(name);
}

extern "C" bool ParcelRewriter_FindAlias(const char* name, char* out_target, size_t out_size) {
    if (!name || !name[0] || !out_target || out_size == 0) return false;
    std::lock_guard<std::mutex> lk(g_sm_mutex);
    return FindAliasUnlocked(name, out_target, out_size);
}

extern "C" bool ParcelRewriter_SetServiceManagerNull(const char* name) {
    if (!name || !name[0] || strlen(name) >= MAX_SM_NAME_LEN) return false;
    std::lock_guard<std::mutex> lk(g_sm_mutex);
    for (auto& o : g_sm_nulls) {
        if (o.used && strcmp(o.name, name) == 0) return true;
        if (!o.used) {
            o.used = true;
            strncpy(o.name, name, MAX_SM_NAME_LEN - 1);
            return true;
        }
    }
    return false;
}

extern "C" bool ParcelRewriter_SetServiceManagerAlias(const char* from, const char* to) {
    if (!from || !from[0] || !to || !to[0]) return false;
    if (strlen(from) >= MAX_SM_NAME_LEN || strlen(to) >= MAX_SM_NAME_LEN) return false;
    std::lock_guard<std::mutex> lk(g_sm_mutex);
    for (auto& a : g_sm_aliases) {
        if (a.used && strcmp(a.from, from) == 0) {
            strncpy(a.to, to, MAX_SM_NAME_LEN - 1);
            return true;
        }
        if (!a.used) {
            a.used = true;
            strncpy(a.from, from, MAX_SM_NAME_LEN - 1);
            strncpy(a.to, to, MAX_SM_NAME_LEN - 1);
            return true;
        }
    }
    return false;
}

extern "C" void ParcelRewriter_ClearServiceManagerOverrides() {
    std::lock_guard<std::mutex> lk(g_sm_mutex);
    memset(g_sm_nulls, 0, sizeof(g_sm_nulls));
    memset(g_sm_aliases, 0, sizeof(g_sm_aliases));
}

// ─── RewriteString16InPlace ──────────────────────────────────────
static bool RewriteString16InPlace(void* parcel,
                                   const char16_t* old_name, size_t old_len,
                                   const char16_t* new_name, size_t new_len) {
    const uint8_t* pData = g_data(parcel);
    if (!pData) return false;
    ptrdiff_t nameOffset = (const uint8_t*)old_name - pData;
    if (nameOffset < 4) return false;
    size_t nameLenOffset = (size_t)(nameOffset - 4);

    size_t old_str_bytes = old_len * 2;
    size_t old_total = (4 + old_str_bytes + 2 + 3) & ~(size_t)3;
    size_t new_str_bytes = new_len * 2;
    size_t new_total = (4 + new_str_bytes + 2 + 3) & ~(size_t)3;
    int64_t delta = (int64_t)new_total - (int64_t)old_total;

    size_t dataSize = g_dataSize(parcel);
    if (dataSize < nameLenOffset + old_total) return false;

    size_t newSize = (size_t)((int64_t)dataSize + delta);
    uint8_t* newBuf = (uint8_t*)malloc(newSize);
    if (!newBuf) return false;

    memcpy(newBuf, pData, nameLenOffset);
    *(int32_t*)(newBuf + nameLenOffset) = (int32_t)new_len;
    char16_t* dst = (char16_t*)(newBuf + nameLenOffset + 4);
    memcpy(dst, new_name, new_len * sizeof(char16_t));
    dst[new_len] = 0;
    size_t written = 4 + new_str_bytes + 2;
    if (written < new_total) {
        memset(newBuf + nameLenOffset + written, 0, new_total - written);
    }
    memcpy(newBuf + nameLenOffset + new_total,
           pData + nameLenOffset + old_total,
           dataSize - nameLenOffset - old_total);

    void** dataField = (void**)((uint8_t*)parcel + 8);
    uint8_t* oldBuf  = (uint8_t*)*dataField;
    *dataField = newBuf;
    *(size_t*)((uint8_t*)parcel + 16) = newSize;
    *(size_t*)((uint8_t*)parcel + 24) = newSize;
    if (oldBuf) free(oldBuf);
    SafeSetPos(parcel, newSize);
    return true;
}

// ─── Primary: transact-level SM alias / null rewrite ──────────────
//
// Called on every handle==0 transact regardless of code. On this ROM
// SM transacts fire on code=4 (not 1/2 as on stock AOSP); removing the
// code filter is essential. Pattern match is the discriminator — an
// accidental String16 match of a registered override name in an
// unrelated SM parcel is effectively impossible.
//
// Null overrides are implemented as a REQUEST rewrite: an overridden
// name is rewritten to '_NOEXIST_SM_OVERRIDE'. The driver's natural
// reply for an unknown service is exceptionCode=0 + null
// flat_binder_object — exactly what the caller expects for "not found".
// This avoids mutating the reply parcel, which is a PROT_READ binder
// mapping (mprotect RW → EACCES) whose native state cannot be reached
// from AfterServiceManagerLookup on this ROM.
//
// Null loop runs before the alias loop: if the same name is registered
// under both, null wins.

extern "C" void ParcelRewriter_TryRewriteServiceManagerLookup(jlong dataPtr, int code) {
    if (!g_ready || !g_data || !g_dataSize || !g_getPos) return;
    if (!dataPtr) return;

    void* parcel = (void*)dataPtr;

    std::lock_guard<std::mutex> lk(g_sm_mutex);

    // ─── Null overrides first ────────────────────────────────────────
    static const char16_t kNoexistName[] = u"_NOEXIST_SM_OVERRIDE";
    constexpr size_t kNoexistLen = sizeof(kNoexistName)/sizeof(char16_t) - 1;

    for (auto& o : g_sm_nulls) {
        if (!o.used) continue;
        const char16_t* old_name = FindServiceNamePattern(parcel, o.name);
        if (!old_name) continue;

        size_t from_len = strlen(o.name);
        if (RewriteString16InPlace(parcel, old_name, from_len,
                                   kNoexistName, kNoexistLen)) {
            g_sm_null_hits.fetch_add(1, std::memory_order_relaxed);
            LOGD("SM null override (transact): '%s' -> '_NOEXIST_SM_OVERRIDE' "
                 "(request rewrite, code=%d)", o.name, code);
        }
        return;
    }

    // ─── Alias overrides ────────────────────────────────────────────
    for (auto& a : g_sm_aliases) {
        if (!a.used) continue;
        const char16_t* old_name = FindServiceNamePattern(parcel, a.from);
        if (!old_name) continue;

        size_t from_len = strlen(a.from);
        size_t to_len   = strlen(a.to);
        char16_t to_buf[MAX_SM_NAME_LEN];
        for (size_t i = 0; i < to_len; i++) to_buf[i] = (char16_t)(uint8_t)a.to[i];
        to_buf[to_len] = 0;

        if (RewriteString16InPlace(parcel, old_name, from_len, to_buf, to_len)) {
            g_sm_alias_hits.fetch_add(1, std::memory_order_relaxed);
            LOGD("SM alias (transact): '%s' -> '%s' (code=%d)", a.from, a.to, code);
        }
        return;
    }
}

// ─── Post-transact hook — retired ────────────────────────────────
// Null override is now implemented pre-transact via request rewrite.
// Kept as a no-op symbol for ABI stability (extern in BinderTransactHook.cpp).
extern "C" void ParcelRewriter_AfterServiceManagerLookup(jlong dataPtr, jlong replyPtr, int code) {
    (void)dataPtr; (void)replyPtr; (void)code;
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

extern "C" int ParcelRewriter_SmNullHitCount() {
    return g_sm_null_hits.load(std::memory_order_relaxed);
}

extern "C" int ParcelRewriter_SmAliasHitCount() {
    return g_sm_alias_hits.load(std::memory_order_relaxed);
}

extern "C" void ParcelRewriter_IncrAliasHit() {
    g_sm_alias_hits.fetch_add(1, std::memory_order_relaxed);
}
extern "C" void ParcelRewriter_IncrNullHit() {
    g_sm_null_hits.fetch_add(1, std::memory_order_relaxed);
}