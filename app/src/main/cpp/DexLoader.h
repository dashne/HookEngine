#pragma once
#include <jni.h>

// Feature 4.3 — Dynamic DEX loading.
//
// Creates a DexClassLoader from a runtime-provided dex/apk path. Runs
// entirely in C++ — the caller (Kotlin) provides the paths and parent,
// the engine does class lookup + method resolution + construction.
//
// Returns a local ref to the new DexClassLoader, or nullptr on failure.
// Safe to call multiple times — each call constructs a fresh loader.
extern "C" jobject DexLoaderCreate(JNIEnv* env, jstring dexPath,
                                   jstring optimizedDir, jobject parent);