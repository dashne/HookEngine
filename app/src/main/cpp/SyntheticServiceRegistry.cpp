#include "SyntheticServiceRegistry.h"
#include "HookRegistry.h"
#include <mutex>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <dlfcn.h>
#include <link.h>
#include <android/log.h>
#include <unistd.h>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

extern "C" HookResult InstallInlineHookEx(void*, void*, void**, const char*);
extern "C" HookResult UninstallInlineHook(void*);

extern "C" const uint8_t* ParcelRewriter_GetData(jlong parcelPtr);
extern "C" size_t         ParcelRewriter_GetSize(jlong parcelPtr);
extern "C" size_t         ParcelRewriter_GetObjectCount(jlong parcelPtr);
extern "C" size_t         ParcelRewriter_GetObjectOffset(jlong parcelPtr, size_t index);
extern "C" bool           ParcelRewriter_SetServiceManagerAlias(const char*, const char*);

#define MAX_SYNTH 16
#define MAX_NAME  128
#define FLAT_BINDER_OBJ_SIZE 24

struct SynthSlot {
    bool     used;
    char     name[MAX_NAME];
    uint8_t  flatBinderBytes[FLAT_BINDER_OBJ_SIZE];
    jobject  binderGlobal;
};

static SynthSlot      g_slots[MAX_SYNTH] = {};
static std::mutex     g_mutex;
static thread_local int  g_activeSlot = -1;
static thread_local bool g_isSynthetic = false;

static void*      g_target_fn = nullptr;
static jobject  (*_orig_readStrongBinder)(JNIEnv*, jclass, jlong) = nullptr;
static bool       g_hook_attempted = false;

// ─── helpers ────────────────────────────────────────────────────────

static bool PatternInParcel(jlong dataPtr, const char* name) {
    if (!name || !name[0]) return false;
    size_t nameLen = strlen(name);
    if (nameLen >= MAX_NAME) return false;
    const uint8_t* data = ParcelRewriter_GetData(dataPtr);
    size_t total = ParcelRewriter_GetSize(dataPtr);
    size_t needed = 4 + nameLen * 2 + 2;
    if (!data || total < needed) return false;
    int32_t lenPrefix = (int32_t)nameLen;
    size_t maxOffset = total - (nameLen * 2 + 2);
    for (size_t off = 4; off <= maxOffset; off += 4) {
        int32_t lp = *(const int32_t*)(data + off - 4);
        if (lp != lenPrefix) continue;
        bool match = true;
        for (size_t i = 0; i < nameLen; i++) {
            char16_t a = *(const char16_t*)(data + off + i * 2);
            char16_t b = (char16_t)(uint8_t)name[i];
            if (a != b) { match = false; break; }
        }
        if (!match) continue;
        char16_t nul = *(const char16_t*)(data + off + nameLen * 2);
        if (nul != 0) continue;
        return true;
    }
    return false;
}

// Get the [start, end) of a loaded library from /proc/self/maps.
static bool GetLibRange(const char* libSubstr, uintptr_t* outStart, uintptr_t* outEnd) {
    FILE* f = fopen("/proc/self/maps", "r");
    if (!f) return false;
    char line[1024];
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, libSubstr)) continue;
        unsigned long s, e;
        if (sscanf(line, "%lx-%lx", &s, &e) != 2) continue;
        if (s < lo) lo = s;
        if (e > hi) hi = e;
    }
    fclose(f);
    if (lo == UINTPTR_MAX || hi <= lo) return false;
    *outStart = lo;
    *outEnd = hi;
    return true;
}

// Scan a memory range for the JNINativeMethod entry whose `name` field
// points to a string equal to `methodName`. Struct is 24 bytes on arm64:
//   +0  const char* name
//   +8  const char* signature
//   +16 void*       fnPtr
//
// Two-pass:
//   Pass 1: find the string "methodName\0" in the range → strAddr.
//   Pass 2: scan the same range (8-byte aligned) for a value == strAddr.
//           That's the JNINativeMethod.name field. Read fnPtr at +16.
//
// This bypasses the need for the JNI function to be exported — the
// RegisterNatives registration puts the fn pointer into a struct in
// the .so's data section at load time. Symbol visibility is irrelevant.
static void* FindJniMethodFnByName(const char* libSubstr, const char* methodName) {
    uintptr_t start = 0, end = 0;
    if (!GetLibRange(libSubstr, &start, &end)) {
        LOGE("SynthSvc: %s not in /proc/self/maps", libSubstr);
        return nullptr;
    }
    LOGD("SynthSvc: %s range = 0x%lx-0x%lx (%lu MB)",
         libSubstr, (unsigned long)start, (unsigned long)end,
         (unsigned long)((end - start) / (1024 * 1024)));

    size_t nameLen = strlen(methodName);
    // Search for the literal string. Include trailing nul for full match.
    const char* found = nullptr;
    for (uintptr_t addr = start; addr + nameLen + 1 <= end; addr++) {
        if (memcmp((const void*)addr, methodName, nameLen) == 0 &&
            *(const char*)(addr + nameLen) == '\0') {
            found = (const char*)addr;
            break;
        }
    }
    if (!found) {
        LOGE("SynthSvc: string '%s' not found in %s", methodName, libSubstr);
        return nullptr;
    }
    uintptr_t strAddr = (uintptr_t)found;
    LOGD("SynthSvc: string '%s' at 0x%lx", methodName, (unsigned long)strAddr);

    // Second pass — find a pointer value equal to strAddr, 8-byte aligned.
    for (uintptr_t addr = start; addr + sizeof(void*) <= end; addr += 8) {
        uintptr_t val = *(const uintptr_t*)addr;
        if (val != strAddr) continue;
        // Candidate JNINativeMethod.name field at `addr`.
        // fnPtr is at addr + 16.
        if (addr + 24 > end) continue;
        uintptr_t fnPtr = *(const uintptr_t*)(addr + 16);
        if (fnPtr < 0x1000) continue;
        // fnPtr should be within some executable mapping — do a quick check.
        if (fnPtr < start || fnPtr >= end) {
            // Still might be valid if the code is elsewhere; accept but warn.
            LOGD("SynthSvc: fnPtr 0x%lx outside lib range — accepting", (unsigned long)fnPtr);
        }
        LOGD("SynthSvc: JNINativeMethod.name at 0x%lx → fnPtr 0x%lx",
             (unsigned long)addr, (unsigned long)fnPtr);
        return (void*)fnPtr;
    }
    LOGE("SynthSvc: no JNINativeMethod entry pointing at 0x%lx", (unsigned long)strAddr);
    return nullptr;
}

// ─── readStrongBinder handler (JNI ABI) ─────────────────────────────

static jobject FakeNativeReadStrongBinder(JNIEnv* env, jclass clazz, jlong nativePtr) {
    jobject orig = nullptr;
    if (_orig_readStrongBinder) {
        orig = _orig_readStrongBinder(env, clazz, nativePtr);
        if (env->ExceptionCheck()) return orig;
    }

    if (!g_isSynthetic) return orig;

    g_isSynthetic = false;
    int slot = g_activeSlot;
    g_activeSlot = -1;
    if (slot < 0 || slot >= MAX_SYNTH) return orig;

    jobject syntheticBinder = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_slots[slot].used && g_slots[slot].binderGlobal) {
            syntheticBinder = env->NewLocalRef(g_slots[slot].binderGlobal);
        }
    }
    if (!syntheticBinder) {
        LOGD("SynthSvc: slot %d empty, passthrough", slot);
        return orig;
    }

    if (orig) env->DeleteLocalRef(orig);
    LOGD("SynthSvc: readStrongBinder returned synthetic Binder for slot %d", slot);
    return syntheticBinder;
}

extern "C" int SyntheticServiceRegistry_InstallReadStrongBinderHook(JNIEnv* env) {
    if (!env) return -1;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_target_fn) return 0;
    if (g_hook_attempted) return -2;
    g_hook_attempted = true;

    // Try dlsym first — cheap. On this ROM the symbol isn't exported, so
    // this will fail; fall through to memory scan.
    const char* methodName = "nativeReadStrongBinder";
    g_target_fn = dlsym(RTLD_DEFAULT, "android_os_Parcel_nativeReadStrongBinder");
    if (g_target_fn) {
        LOGD("SynthSvc: dlsym android_os_Parcel_nativeReadStrongBinder = %p", g_target_fn);
    } else {
        // Memory scan across the likely libraries.
        const char* libs[] = {
                "libandroid_runtime.so",
                "libframework-runtime.so",
                "libframework-connectivity-tiramisu-jni.so",
                nullptr
        };
        for (int i = 0; libs[i] && !g_target_fn; i++) {
            g_target_fn = FindJniMethodFnByName(libs[i], methodName);
            if (g_target_fn) {
                LOGD("SynthSvc: found in %s at %p", libs[i], g_target_fn);
            }
        }
    }

    if (!g_target_fn) {
        LOGE("SynthSvc: nativeReadStrongBinder JNI impl not found via dlsym or scan");
        return -10;
    }

    void* orig = nullptr;
    HookResult r = InstallInlineHookEx(g_target_fn,
                                       (void*)FakeNativeReadStrongBinder,
                                       &orig, "nativeReadStrongBinder");
    if (r != HOOK_OK && r != HOOK_ALREADY_HOOKED) {
        LOGE("SynthSvc: InstallInlineHookEx r=%d", (int)r);
        g_target_fn = nullptr;
        return -11;
    }
    _orig_readStrongBinder = (decltype(_orig_readStrongBinder))orig;
    LOGD("SynthSvc: nativeReadStrongBinder hooked target=%p orig=%p",
         g_target_fn, orig);
    return 0;
}

// ─── registry ────────────────────────────────────────────────────────

extern "C" int SyntheticServiceRegistry_Add(JNIEnv* env, const char* name,
                                            const uint8_t* flatBinderBytes,
                                            size_t bytesLen, jobject binder) {
    if (!env || !name || !name[0] || !flatBinderBytes || !binder) return -1;
    if (bytesLen != FLAT_BINDER_OBJ_SIZE) {
        LOGE("SynthSvc: bytes len %zu != %d", bytesLen, FLAT_BINDER_OBJ_SIZE);
        return -1;
    }
    if (strlen(name) >= MAX_NAME) return -1;

    SyntheticServiceRegistry_InstallReadStrongBinderHook(env);

    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& s : g_slots) {
        if (s.used && strcmp(s.name, name) == 0) {
            LOGE("SynthSvc: '%s' already registered", name);
            return -2;
        }
    }
    for (auto& s : g_slots) {
        if (s.used) continue;
        jobject g = env->NewGlobalRef(binder);
        if (!g) return -3;
        s.used = true;
        strncpy(s.name, name, MAX_NAME - 1);
        s.name[MAX_NAME - 1] = '\0';
        memcpy(s.flatBinderBytes, flatBinderBytes, FLAT_BINDER_OBJ_SIZE);
        s.binderGlobal = g;
        ParcelRewriter_SetServiceManagerAlias(name, "activity");
        LOGD("SynthSvc: added '%s'", name);
        return 0;
    }
    return -4;
}

extern "C" int SyntheticServiceRegistry_Remove(JNIEnv* env, const char* name) {
    if (!name || !name[0]) return -1;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& s : g_slots) {
        if (!s.used || strcmp(s.name, name) != 0) continue;
        if (s.binderGlobal && env) env->DeleteGlobalRef(s.binderGlobal);
        s.binderGlobal = nullptr;
        memset(s.name, 0, sizeof(s.name));
        memset(s.flatBinderBytes, 0, sizeof(s.flatBinderBytes));
        s.used = false;
        ParcelRewriter_SetServiceManagerAlias(name, "_NOEXIST_SM_OVERRIDE");
        LOGD("SynthSvc: removed '%s'", name);
        return 0;
    }
    return -2;
}

extern "C" int SyntheticServiceRegistry_Count() {
    std::lock_guard<std::mutex> lk(g_mutex);
    int n = 0;
    for (auto& s : g_slots) if (s.used) n++;
    return n;
}

extern "C" void SyntheticServiceRegistry_ClearAll(JNIEnv* env) {
    std::lock_guard<std::mutex> lk(g_mutex);
    for (auto& s : g_slots) {
        if (!s.used) continue;
        if (s.binderGlobal && env) env->DeleteGlobalRef(s.binderGlobal);
        s.binderGlobal = nullptr;
        memset(s.name, 0, sizeof(s.name));
        memset(s.flatBinderBytes, 0, sizeof(s.flatBinderBytes));
        s.used = false;
    }
}

extern "C" void SyntheticServiceRegistry_PreIntercept(jlong dataPtr, int code) {
    g_isSynthetic = false;
    g_activeSlot = -1;
    if (code != 1 && code != 4) return;
    if (!dataPtr) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    for (int i = 0; i < MAX_SYNTH; i++) {
        if (!g_slots[i].used) continue;
        if (PatternInParcel(dataPtr, g_slots[i].name)) {
            g_isSynthetic = true;
            g_activeSlot = i;
            LOGD("SynthSvc: pre matched '%s'", g_slots[i].name);
            return;
        }
    }
}

extern "C" void SyntheticServiceRegistry_PostIntercept(jlong replyPtr) {
    if (!g_isSynthetic) return;
    const uint8_t* data = ParcelRewriter_GetData(replyPtr);
    size_t size = ParcelRewriter_GetSize(replyPtr);
    int32_t ex = (data && size >= 4) ? *(const int32_t*)data : -1;
    LOGD("SynthSvc: PostIntercept slot=%d size=%zu ex=%d — state retained",
         g_activeSlot, size, ex);
}

// ─── JNI bridges ─────────────────────────────────────────────────────

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_syntheticAdd(
        JNIEnv* env, jobject, jstring jname, jobject jbinder) {
    if (!env || !jname || !jbinder) return -1;
    const char* name = env->GetStringUTFChars(jname, nullptr);
    if (!name) return -1;
    jclass parcelCls = env->FindClass("android/os/Parcel");
    if (!parcelCls) {
        env->ExceptionClear();
        env->ReleaseStringUTFChars(jname, name);
        return -10;
    }
    jmethodID obtainM   = env->GetStaticMethodID(parcelCls, "obtain", "()Landroid/os/Parcel;");
    jmethodID writeM    = env->GetMethodID(parcelCls, "writeStrongBinder", "(Landroid/os/IBinder;)V");
    jmethodID recycleM  = env->GetMethodID(parcelCls, "recycle", "()V");
    jfieldID  mNativePtrF = env->GetFieldID(parcelCls, "mNativePtr", "J");
    if (!obtainM || !writeM || !recycleM || !mNativePtrF) {
        env->ExceptionClear();
        env->DeleteLocalRef(parcelCls);
        env->ReleaseStringUTFChars(jname, name);
        return -11;
    }
    jobject parcel = env->CallStaticObjectMethod(parcelCls, obtainM);
    if (env->ExceptionCheck() || !parcel) {
        env->ExceptionClear();
        env->DeleteLocalRef(parcelCls);
        env->ReleaseStringUTFChars(jname, name);
        return -12;
    }
    env->CallVoidMethod(parcel, writeM, jbinder);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->CallVoidMethod(parcel, recycleM);
        env->DeleteLocalRef(parcel);
        env->DeleteLocalRef(parcelCls);
        env->ReleaseStringUTFChars(jname, name);
        return -13;
    }
    jlong nativePtr = env->GetLongField(parcel, mNativePtrF);
    uint8_t flatBinderBytes[FLAT_BINDER_OBJ_SIZE];
    bool ok = false;
    if (nativePtr != 0) {
        const uint8_t* dataPtr = ParcelRewriter_GetData(nativePtr);
        size_t dataSize = ParcelRewriter_GetSize(nativePtr);
        size_t capCount = ParcelRewriter_GetObjectCount(nativePtr);
        size_t capOff   = (capCount > 0)
                          ? ParcelRewriter_GetObjectOffset(nativePtr, 0)
                          : (size_t)-1;
        LOGD("SynthSvc: capture dataSize=%zu objCount=%zu off[0]=%zd",
             dataSize, capCount, (ptrdiff_t)capOff);
        if (dataPtr && dataSize >= FLAT_BINDER_OBJ_SIZE) {
            if (capCount > 0 && capOff != (size_t)-1 &&
                capOff + FLAT_BINDER_OBJ_SIZE <= dataSize) {
                memcpy(flatBinderBytes, dataPtr + capOff, FLAT_BINDER_OBJ_SIZE);
                uint32_t type      = *(uint32_t*)flatBinderBytes;
                uint32_t flags_val = *(uint32_t*)(flatBinderBytes + 4);
                uint64_t bv        = *(uint64_t*)(flatBinderBytes + 8);
                uint64_t cv        = *(uint64_t*)(flatBinderBytes + 16);
                LOGD("SynthSvc: captured off=%zu type=0x%08x flags=0x%08x "
                     "binder=0x%016lx cookie=0x%016lx",
                     capOff, type, flags_val,
                     (unsigned long)bv, (unsigned long)cv);
                if (type == 1 || type == 2) {
                    ok = true;
                } else if (capOff == 0 &&
                           (flags_val & 0x100) != 0 &&
                           bv > 0x1000 && cv > 0x1000) {
                    LOGE("SynthSvc: type=0x%08x corrupt at off=0 — forcing", type);
                    flatBinderBytes[0] = 1; flatBinderBytes[1] = 0;
                    flatBinderBytes[2] = 0; flatBinderBytes[3] = 0;
                    ok = true;
                }
            } else {
                memcpy(flatBinderBytes, dataPtr, FLAT_BINDER_OBJ_SIZE);
                uint32_t type      = *(uint32_t*)flatBinderBytes;
                uint32_t flags_val = *(uint32_t*)(flatBinderBytes + 4);
                uint64_t bv        = *(uint64_t*)(flatBinderBytes + 8);
                uint64_t cv        = *(uint64_t*)(flatBinderBytes + 16);
                LOGE("SynthSvc: obj table unusable, off0 fallback "
                     "type=0x%08x flags=0x%08x", type, flags_val);
                if (type == 1 || type == 2) ok = true;
                else if ((flags_val & 0x100) != 0 && bv > 0x1000 && cv > 0x1000) {
                    flatBinderBytes[0] = 1; flatBinderBytes[1] = 0;
                    flatBinderBytes[2] = 0; flatBinderBytes[3] = 0;
                    ok = true;
                }
            }
        }
    }
    env->CallVoidMethod(parcel, recycleM);
    env->DeleteLocalRef(parcel);
    env->DeleteLocalRef(parcelCls);
    if (!ok) {
        env->ReleaseStringUTFChars(jname, name);
        return -14;
    }
    int rc = SyntheticServiceRegistry_Add(env, name, flatBinderBytes,
                                          FLAT_BINDER_OBJ_SIZE, jbinder);
    env->ReleaseStringUTFChars(jname, name);
    return (jint)rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_syntheticRemove(
        JNIEnv* env, jobject, jstring jname) {
    if (!env || !jname) return -1;
    const char* name = env->GetStringUTFChars(jname, nullptr);
    if (!name) return -1;
    int rc = SyntheticServiceRegistry_Remove(env, name);
    env->ReleaseStringUTFChars(jname, name);
    return (jint)rc;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_syntheticCount(JNIEnv*, jobject) {
    return (jint)SyntheticServiceRegistry_Count();
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_syntheticClearAll(JNIEnv* env, jobject) {
    SyntheticServiceRegistry_ClearAll(env);
}