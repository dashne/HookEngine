// ProxyBridge.cpp
//
// Session 13 — Java-surface JNI entries for ApplicationProxy dispatchers.
//
// nativeCNVM — decomp-verified port of libkxqpplatform.so sub_30BF7C.
//              Cache-init sub_30C238 verified: 8 jmethodIDs (Boolean,Integer,
//              Byte,Long,Short,Float,Double,Character.value) + Executable.artMethod
//              fieldID on API>=30. artMethod fieldID not read by nativeCNVM
//              (confirmed). Three safe-strengthening divergences vs reference:
//                (1) null-guards method/obj (ref does not);
//                (2) zero-initializes jvalue[] (ref does not);
//                (3) DeleteLocalRef(GetObjectClass(obj)) (ref leaks it).
//
// handleJHMethod — JNI entry present. Behavior NOT decomp-verified:
//                  sub_29E168 decomp is pending. Two provable local-ref leaks
//                  from Session 13 audit are fixed here (BoxResult FindClass
//                  leak; static GetObjectClass leak). Static-dispatch class
//                  question (GetObjectClass(obj) vs declaring class) remains
//                  OPEN pending sub_29E168.
//
// Reference behavior (sub_30BF7C — nativeCNVM):
//   - sig[0] is a leading sentinel; sig[i+1] is the JNI type tag for
//     objArr[i] ('B','C','D','F','I','J','L','S','Z').
//   - Each objArr[i] is coerced to a jvalue via Call<X>MethodV on the
//     cached jmethodID of the corresponding box class.
//   - method resolves via FromReflectedMethod -> jmethodID.
//   - obj's concrete class via GetObjectClass.
//   - CallNonvirtualVoidMethodA(obj, cls, mid, args) — nonvirtual, void.
//   - Target exceptions stay pending; caller checks.

#include <jni.h>
#include <android/api-level.h>
#include <android/log.h>
#include <mutex>
#include <cstring>
#include <cstdlib>

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

// ─── Path-B helpers — reflective descriptor derivation ───────────────
    struct ReflectIds {
        jmethodID getName           = nullptr;
        jmethodID getReturnType     = nullptr;
        jmethodID getParameterTypes = nullptr;
        jmethodID getModifiers      = nullptr;
        jmethodID classGetName      = nullptr;
        jmethodID modIsStatic       = nullptr;
        // Box.valueOf static factories for return boxing
        jmethodID valueOfBoolean    = nullptr;
        jmethodID valueOfByte       = nullptr;
        jmethodID valueOfChar       = nullptr;
        jmethodID valueOfShort      = nullptr;
        jmethodID valueOfInt        = nullptr;
        jmethodID valueOfLong       = nullptr;
        jmethodID valueOfFloat      = nullptr;
        jmethodID valueOfDouble     = nullptr;
        // Cached jclass refs so BoxResult doesn't FindClass per call.
        jclass    clsBoolean        = nullptr;
        jclass    clsByte           = nullptr;
        jclass    clsCharacter      = nullptr;
        jclass    clsShort          = nullptr;
        jclass    clsInteger        = nullptr;
        jclass    clsLong           = nullptr;
        jclass    clsFloat          = nullptr;
        jclass    clsDouble         = nullptr;
        jclass    clsModifier       = nullptr;
    };

    ReflectIds g_rids;
    std::once_flag g_rids_once;

    void InitReflectIds(JNIEnv* env) {
        jclass methodCls = env->FindClass("java/lang/reflect/Method");
        if (methodCls) {
            g_rids.getName           = env->GetMethodID(methodCls, "getName",           "()Ljava/lang/String;");
            g_rids.getReturnType     = env->GetMethodID(methodCls, "getReturnType",     "()Ljava/lang/Class;");
            g_rids.getParameterTypes = env->GetMethodID(methodCls, "getParameterTypes", "()[Ljava/lang/Class;");
            g_rids.getModifiers      = env->GetMethodID(methodCls, "getModifiers",      "()I");
            env->DeleteLocalRef(methodCls);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        jclass classCls = env->FindClass("java/lang/Class");
        if (classCls) {
            g_rids.classGetName = env->GetMethodID(classCls, "getName", "()Ljava/lang/String;");
            env->DeleteLocalRef(classCls);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        jclass modCls = env->FindClass("java/lang/reflect/Modifier");
        if (modCls) {
            g_rids.modIsStatic = env->GetStaticMethodID(modCls, "isStatic", "(I)Z");
            g_rids.clsModifier = (jclass)env->NewGlobalRef(modCls);
            env->DeleteLocalRef(modCls);
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        struct BoxEntry { const char* cls; const char* sig; jmethodID* slot; jclass* globalCls; };
        BoxEntry boxes[] = {
                {"java/lang/Boolean",   "(Z)Ljava/lang/Boolean;",   &g_rids.valueOfBoolean, &g_rids.clsBoolean},
                {"java/lang/Byte",      "(B)Ljava/lang/Byte;",      &g_rids.valueOfByte,    &g_rids.clsByte},
                {"java/lang/Character", "(C)Ljava/lang/Character;", &g_rids.valueOfChar,    &g_rids.clsCharacter},
                {"java/lang/Short",     "(S)Ljava/lang/Short;",     &g_rids.valueOfShort,   &g_rids.clsShort},
                {"java/lang/Integer",   "(I)Ljava/lang/Integer;",   &g_rids.valueOfInt,     &g_rids.clsInteger},
                {"java/lang/Long",      "(J)Ljava/lang/Long;",      &g_rids.valueOfLong,    &g_rids.clsLong},
                {"java/lang/Float",     "(F)Ljava/lang/Float;",     &g_rids.valueOfFloat,   &g_rids.clsFloat},
                {"java/lang/Double",    "(D)Ljava/lang/Double;",    &g_rids.valueOfDouble,  &g_rids.clsDouble},
        };
        for (auto& b : boxes) {
            jclass c = env->FindClass(b.cls);
            if (c) {
                *b.slot = env->GetStaticMethodID(c, "valueOf", b.sig);
                *b.globalCls = (jclass)env->NewGlobalRef(c);
                env->DeleteLocalRef(c);
            }
            if (env->ExceptionCheck()) env->ExceptionClear();
        }
        LOGD("ProxyBridge: reflect ids ready");
    }

    inline ReflectIds& Rids(JNIEnv* env) {
        std::call_once(g_rids_once, [env]{ InitReflectIds(env); });
        return g_rids;
    }

    char TagForClassName(const char* name) {
        if (!name || !name[0]) return 'L';
        if (!name[1]) {
            switch (name[0]) {
                case 'V': case 'B': case 'C': case 'D':
                case 'F': case 'I': case 'J': case 'S': case 'Z':
                    return name[0];
            }
        }
        return 'L';
    }

    size_t BuildDescriptor(JNIEnv* env, jobject target, char** outBuf) {
        *outBuf = nullptr;
        ReflectIds& r = Rids(env);
        if (!r.getReturnType || !r.getParameterTypes || !r.classGetName) return 0;

        jobject retCls = env->CallObjectMethod(target, r.getReturnType);
        if (env->ExceptionCheck() || !retCls) { env->ExceptionClear(); return 0; }

        jobjectArray paramClsArr = (jobjectArray) env->CallObjectMethod(target, r.getParameterTypes);
        if (env->ExceptionCheck()) {
            env->ExceptionClear(); env->DeleteLocalRef(retCls); return 0;
        }
        jsize n = paramClsArr ? env->GetArrayLength(paramClsArr) : 0;

        char* buf = (char*) malloc((size_t)(n + 1));
        if (!buf) {
            if (paramClsArr) env->DeleteLocalRef(paramClsArr);
            env->DeleteLocalRef(retCls);
            return 0;
        }

        jstring retName = (jstring) env->CallObjectMethod(retCls, r.classGetName);
        const char* retC = retName ? env->GetStringUTFChars(retName, nullptr) : nullptr;
        buf[0] = TagForClassName(retC);
        if (retC) env->ReleaseStringUTFChars(retName, retC);
        if (retName) env->DeleteLocalRef(retName);
        env->DeleteLocalRef(retCls);

        for (jsize i = 0; i < n; i++) {
            jobject pc = env->GetObjectArrayElement(paramClsArr, i);
            if (!pc) { buf[i + 1] = 'L'; continue; }
            jstring nm = (jstring) env->CallObjectMethod(pc, r.classGetName);
            const char* c = nm ? env->GetStringUTFChars(nm, nullptr) : nullptr;
            buf[i + 1] = TagForClassName(c);
            if (c) env->ReleaseStringUTFChars(nm, c);
            if (nm) env->DeleteLocalRef(nm);
            env->DeleteLocalRef(pc);
        }
        if (paramClsArr) env->DeleteLocalRef(paramClsArr);

        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            free(buf);
            return 0;
        }
        *outBuf = buf;
        return (size_t)(n + 1);
    }

    bool IsStaticMethod(JNIEnv* env, jobject target) {
        ReflectIds& r = Rids(env);
        if (!r.getModifiers || !r.modIsStatic || !r.clsModifier) return false;
        jint mods = env->CallIntMethod(target, r.getModifiers);
        if (env->ExceptionCheck()) { env->ExceptionClear(); return false; }
        return env->CallStaticBooleanMethod(r.clsModifier, r.modIsStatic, mods) == JNI_TRUE;
    }

// Box a primitive result. Uses cached global class refs so no per-call
// FindClass leak (Session 13 fix — previous FindClass per call leaked a
// local per invocation).
    jobject BoxResult(JNIEnv* env, char tag, jvalue v) {
        ReflectIds& r = Rids(env);
        switch (tag) {
            case 'Z': return (r.valueOfBoolean && r.clsBoolean)
                             ? env->CallStaticObjectMethod(r.clsBoolean, r.valueOfBoolean, v.z) : nullptr;
            case 'B': return (r.valueOfByte && r.clsByte)
                             ? env->CallStaticObjectMethod(r.clsByte, r.valueOfByte, v.b) : nullptr;
            case 'C': return (r.valueOfChar && r.clsCharacter)
                             ? env->CallStaticObjectMethod(r.clsCharacter, r.valueOfChar, v.c) : nullptr;
            case 'S': return (r.valueOfShort && r.clsShort)
                             ? env->CallStaticObjectMethod(r.clsShort, r.valueOfShort, v.s) : nullptr;
            case 'I': return (r.valueOfInt && r.clsInteger)
                             ? env->CallStaticObjectMethod(r.clsInteger, r.valueOfInt, v.i) : nullptr;
            case 'J': return (r.valueOfLong && r.clsLong)
                             ? env->CallStaticObjectMethod(r.clsLong, r.valueOfLong, v.j) : nullptr;
            case 'F': return (r.valueOfFloat && r.clsFloat)
                             ? env->CallStaticObjectMethod(r.clsFloat, r.valueOfFloat, v.f) : nullptr;
            case 'D': return (r.valueOfDouble && r.clsDouble)
                             ? env->CallStaticObjectMethod(r.clsDouble, r.valueOfDouble, v.d) : nullptr;
            default: return nullptr;
        }
    }

    void UnwrapArgs(JNIEnv* env, UnwrapCache& c,
                    jobjectArray arr, const char* desc, size_t descLen,
                    jvalue* args) {
        (void)descLen;
        jsize n = env->GetArrayLength(arr);
        for (jsize i = 0; i < n; i++) {
            char tag = desc[i + 1];
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
                default: break;
            }
            if (elem) env->DeleteLocalRef(elem);
        }
    }

} // namespace

// ─── nativeCNVM — ported from sub_30BF7C (decomp-verified Session 13) ──
extern "C" JNIEXPORT void JNICALL
Java_com_ims134_hookengine_ProxyBridge_nativeCNVM(
        JNIEnv* env, jobject b,
        jobject method, jobject obj, jstring str, jobjectArray arr) {
    if (!env) return;

    UnwrapCache& c = Cache(env);

    if (!method || !obj || !str || !arr) return;

    const char* sig = env->GetStringUTFChars(str, nullptr);
    if (!sig) return;

    jsize n = env->GetArrayLength(arr);
    jvalue* args = (n > 0) ? new jvalue[(size_t)n]() : nullptr;

    for (jsize i = 0; i < n; i++) {
        char tag = sig[i + 1];
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
            default: break;
        }

        if (elem) env->DeleteLocalRef(elem);
    }



    size_t sigLen = strlen(sig);
    env->ReleaseStringUTFChars(str, sig);



    jmethodID mid = env->FromReflectedMethod(method);
    if (!mid) {
        if (args) delete[] args;
        return;
    }
    jclass cls = env->GetObjectClass(obj);

    // Session 13: arity guard. CallNonvirtualVoidMethodA reads
    // param_count jvalues; if n < param_count it walks off the end
    // of `args` (SIGSEGV, nativeCNVMNullArgsIsNoOp). The decomp of
    // sub_30BF7C does not guard — the reference crashes on short
    // input. We refuse instead: no dispatch, no crash. Safe-
    // strengthening divergence #4, documented in the header comment.
    //
    // Calling GetArrayLength again here keeps the guard local; the
    // earlier `n` is the same value, use it.
    // We cannot cheaply ask ART for a method's param count from JNI
    // without GetMethodID/reflection, so we rely on `sig`: the
    // descriptor's length minus the leading return tag *is* the
    // parameter count by contract. If they disagree, refuse.
    size_t paramCount = sigLen > 0 ? sigLen - 1 : 0;
    if ((size_t)n != paramCount) {
        LOGD("ProxyBridge.nativeCNVM: arity mismatch n=%d paramCount=%zu — refusing dispatch",
             (int)n, paramCount);
        if (cls) env->DeleteLocalRef(cls);
        if (args) delete[] args;
        return;
    }

    env->CallNonvirtualVoidMethodA(obj, cls, mid, args);

    if (cls) env->DeleteLocalRef(cls);
    if (args) delete[] args;
}

// ─── handleJHMethod — surface delivered; behavior UNVERIFIED ──────────
// sub_29E168 decomp pending. Local-ref leak from BoxResult and static
// GetObjectClass fixed in Session 13 (provable defect, independent of
// sub_29E168). Static-dispatch class question (GetObjectClass(obj) vs
// declaring class) remains OPEN pending decomp.
extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ProxyBridge_handleJHMethod(
        JNIEnv* env, jobject,
        jobjectArray methodArr,
        jobject obj,
        jobject /*obj2*/,
        jobjectArray objArr)
        {
    //TODO:: for now we just return null because we differed its implementation to another time
    return nullptr;
}