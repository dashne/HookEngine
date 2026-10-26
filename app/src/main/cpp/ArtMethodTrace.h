#pragma once
#include <cstdint>
#include <jni.h>
#include "HookRegistry.h"

// Feature 1.4 — ArtMethod entry/exit trace.
//
// Handler fires pre-call (onEntry) and post-call (onExit) around the
// method's real body. The original method still executes normally and
// returns its real value — the caller observes no difference beyond the
// side effects of the handlers.
//
// IMPORTANT — fast-JNI exclusion:
// ART's RegisterNatives marks methods with an all-primitive signature
// (int(), int(int), long(), void(), etc.) as "fast-JNI". Their ArtMethod
// data_ slot holds a small index token (< 0x1000) into ART's fast-JNI
// dispatch table, not a callable code pointer. JavaHookInstall refuses
// such methods. TraceAttach inherits that refusal.
//
// Therefore any method passed to TraceAttach MUST NOT be fast-JNI
// eligible. Easiest way to break eligibility: include a non-primitive
// (String, Object, array) in the signature. Framework natives like
// Binder.getCallingUid are compiled eagerly to a fast stub in libart.so
// and are NOT fast-JNI tokens — they are hookable.
//
// arity parameter selects which JNI wrapper to install:
//   0 = int foo()
//   1 = int foo(int)
//   2 = int foo(String)
//   3 = int foo(String, int)

typedef void (*TraceEntryFn)(void* ctx);
typedef void (*TraceExitFn)(void* ctx, int64_t ret);

extern "C" HookResult TraceAttach(JNIEnv* env, jobject methodObj,
                                  TraceEntryFn onEntry, TraceExitFn onExit,
                                  void* ctx, int arity);
extern "C" HookResult TraceDetach(JNIEnv* env, jobject methodObj);
extern "C" int        TraceCount();
extern "C" void       TraceResetAll();