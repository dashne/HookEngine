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
//
// Session 23 — Wall B: ParcelRewriter_TryRewriteStorageRequest added to
// intercept IStorageManager transacts and rewrite callingPackage
// "com.viber.voip" -> "com.ims134.hookengine" so AMS accepts.
//
// Session 28 — Wall C2: function renamed to ParcelRewriter_TryRewriteCallerIdentity
// and extended to intercept IContentProvider transacts. Rewrites
// AttributionSource uid 10000 -> 10428 and packageName
// "com.viber.voip" -> "com.ims134.hookengine". Layout on API 37 is
// empirically different from AOSP: leading int32 (92) precedes uid;
// tag field comes between pid and packageName. Preserved on attempt 1;
// toggle CP_REWRITE_UPDATE_LEADING_INT32 for attempt 2 if server rejects.

#include <jni.h>
#include <android/log.h>
#include <dlfcn.h>
#include <link.h>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <cstdlib>

// Session 28 — API 37 AttributionSource layout has an unexplained leading
// int32 (value 92) before uid. If the server validates it as a length,
// preserving it causes a mismatch after the pkg rewrite grows the buffer.
// Attempt 1 preserves it. If server rejects after "CP rewrite fired" logs,
// flip to 1 and rebuild for attempt 2.
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

// ─── Session 28 — Wall C2 (IContentProvider) + Wall B (IStorageManager) ──
// Renamed from TryRewriteStorageRequest. Handles both AIDL interfaces.
// Pure C++, no JNI on caller thread (Rule 8).
extern "C" bool ParcelRewriter_TryRewriteCallerIdentity(jlong dataPtr, int code) {
    if (!g_ready || !g_data || !g_dataSize || !g_getPos) return false;
    if (!dataPtr) return false;

    void* parcel = (void*)dataPtr;
    size_t saved = g_getPos(parcel);
    SafeSetPos(parcel, 0);

    bool rewritten = false;
    do {
        int32_t strict, worksource, magic;
        if (!SafeReadInt32(parcel, &strict))     break;
        if (!SafeReadInt32(parcel, &worksource)) break;
        if (!SafeReadInt32(parcel, &magic))      break;
        if (magic != kMagic) break;

        size_t ifaceLen = 0;
        const char16_t* iface = SafeReadString16(parcel, &ifaceLen);
        if (!iface) break;

        static const char16_t kStorageIface[] = u"android.os.storage.IStorageManager";
        constexpr size_t kStorageIfaceLen = sizeof(kStorageIface)/sizeof(char16_t) - 1;
        static const char16_t kContentProviderIface[] = u"android.content.IContentProvider";
        constexpr size_t kContentProviderIfaceLen = sizeof(kContentProviderIface)/sizeof(char16_t) - 1;

        // Common rewrite constants (both branches).
        static const char16_t kOld[] = u"com.viber.voip";
        static const char16_t kNew[] = u"com.ims134.hookengine";
        constexpr size_t kOldLen = sizeof(kOld)/sizeof(char16_t) - 1;  // 14
        constexpr size_t kNewLen = sizeof(kNew)/sizeof(char16_t) - 1;  // 21
        constexpr size_t kOldTotal = 4 + 2*kOldLen + 2 + 2;  // 36
        constexpr size_t kNewTotal = 4 + 2*kNewLen + 2 + 0;  // 48
        constexpr size_t kDelta    = kNewTotal - kOldTotal;  // 12

        if (ifaceLen == kStorageIfaceLen &&
            memcmp(iface, kStorageIface, kStorageIfaceLen * sizeof(char16_t)) == 0) {
            // ── IStorageManager branch (Session 23, Wall B) ──
            // Unchanged from Session 23. Handles getVolumeList and siblings.

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

            size_t newPos = (saved > oldStringStart) ? (saved + kDelta) : saved;
            SafeSetPos(parcel, newPos);

            LOGD("STORAGE rewrite fired: code=%d userId=%d size=%zu->%zu delta=%zu",
                 code, userId, dataSize, newSize, kDelta);
            rewritten = true;

        } else if (ifaceLen == kContentProviderIfaceLen &&
                   memcmp(iface, kContentProviderIface, kContentProviderIfaceLen * sizeof(char16_t)) == 0) {
            // ── IContentProvider branch (Session 28, Wall C2) ──
            // Empirical AttributionSource layout on API 37 (from Session 27
            // dump 5c000000 10270000 10270000 00000000 0e000000 63006f00...):
            //   [+0]  int32   leading prefix (92) — purpose unknown, PRESERVE
            //   [+4]  int32   uid          ← rewrite target
            //   [+8]  int32   pid
            //   [+12] int32   tag length (0)
            //   [+16] int32   pkg length (14)
            //   [+20] chars   pkg chars UTF-16
            //   [+48] u16     null
            //   [+50] u16     pad
            //   [+52] ...     suffix (next chain, permission owner, ...)

            size_t aSStart = g_getPos(parcel);
            size_t dataSize = g_dataSize(parcel);
            const uint8_t* pData = g_data(parcel);
            if (!pData || aSStart + 20 > dataSize) break;

            int32_t leading = 0;
            if (!SafeReadInt32(parcel, &leading)) break;
            int32_t uid = 0;
            if (!SafeReadInt32(parcel, &uid)) break;
            int32_t pid = 0;
            if (!SafeReadInt32(parcel, &pid)) break;
            (void)pid;
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

            if (uid != 10000 || cpLen != kOldLen ||
                memcmp(cp, kOld, kOldLen * sizeof(char16_t)) != 0) {
                LOGD("CP iface matched uid=%d pkg='%s' — not rewriting (code=%d)",
                     uid, ascii, code);
                break;
            }

            constexpr size_t kUidOffsetInAS     = 4;
            constexpr size_t kPkgLenOffsetInAS  = 16;
            constexpr size_t kSuffixStartInAS   = kPkgLenOffsetInAS + kOldTotal;  // 16 + 36 = 52

            size_t pkgLenOffset = aSStart + kPkgLenOffsetInAS;
            size_t suffixStart  = aSStart + kSuffixStartInAS;
            if (dataSize < suffixStart) break;

            size_t newSize = dataSize + kDelta;
            uint8_t* newBuf = (uint8_t*)malloc(newSize);
            if (!newBuf) break;

            // 1. Copy everything up to and including the leading int32.
            memcpy(newBuf, pData, aSStart + kUidOffsetInAS);

            // 2. Optionally update the leading int32 (attempt 2 toggle).
#if CP_REWRITE_UPDATE_LEADING_INT32
            *(int32_t*)(newBuf + aSStart) = *(const int32_t*)(pData + aSStart) + (int32_t)kDelta;
#endif

            // 3. Rewrite uid.
            *(int32_t*)(newBuf + aSStart + kUidOffsetInAS) = 10428;

            // 4. Copy pid + tag length (bytes [+8..+16), 8 bytes unchanged).
            memcpy(newBuf + aSStart + 8, pData + aSStart + 8, kPkgLenOffsetInAS - 8);

            // 5. Write new pkg string16 at [+16].
            *(int32_t*)(newBuf + pkgLenOffset) = (int32_t)kNewLen;
            memcpy(newBuf + pkgLenOffset + 4, kNew, kNewLen * sizeof(char16_t));
            *(char16_t*)(newBuf + pkgLenOffset + 4 + kNewLen * sizeof(char16_t)) = 0;

            // 6. Copy suffix shifted by delta.
            memcpy(newBuf + pkgLenOffset + kNewTotal,
                   pData + suffixStart,
                   dataSize - suffixStart);

            // 7. Swap buffer.
            void** dataField = (void**)((uint8_t*)parcel + 8);
            uint8_t* oldBuf  = (uint8_t*)*dataField;
            *dataField = newBuf;
            *(size_t*)((uint8_t*)parcel + 16) = newSize;
            *(size_t*)((uint8_t*)parcel + 24) = newSize;
            if (oldBuf) free(oldBuf);

            size_t newPos = (saved >= aSStart) ? (saved + kDelta) : saved;
            SafeSetPos(parcel, newPos);

            LOGD("CP rewrite fired: code=%d uid=10000->10428 pkg='%s'->'com.ims134.hookengine' "
                 "size=%zu->%zu delta=%zu leading=%d(%s)",
                 code, ascii, dataSize, newSize, kDelta, leading,
                 CP_REWRITE_UPDATE_LEADING_INT32 ? "updated" : "preserved");
            rewritten = true;

        } else {
            break;
        }
    } while (0);

    if (!rewritten) {
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