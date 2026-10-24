# handleJHMethod — decomp-integration spec

**Status:** BLOCKED on IDA decomp of `sub_29E168` (libkxqpplatform.so).
**Purpose:** make `handleJHMethod` a 30-minute edit the moment the decomp lands,
instead of a multi-hour discovery session.
**Enforced by:** Rule 7 (IDA-first when behavior is uncertain).
**Do not modify `Java_com_ims134_hookengine_ProxyBridge_handleJHMethod`'s C++
body — including any of the scaffolding helpers it would use — until the
required decomp evidence below is in hand.**

---

## 1. What we have

JNI surface, from `ProxyBridge.cpp`:

```cpp
extern "C" JNIEXPORT jobject JNICALL
Java_com_ims134_hookengine_ProxyBridge_handleJHMethod(
    JNIEnv* env, jobject,
    jobjectArray methodArr,   // reflect.Method[]
    jobject      obj,         // receiver
    jobject      obj2,        // ??? — purpose unknown
    jobjectArray objArr);     // boxed arguments, one per parameter