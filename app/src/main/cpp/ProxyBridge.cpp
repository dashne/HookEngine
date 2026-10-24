// ProxyBridge.cpp
//
// Session 13: registered Java-surface stubs for handleJHMethod and
// nativeCNVM matching com.excelliance.kxqp.platform.ApplicationProxy.
//
// Session 14: nativeCNVM reimplemented from IDA decomp of
// libkxqpplatform.so sub_30BF7C. Entry-side lazy-init of boxed-primitive
// jmethodIDs ported from sub_30C238 (verified: it is a one-time static
// cache of 8 jmethodIDs + 1 jfieldID, not a per-call side effect).
//
// Reference behavior (sub_30BF7C):
//   - str[0] is a leading sentinel; str[i+1] is the JNI type tag for
//     objArr[i] ('B','C','D','F','I','J','L','S','Z').
//   - Each objArr[i] is coerced to a jvalue via Call<X>MethodV on the
//     cached jmethodID of the corresponding box class.
//   - method resolves via FromReflectedMethod → jmethodID.
//   - obj's concrete class via GetObjectClass.
//   - CallNonvirtualVoidMethodA(obj, cls, mid, args) — nonvirtual, void.
//   - Target exceptions stay pending; caller checks.
//
// Reference behavior (sub_30C238):
//   - Lazy-init on first nativeCNVM call (guard: cached booleanValue id).
//   - Caches 8 jmethodIDs: Boolean,Integer,Byte,Long,Short,Float,Double,
//     Character.
//   - If API >= 30, caches jfieldID of Executable.artMethod. Not used by
//     nativeCNVM; likely used by handleJHMethod. Cached here for future.
//
// handleJHMethod remains a stub — location in .so not yet found.

#include <jni.h>
#include <android/api-level.h>
#include <android/log.h>
#include <mutex>

#define TAG "DualCore"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

namespace {

// ─── Lazy cache of unwrap jmethodIDs (mirrors sub_30C238) ────────────
    struct UnwrapCache {
        jmethodID boolValue    = nullptr;
        jmethodID intValue     = nullptr;
        jmethodID byteValue    = nullptr;
        jmethodID longValue    = nullptr;
        jmethodID shortValue   = nullptr;
        jmethodID floatValue   = nullptr;
        jmethodID doubleValue  = nullptr;
        jmethodID charValue    = nullptr;
        jfieldID  artMethodFid = nullptr;  // Executable.artMethod (API >= 30)
        bool      initialized  = false;
    };

    UnwrapCache g_cache;
    std::once_flag g_cache_once;

    jmethodID resolveValueM(JNIEnv* env, const char* cls, const char* name,
                            const char* sig) {
        jclass c = env->FindClass(cls);
        if (!c) { env->ExceptionClear(); return nullptr; }
        jmethodID m = env->GetMethodID(c, name, sig);
        env->DeleteLocalRef(c);
        if (!m) env->ExceptionClear();
        return m;
    }

    void InitUnwrapCache(JNIEnv* env) {
        g_cache.boolValue   = resolveValueM(env, "java/lang/Boolean",   "booleanValue", "()Z");
        g_cache.intValue    = resolveValueM(env, "java/lang/Integer",   "intValue",     "()I");
        g_cache.byteValue   = resolveValueM(env, "java/lang/Byte",      "byteValue",    "()B");
        g_cache.longValue   = resolveValueM(env, "java/lang/Long",      "longValue",    "()J");
        g_cache.shortValue  = resolveValueM(env, "java/lang/Short",     "shortValue",   "()S");
        g_cache.floatValue  = resolveValueM(env, "java/lang/Float",     "floatValue",   "()F");
        g_cache.doubleValue = resolveValueM(env, "java/lang/Double",    "doubleValue",  "()D");
        g_cache.charValue   = resolveValueM(env, "java/lang/Character", "charValue",    "()C");

        if (android_get_device_api_level() >= 30) {
            jclass ec = env->FindClass("java/lang/reflect/Executable");
            if (ec) {
                jfieldID f = env->GetFieldID(ec, "artMethod", "J");
                env->DeleteLocalRef(ec);
                if (!f) env->ExceptionClear();
                else    g_cache.artMethodFid = f;
            } else {
                env->ExceptionClear();
            }
        }

        g_cache.initialized = true;
        LOGD("ProxyBridge: unwrap cache ready (artMethodFid=%p)",
             (void*)g_cache.artMethodFid);
    }

    inline UnwrapCache& Cache(JNIEnv* env) {
        std::call_once(g_cache_once, [env]{ InitUnwrapCache(env); });
        return g_cache;
    }

// ─── Per-tag unwrap via cached jmethodID ─────────────────────────────
// Matches reference sub_29E9F0/sub_29EA8C/... one-line Call<X>MethodV.

    jbyte unwrapByte(JNIEnv* env, jobject o, jmethodID m) {
        return o && m ? env->CallByteMethod(o, m) : 0;
    }
    jchar unwrapChar(JNIEnv* env, jobject o, jmethodID m) {
        return o && m ? env->CallCharMethod(o, m) : 0;
    }
    jdouble unwrapDouble(JNIEnv* env, jobject o, jmethodID m) {
        return o && m ? env->CallDoubleMethod(o, m) : 0;
    }
    jfloat unwrapFloat(JNIEnv* env, jobject o, jmethodID m) {
        return o && m ? env->CallFloatMethod(o, m) : 0;
    }
    jint unwrapInt(JNIEnv* env, jobject o, jmethodID m) {
        return o && m ? env->CallIntMethod(o, m) : 0;
    }
    jlong unwrapLong(JNIEnv* env, jobject o, jmethodID m) {
        return o && m ? env->CallLongMethod(o, m) : 0;
    }
    jshort unwrapShort(JNIEnv* env, jobject o, jmethodID m) {
        return o && m ? env->CallShortMethod(o, m) : 0;
    }
    jboolean unwrapBoolean(JNIEnv* env, jobject o, jmethodID m) {
        return o && m ? env->CallBooleanMethod(o, m) : 0;
    }

} // namespace

// ─── nativeCNVM — ported from sub_30BF7C ─────────────────────────────
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ProxyBridge_nativeCNVM(
        JNIEnv* env, jclass,
        jobject method, jobject obj, jstring str, jobjectArray arr) {
    if (!env) return;

    // Reference: sub_30C238(env) at entry. Verified: one-time static
    // cache of unwrap jmethodIDs + Executable.artMethod fieldID.
    UnwrapCache& c = Cache(env);

    if (!method || !obj || !str || !arr) return;

    const char* sig = env->GetStringUTFChars(str, nullptr);
    if (!sig) return;

    jsize n = env->GetArrayLength(arr);
    jvalue* args = (n > 0) ? new jvalue[(size_t)n]() : nullptr;

    for (jsize i = 0; i < n; i++) {
        char tag = sig[i + 1];  // reference skips sig[0]
        jobject elem = env->GetObjectArrayElement(arr, i);

        switch (tag) {
            case 'B': args[i].b = unwrapByte(env, elem, c.byteValue);       break;
            case 'C': args[i].c = unwrapChar(env, elem, c.charValue);       break;
            case 'D': args[i].d = unwrapDouble(env, elem, c.doubleValue);   break;
            case 'F': args[i].f = unwrapFloat(env, elem, c.floatValue);     break;
            case 'I': args[i].i = unwrapInt(env, elem, c.intValue);         break;
            case 'J': args[i].j = unwrapLong(env, elem, c.longValue);       break;
            case 'L': args[i].l = elem ? env->NewLocalRef(elem) : nullptr;  break;
            case 'S': args[i].s = unwrapShort(env, elem, c.shortValue);     break;
            case 'Z': args[i].z = unwrapBoolean(env, elem, c.boolValue);    break;
            default: break;  // reference has no default; slot stays zeroed
        }

        if (elem) env->DeleteLocalRef(elem);
    }

    env->ReleaseStringUTFChars(str, sig);

    jmethodID mid = env->FromReflectedMethod(method);
    jclass cls = env->GetObjectClass(obj);
    env->CallNonvirtualVoidMethodA(obj, cls, mid, args);

    if (cls) env->DeleteLocalRef(cls);
    if (args) delete[] args;
}

// ─── handleJHMethod — stub, decomp still pending ─────────────────────
extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ProxyBridge_handleJHMethod(
        JNIEnv* env, jclass,
        jobjectArray /*methodArr*/,
        jobject /*obj*/,
        jobject /*obj2*/,
        jobjectArray /*objArr*/) {
    LOGD("ProxyBridge.handleJHMethod: STUB — decomp pending, returning null");
    return nullptr;
}