// NativeFnln.cpp — Session 54. Feature 4.2. Rule 15 debt closure.
//
// Native implementation of ClassLoader chaining. Replaces the Kotlin
// ClassLoaderBridge.chain() reflection logic with C++ orchestration:
// read guest.pathList.dexElements, read host.pathList.dexElements,
// dedupe host elements (by identity), prepend host-only elements to
// guest's array, set it back via Field.set.
//
// The reflection needed to bypass hidden-api on `pathList` and
// `dexElements` (both private fields) goes through HiddenApiBridge
// (Kotlin object) via JNI — same pattern ParcelRewriter.cpp already
// uses for its reflective field accesses.
//
// Returns:
//   0   success (hostOnly count, or 0 if guest === host or no-op)
//   -1  invalid args
//   -2  pathList field not found
//   -3  dexElements field not found
//   -4  Field.get / Field.set methodIDs not found
//   -5  pathList read failed
//   -6  dexElements read failed
//   -7  element class lookup failed
//   -8  merged array allocation failed
//   -9  Field.set threw

#include <jni.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <android/log.h>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

// ─── HiddenApiBridge reflection helpers ──────────────────────────────

static jobject habGetField(JNIEnv* env, jclass targetCls, const char* fieldName) {
    jclass habCls = env->FindClass("com/ims134/hookengine/HiddenApiBridge");
    if (!habCls) { env->ExceptionClear(); return nullptr; }
    jfieldID instFid = env->GetStaticFieldID(habCls, "INSTANCE",
                                             "Lcom/ims134/hookengine/HiddenApiBridge;");
    if (!instFid) { env->ExceptionClear(); env->DeleteLocalRef(habCls); return nullptr; }
    jobject inst = env->GetStaticObjectField(habCls, instFid);
    if (!inst) { env->ExceptionClear(); env->DeleteLocalRef(habCls); return nullptr; }
    jmethodID refM = env->GetMethodID(habCls, "reflectGetDeclaredField",
                                      "(Ljava/lang/Class;Ljava/lang/String;)Ljava/lang/reflect/Field;");
    if (!refM) {
        env->ExceptionClear();
        env->DeleteLocalRef(inst);
        env->DeleteLocalRef(habCls);
        return nullptr;
    }
    jstring jname = env->NewStringUTF(fieldName);
    jobject field = env->CallObjectMethod(inst, refM, targetCls, jname);
    env->DeleteLocalRef(jname);
    env->DeleteLocalRef(inst);
    env->DeleteLocalRef(habCls);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return field;
}

static void habSetAccessible(JNIEnv* env, jobject field) {
    if (!field) return;
    jclass habCls = env->FindClass("com/ims134/hookengine/HiddenApiBridge");
    if (!habCls) { env->ExceptionClear(); return; }
    jfieldID instFid = env->GetStaticFieldID(habCls, "INSTANCE",
                                             "Lcom/ims134/hookengine/HiddenApiBridge;");
    if (!instFid) { env->ExceptionClear(); env->DeleteLocalRef(habCls); return; }
    jobject inst = env->GetStaticObjectField(habCls, instFid);
    jmethodID setAccM = env->GetMethodID(habCls, "reflectSetAccessible",
                                         "(Ljava/lang/Object;)Z");
    if (setAccM && inst) env->CallBooleanMethod(inst, setAccM, field);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (inst) env->DeleteLocalRef(inst);
    env->DeleteLocalRef(habCls);
}

static jobject habFindFieldInHierarchy(JNIEnv* env, jclass cls, const char* fieldName) {
    jclass classCls = env->FindClass("java/lang/Class");
    if (!classCls) { env->ExceptionClear(); return nullptr; }
    jmethodID getSuper = env->GetMethodID(classCls, "getSuperclass", "()Ljava/lang/Class;");

    jclass currentCls = (jclass)env->NewLocalRef(cls);
    jobject result = nullptr;
    while (currentCls) {
        jobject field = habGetField(env, currentCls, fieldName);
        if (field) { result = field; break; }
        jclass sup = getSuper ? (jclass)env->CallObjectMethod(currentCls, getSuper) : nullptr;
        if (env->ExceptionCheck()) { env->ExceptionClear(); sup = nullptr; }
        env->DeleteLocalRef(currentCls);
        currentCls = sup;
    }
    if (currentCls && !result) env->DeleteLocalRef(currentCls);
    env->DeleteLocalRef(classCls);
    if (result) habSetAccessible(env, result);
    return result;
}

// ─── Main implementation ─────────────────────────────────────────────

static int fnln_impl(JNIEnv* env, jobject guest, jobject host) {
    if (!env || !guest || !host) return -1;
    if (env->IsSameObject(guest, host)) return 0;

    jclass guestCls = env->GetObjectClass(guest);
    if (!guestCls) { env->ExceptionClear(); return -1; }

    jobject pathListField = habFindFieldInHierarchy(env, guestCls, "pathList");
    env->DeleteLocalRef(guestCls);
    if (!pathListField) { LOGE("fnln: pathList field not found"); return -2; }

    jclass dexPathListCls = env->FindClass("dalvik/system/DexPathList");
    if (!dexPathListCls) {
        env->ExceptionClear();
        env->DeleteLocalRef(pathListField);
        return -3;
    }
    jobject dexElementsField = habGetField(env, dexPathListCls, "dexElements");
    env->DeleteLocalRef(dexPathListCls);
    if (!dexElementsField) {
        LOGE("fnln: dexElements field not found");
        env->DeleteLocalRef(pathListField);
        return -3;
    }
    habSetAccessible(env, dexElementsField);

    jclass fieldCls = env->FindClass("java/lang/reflect/Field");
    if (!fieldCls) {
        env->ExceptionClear();
        env->DeleteLocalRef(pathListField);
        env->DeleteLocalRef(dexElementsField);
        return -4;
    }
    jmethodID fieldGet = env->GetMethodID(fieldCls, "get",
                                          "(Ljava/lang/Object;)Ljava/lang/Object;");
    jmethodID fieldSet = env->GetMethodID(fieldCls, "set",
                                          "(Ljava/lang/Object;Ljava/lang/Object;)V");
    if (!fieldGet || !fieldSet) {
        env->ExceptionClear();
        env->DeleteLocalRef(fieldCls);
        env->DeleteLocalRef(pathListField);
        env->DeleteLocalRef(dexElementsField);
        return -4;
    }

    jobject guestPathList = env->CallObjectMethod(pathListField, fieldGet, guest);
    jobject hostPathList  = env->CallObjectMethod(pathListField, fieldGet, host);
    if (env->ExceptionCheck() || !guestPathList || !hostPathList) {
        env->ExceptionClear();
        if (guestPathList) env->DeleteLocalRef(guestPathList);
        if (hostPathList) env->DeleteLocalRef(hostPathList);
        env->DeleteLocalRef(fieldCls);
        env->DeleteLocalRef(pathListField);
        env->DeleteLocalRef(dexElementsField);
        return -5;
    }

    jobjectArray guestArr = (jobjectArray)env->CallObjectMethod(dexElementsField, fieldGet, guestPathList);
    jobjectArray hostArr  = (jobjectArray)env->CallObjectMethod(dexElementsField, fieldGet, hostPathList);
    if (env->ExceptionCheck() || !guestArr || !hostArr) {
        env->ExceptionClear();
        if (guestArr) env->DeleteLocalRef(guestArr);
        if (hostArr) env->DeleteLocalRef(hostArr);
        env->DeleteLocalRef(guestPathList);
        env->DeleteLocalRef(hostPathList);
        env->DeleteLocalRef(fieldCls);
        env->DeleteLocalRef(pathListField);
        env->DeleteLocalRef(dexElementsField);
        return -6;
    }

    jsize guestLen = env->GetArrayLength(guestArr);
    jsize hostLen  = env->GetArrayLength(hostArr);
    LOGD("fnln: guestLen=%d hostLen=%d", (int)guestLen, (int)hostLen);

    // Compute host-only (dedupe by identity)
    std::vector<jobject> hostOnly;
    for (jsize i = 0; i < hostLen; i++) {
        jobject h = env->GetObjectArrayElement(hostArr, i);
        if (!h) continue;
        bool found = false;
        for (jsize j = 0; j < guestLen; j++) {
            jobject g = env->GetObjectArrayElement(guestArr, j);
            if (g && env->IsSameObject(h, g)) {
                found = true;
                env->DeleteLocalRef(g);
                break;
            }
            if (g) env->DeleteLocalRef(g);
        }
        if (!found) hostOnly.push_back(env->NewLocalRef(h));
        env->DeleteLocalRef(h);
    }

    int result = (int)hostOnly.size();
    LOGD("fnln: host-only elements = %d", result);

    auto cleanup = [&]() {
        for (auto h : hostOnly) env->DeleteLocalRef(h);
        env->DeleteLocalRef(guestArr);
        env->DeleteLocalRef(hostArr);
        env->DeleteLocalRef(guestPathList);
        env->DeleteLocalRef(hostPathList);
        env->DeleteLocalRef(fieldCls);
        env->DeleteLocalRef(pathListField);
        env->DeleteLocalRef(dexElementsField);
    };

    if (hostOnly.empty()) {
        LOGD("fnln: no host-only elements to add (already chained)");
        cleanup();
        return 0;
    }

    jclass elemCls = env->GetObjectClass(hostOnly[0]);
    if (!elemCls) {
        env->ExceptionClear();
        cleanup();
        return -7;
    }

    jsize newSize = (jsize)hostOnly.size() + guestLen;
    jobjectArray newArr = env->NewObjectArray(newSize, elemCls, nullptr);
    env->DeleteLocalRef(elemCls);
    if (!newArr) {
        env->ExceptionClear();
        cleanup();
        return -8;
    }

    for (size_t i = 0; i < hostOnly.size(); i++) {
        env->SetObjectArrayElement(newArr, (jsize)i, hostOnly[i]);
    }
    for (jsize i = 0; i < guestLen; i++) {
        jobject g = env->GetObjectArrayElement(guestArr, i);
        if (g) {
            env->SetObjectArrayElement(newArr, (jsize)hostOnly.size() + i, g);
            env->DeleteLocalRef(g);
        }
    }

    env->CallVoidMethod(dexElementsField, fieldSet, guestPathList, newArr);
    if (env->ExceptionCheck()) {
        LOGE("fnln: Field.set threw");
        env->ExceptionClear();
        env->DeleteLocalRef(newArr);
        cleanup();
        return -9;
    }

    LOGD("fnln: SUCCESS, added %d host-only elements", result);
    env->DeleteLocalRef(newArr);
    cleanup();
    return result;
}

extern "C" JNIEXPORT jint JNICALL
Java_com_ims134_hookengine_ClassLoaderBridge_nativeFnln(
        JNIEnv* env, jobject /*this*/, jobject guest, jobject host) {
    return (jint)fnln_impl(env, guest, host);
}