// CppParcelRewriter.cpp
//
// AIDL prelude rewriter for IStorageManager + IContentProvider.
//
// Session 23 — Wall B (IStorageManager callingPackage rewrite).
// Session 28 — Wall C2 (IContentProvider pkg+uid rewrite for viber).
// Session 29 — Wall C3 diagnostic (miui.* ifaces, capped).
// Session 32 — uid resolved via getuid() at call time.
// Session 33 — two bugs from Session 32 fixed:
//   1. getpid() from libhookengine returns spoofed 10000 (Fake_getpid
//      inline hook on libc body fires for all callers). getuid() works
//      by accident (GOT hook on libjavacore/libopenjdk, not libhookengine).
//      Fix: use syscall(SYS_getpid) / syscall(SYS_getuid) — bypass both
//      inline and GOT. Prior prewarm diag showed f4=14755 f8=10431 (real),
//      and we clobbered f4 with 10000 (spoofed), breaking prewarm.
//   2. No skip-if-correct. Prewarm already had correct values; we rewrote
//      unnecessarily. Fix: only rewrite when fields differ from targets.
//
// AttributionSource layout on API 37 (confirmed by diag):
//   own-pkg:  [leading][pid][uid][tagLen][pkgLen][pkg]
//   viber:    [leading][uid][pid][tagLen][pkgLen][pkg]
// Two construction paths, two field orders. Route by pkg.

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

    // syscall, not getpid()/getuid() — inline hook on getpid body fires for
    // libhookengine callers, returning spoofed g_virtualPid (10000).
    // getuid() works by accident (GOT-only hook, libhookengine skipped),
    // but use syscall for both to be explicit and future-proof.
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

        // ── Wall C3 diagnostic (miui.* iface dump, capped 3) ──
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
        constexpr size_t kOldLen = sizeof(kOld)/sizeof(char16_t) - 1;  // 14
        constexpr size_t kNewLen = sizeof(kNew)/sizeof(char16_t) - 1;  // 21
        constexpr size_t kOldTotal = 4 + 2*kOldLen + 2 + 2;  // 36
        constexpr size_t kNewTotal = 4 + 2*kNewLen + 2 + 0;  // 48
        constexpr size_t kDelta    = kNewTotal - kOldTotal;  // 12

        if (ifaceLen == kStorageIfaceLen &&
            memcmp(iface, kStorageIface, kStorageIfaceLen * sizeof(char16_t)) == 0) {
            // ── IStorageManager (Wall B) — unchanged ──
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
            // ── IContentProvider (Wall C2 + Wall C2b) ──
            // Layouts confirmed:
            //   own-pkg:  [leading][pid][uid][tagLen][pkgLen][pkg]
            //   viber:    [leading][uid][pid][tagLen][pkgLen][pkg]
            // Route by pkg. Skip rewrite if already correct.

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

            // Diagnostic: own-pkg calls (first 5)
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
            constexpr size_t kSuffixStartInAS  = kPkgLenOffsetInAS + kOldTotal;  // 52

            if (pkgIsOwn) {
                // ── Wall C2b: own-pkg, [pid][uid] ──
                // Write realPid to [+4], realUid to [+8]. Same-size swap.
                // Skip if already correct (prewarm case — f4=realPid, f8=realUid
                // observed in prior diag; our Session 32 code broke this by
                // overwriting f4 with spoofed 10000).
                if (f4 == realPid && f8 == realUid) {
                    LOGD("CP own-pkg already correct (pid=%d uid=%d) — skip rewrite (code=%d)",
                         realPid, realUid, code);
                    break;
                }
                size_t newSize = dataSize;
                uint8_t* newBuf = (uint8_t*)malloc(newSize);
                if (!newBuf) break;

                memcpy(newBuf, pData, aSStart + 4);       // prefix incl leading
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
                // ── Wall C2: Viber, [uid][pid] ──
                // Write realUid to [+4] (uid slot), realPid to [+8] (pid slot).
                // Buffer grows by kDelta (pkg 14→21 chars). Always rewrite
                // because pkg must be replaced regardless of uid/pid state.
                size_t pkgLenOffset = aSStart + kPkgLenOffsetInAS;
                size_t suffixStart  = aSStart + kSuffixStartInAS;
                if (dataSize < suffixStart) break;

                size_t newSize = dataSize + kDelta;
                uint8_t* newBuf = (uint8_t*)malloc(newSize);
                if (!newBuf) break;

                memcpy(newBuf, pData, aSStart + 4);       // prefix incl leading
#if CP_REWRITE_UPDATE_LEADING_INT32
                *(int32_t*)(newBuf + aSStart) = *(const int32_t*)(pData + aSStart) + (int32_t)kDelta;
#endif
                *(int32_t*)(newBuf + aSStart + 4) = realUid;   // uid slot
                *(int32_t*)(newBuf + aSStart + 8) = realPid;   // pid slot
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