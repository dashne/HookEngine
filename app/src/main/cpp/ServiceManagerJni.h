#pragma once
#include <jni.h>
#include "HookRegistry.h"

// Feature 3.4 — ServiceManager lookup interception via ArtMethod hook.
//
// Installs a JavaHook on android.os.ServiceManager.rawGetService and
// rawCheckService (static native IBinder methods). Consults the same
// override tables that ParcelRewriter_SetServiceManagerNull / Alias /
// Clear maintain (defined in CppParcelRewriter.cpp).
//
// SvcMgrJniAttach returns 0 on success, negative on failure:
//   -1  null JNIEnv
//   -2  ServiceManager class not found
//   -3  no hookable method (rawGetService / rawCheckService both absent
//       or fast-JNI token / JavaHookInstall refused)
//
// Both methods are hookable because their JNI signature is
// (String)IBinder — non-primitive both ways, so not fast-JNI-eligible.
//
// The wrapper reads the String arg via JNI, so it never parses parcels.
// Override lookup order:
//   1. Is name null-overridden? → return nullptr
//   2. Is name aliased?        → recurse via _orig(aliasedName)
//   3. Otherwise               → _orig(name)

extern "C" int  SvcMgrJniAttach(JNIEnv* env);
extern "C" void SvcMgrJniDetach();
extern "C" int  SvcMgrJniCount();