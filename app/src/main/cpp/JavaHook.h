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

// Feature 3.4 session 51 — direct jmethodID entry.
//
// Same behavior as JavaHookInstall but skips the FromReflectedMethod round-
// trip. On MIUI/API 37, FromReflectedMethod returns 0x3d (a small token) for
// ServiceManager.getService even though reflection reports native=false. This
// entry path accepts a jmethodID returned by GetStaticMethodID/GetMethodID
// directly, so callers who already have the jmethodID can bypass the anomaly.
//
// If GetStaticMethodID also returns a token, the fast-JNI guard here will
// refuse with the same log line — that would mean the method truly is fast-
// JNI on this ART and ArtMethod hook is architecturally blocked.
extern "C" JavaHook* JavaHookInstallByMethodID(JNIEnv* env, jmethodID method_id,
                                               void* replacement, void** out_original,
                                               const char* name);

extern "C" HookResult JavaHookUninstall(JavaHook* hook);
extern "C" int JavaHookUninstallAll();
extern "C" int JavaHookCount();