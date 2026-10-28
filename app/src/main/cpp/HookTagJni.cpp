//
// Created by ibrahim on 10/28/2026.
//
// Session 71 — Feature 6.2 JNI bridge.
//
// New file (not an addition to native-lib.cpp). JNI function names follow
// the standard mangling for NativeTestBridge externals, so this file is a
// parallel source of JNI exports rather than a replacement.
//
// Defines:
//   5 fixture (real, replacement, replacement-alt, GOT slot, counter) tuples.
//   installTagFixture / installTagFixtureAlt — call HookGotEntryExTagged.
//   callTagFixture — invokes whatever the slot currently points at.
//   getTagReplCalls / getTagAltCalls — counters.
//   resetTagFixtures — counters to 0 (does NOT touch slot values).
//   registryCountByTag / SnapshotByTag / UninstallByTag / ListTags — thin
//   wrappers over HookRegistry.cpp.

#include <jni.h>
#include <cstdio>
#include <cstdint>
#include <new>
#include <cstring>
#include <android/log.h>
#include "HookRegistry.h"

#define LOG_TAG "HookTagJni"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

// Real functions — initial values of the synthetic GOT slots.
    extern "C" int tagReal0(int x) { return 10 + x; }
    extern "C" int tagReal1(int x) { return 11 + x; }
    extern "C" int tagReal2(int x) { return 12 + x; }
    extern "C" int tagReal3(int x) { return 13 + x; }
    extern "C" int tagReal4(int x) { return 14 + x; }

// Primary replacements (return 1000+x) and alt replacements (return 3000+x).
// Alt exists so `conflictDetectionIgnoresTag` can force HookGotEntryEx past
// the `*gotAddress == newFunction` early return and into FindHandle — that
// is what actually pins the "FindHandle must stay tag-blind" constraint
// from PROJECT_STATE §6.2. Using the same replacement on the second install
// would return HOOK_ALREADY_HOOKED from the early return without ever
// touching FindHandle, making the test prove a weaker claim.
    volatile int g_tagReplCalls[5] = {0};
    volatile int g_tagAltCalls[5]  = {0};

    extern "C" int tagRepl0(int x) { __atomic_fetch_add(&g_tagReplCalls[0], 1, __ATOMIC_RELAXED); return 1000 + x; }
    extern "C" int tagRepl1(int x) { __atomic_fetch_add(&g_tagReplCalls[1], 1, __ATOMIC_RELAXED); return 1000 + x; }
    extern "C" int tagRepl2(int x) { __atomic_fetch_add(&g_tagReplCalls[2], 1, __ATOMIC_RELAXED); return 1000 + x; }
    extern "C" int tagRepl3(int x) { __atomic_fetch_add(&g_tagReplCalls[3], 1, __ATOMIC_RELAXED); return 1000 + x; }
    extern "C" int tagRepl4(int x) { __atomic_fetch_add(&g_tagReplCalls[4], 1, __ATOMIC_RELAXED); return 1000 + x; }

    extern "C" int tagAlt0(int x) { __atomic_fetch_add(&g_tagAltCalls[0], 1, __ATOMIC_RELAXED); return 3000 + x; }
    extern "C" int tagAlt1(int x) { __atomic_fetch_add(&g_tagAltCalls[1], 1, __ATOMIC_RELAXED); return 3000 + x; }
    extern "C" int tagAlt2(int x) { __atomic_fetch_add(&g_tagAltCalls[2], 1, __ATOMIC_RELAXED); return 3000 + x; }
    extern "C" int tagAlt3(int x) { __atomic_fetch_add(&g_tagAltCalls[3], 1, __ATOMIC_RELAXED); return 3000 + x; }
    extern "C" int tagAlt4(int x) { __atomic_fetch_add(&g_tagAltCalls[4], 1, __ATOMIC_RELAXED); return 3000 + x; }

    void* g_tagReals[5] = {
            (void*)&tagReal0, (void*)&tagReal1, (void*)&tagReal2,
            (void*)&tagReal3, (void*)&tagReal4
    };
    void* g_tagRepls[5] = {
            (void*)&tagRepl0, (void*)&tagRepl1, (void*)&tagRepl2,
            (void*)&tagRepl3, (void*)&tagRepl4
    };
    void* g_tagAlts[5] = {
            (void*)&tagAlt0, (void*)&tagAlt1, (void*)&tagAlt2,
            (void*)&tagAlt3, (void*)&tagAlt4
    };

// The synthetic GOT slots. Initial value = the real function; after a hook
// install, the value is the replacement; after uninstall, restored. These
// live in .data of libhookengine.so — always in /proc/self/maps — so
// HookGotEntryEx's ReadPageProt succeeds and mprotect(RW) is a no-op.
    void* g_tagSlots[5] = {
            (void*)&tagReal0, (void*)&tagReal1, (void*)&tagReal2,
            (void*)&tagReal3, (void*)&tagReal4
    };

    jint InstallSlot(JNIEnv* env, jint idx, jstring jtag, void* replacement) {
        if (idx < 0 || idx >= 5) return -1;
        const char* tag = nullptr;
        if (jtag != nullptr) {
            tag = env->GetStringUTFChars(jtag, nullptr);
            if (!tag) return -2;
        }
        HookHandle* out = nullptr;
        HookResult r = HookGotEntryExTagged(&g_tagSlots[idx], replacement, nullptr,
                                            "tag-fixture", "tagfix", tag, &out);
        if (jtag) env->ReleaseStringUTFChars(jtag, tag);
        LOGD("InstallSlot(%d, tag=%s, repl=%p) -> %d",
             (int)idx, tag ? tag : "<null>", replacement, (int)r);
        return (jint)r;
    }

} // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_installTagFixture(
        JNIEnv* env, jobject, jint idx, jstring jtag) {
    if (idx < 0 || idx >= 5) return -1;
    return InstallSlot(env, idx, jtag, g_tagRepls[idx]);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_installTagFixtureAlt(
        JNIEnv* env, jobject, jint idx, jstring jtag) {
    if (idx < 0 || idx >= 5) return -1;
    return InstallSlot(env, idx, jtag, g_tagAlts[idx]);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_callTagFixture(
        JNIEnv*, jobject, jint idx, jint x) {
    if (idx < 0 || idx >= 5) return -1;
    typedef int (*FnT)(int);
    FnT fn = (FnT)g_tagSlots[idx];
    if (!fn) return -2;
    return (jint)fn((int)x);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_getTagReplCalls(
        JNIEnv*, jobject, jint idx) {
    if (idx < 0 || idx >= 5) return -1;
    return (jint)__atomic_load_n(&g_tagReplCalls[idx], __ATOMIC_RELAXED);
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_getTagAltCalls(
        JNIEnv*, jobject, jint idx) {
    if (idx < 0 || idx >= 5) return -1;
    return (jint)__atomic_load_n(&g_tagAltCalls[idx], __ATOMIC_RELAXED);
}

extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_NativeTestBridge_resetTagFixtures(
        JNIEnv*, jobject) {
    for (int i = 0; i < 5; i++) {
        __atomic_store_n(&g_tagReplCalls[i], 0, __ATOMIC_RELAXED);
        __atomic_store_n(&g_tagAltCalls[i],  0, __ATOMIC_RELAXED);
        // Diagnostic only — if a slot is still patched, uninstallAll must
        // have failed earlier. Log so the test log shows it.
        if (g_tagSlots[i] != g_tagReals[i] &&
            g_tagSlots[i] != g_tagRepls[i] &&
            g_tagSlots[i] != g_tagAlts[i]) {
            LOGE("resetTagFixtures: slot %d has unexpected value %p",
                 i, g_tagSlots[i]);
        }
    }
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_NativeTestBridge_registryCountByTag(
        JNIEnv* env, jobject, jstring jtag) {
    const char* tag = "";
    if (jtag != nullptr) {
        tag = env->GetStringUTFChars(jtag, nullptr);
        if (!tag) return -1;
    }
    int n = RegistryCountByTag(tag);
    if (jtag) env->ReleaseStringUTFChars(jtag, tag);
    return (jint)n;
}

extern "C" JNIEXPORT jlongArray JNICALL
Java_com_ims134_hookengine_NativeTestBridge_registrySnapshotByTag(
        JNIEnv* env, jobject, jstring jtag, jint max) {
    if (max <= 0) return env->NewLongArray(0);
    if (max > 512) max = 512;
    const char* tag = "";
    if (jtag != nullptr) {
        tag = env->GetStringUTFChars(jtag, nullptr);
        if (!tag) return nullptr;
    }
    HookHandle* buf = new (std::nothrow) HookHandle[(size_t)max];
    if (!buf) {
        if (jtag) env->ReleaseStringUTFChars(jtag, tag);
        return nullptr;
    }
    int n = RegistrySnapshotByTag(tag, buf, (int)max);
    if (jtag) env->ReleaseStringUTFChars(jtag, tag);
    jlongArray arr = env->NewLongArray(n);
    if (arr && n > 0) {
        jlong* elems = env->GetLongArrayElements(arr, nullptr);
        if (elems) {
            for (int i = 0; i < n; i++) elems[i] = (jlong)(intptr_t)buf[i].target;
            env->ReleaseLongArrayElements(arr, elems, 0);
        }
    }
    delete[] buf;
    return arr;
}

extern "C" JNIEXPORT jintArray JNICALL
Java_com_ims134_hookengine_NativeTestBridge_registryUninstallByTag(
        JNIEnv* env, jobject, jstring jtag) {
    const char* tag = "";
    if (jtag != nullptr) {
        tag = env->GetStringUTFChars(jtag, nullptr);
        if (!tag) return nullptr;
    }
    int failures = 0;
    int removed = RegistryUninstallByTag(tag, &failures);
    if (jtag) env->ReleaseStringUTFChars(jtag, tag);
    jintArray arr = env->NewIntArray(2);
    if (!arr) return nullptr;
    jint elems[2] = { (jint)removed, (jint)failures };
    env->SetIntArrayRegion(arr, 0, 2, elems);
    return arr;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_ims134_hookengine_NativeTestBridge_registryListTags(
        JNIEnv* env, jobject) {
    char buf[2048];
    int n = RegistryListTags(buf, (int)sizeof(buf));
    if (n < 0) return env->NewStringUTF("");
    return env->NewStringUTF(buf);
}