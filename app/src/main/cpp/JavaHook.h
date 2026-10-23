#pragma once
#include <jni.h>
#include "HookRegistry.h"

// Generic ArtMethod hook. Install once per method; uninstall individually or all at once.
//
// What it does: finds the ArtMethod entry pointer for `method` (offset 24 for
// native methods on API 34+), saves the original, swaps in `replacement`.
// `original` gets the pointer the caller can invoke through to reach the real
// implementation. `replacement` must have the same JNI signature as the original.
//
// Fails silently (returns nullptr) if the entry point can't be located. Safe to
// call twice — second call sees already-hooked and returns nullptr.

struct JavaHook;

extern "C" JavaHook* JavaHookInstall(JNIEnv* env, jobject method_obj,
                                     void* replacement, void** out_original,
                                     const char* name);
extern "C" HookResult JavaHookUninstall(JavaHook* hook);
extern "C" int JavaHookUninstallAll();
extern "C" int JavaHookCount();