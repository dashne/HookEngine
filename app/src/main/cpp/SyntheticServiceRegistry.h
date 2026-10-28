#pragma once
#include <jni.h>
#include <cstdint>

// Feature 3.7 — Native ServiceManager addition.
//
// Working mechanism on MIUI/API 37: ArtMethod hook on the Java method
// android.os.Parcel.readStrongBinder(). This is the wrapper around the
// JNI nativeReadStrongBinder(long), called by Service.readFromParcel
// inside IServiceManager.checkService2's reply parsing. When our hook
// fires for a registered synthetic name, it returns the stored Kotlin
// Binder object directly, bypassing the driver's BinderProxy.
//
// History of mechanisms tried and why each failed on this ROM:
//   A: mprotect reply Parcel's data page → EACCES (driver mmap PROT_READ)
//   B: malloc+copy+swap at offset 4 → corrupts hasResult field
//   C: scan reply bytes for flat_binder_object → false positives
//   D: use reply's mObjects[0] table, write there → "not in object list"
//   E: write at mObjects[0], reset mDataPos → "protected data in Parcel"
//      (validateReadData: object marker at 12 spans the caller's int read)
//   F: Java hook on ServiceManagerProxy.getService2 → fast-JNI (mid=0x3d)
//   G: C++ hook on Parcel::readStrongBinder → wrong ABI (retval in x8)
//   H (this): Java hook on Parcel.readStrongBinder() (the Java wrapper)
//
// The transact-level alias rewrite (Feature 3.4) is retained — it forces
// the driver to return a valid non-null reply (activity service's handle),
// which the caller then tries to read via readStrongBinder. Our hook
// fires at that point and returns the correct binder.
//
// Same-process callers receive the original Kotlin Binder. Cross-process
// callers cannot (app-uid cannot add services to SM). Feature is honest
// about this boundary.
//
// Max 16 concurrent synthetic services. Name max 128 bytes.

extern "C" int  SyntheticServiceRegistry_Add(JNIEnv* env, const char* name,
                                             const uint8_t* flatBinderBytes,
                                             size_t bytesLen, jobject binder);
extern "C" int  SyntheticServiceRegistry_Remove(JNIEnv* env, const char* name);
extern "C" int  SyntheticServiceRegistry_Count();
extern "C" void SyntheticServiceRegistry_ClearAll(JNIEnv* env);

extern "C" void SyntheticServiceRegistry_PreIntercept(jlong dataPtr, int code);
extern "C" void SyntheticServiceRegistry_PostIntercept(jlong replyPtr);

// Install the Parcel.readStrongBinder() ArtMethod hook. Called once from
// syntheticAdd. Idempotent. Returns 0 on success, negative on failure.
extern "C" int  SyntheticServiceRegistry_InstallReadStrongBinderHook(JNIEnv* env);