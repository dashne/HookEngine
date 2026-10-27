#pragma once
#include <jni.h>

extern "C" int  FastJniProbe();
extern "C" int  FastJniInstall();
extern "C" void FastJniUninstall();
extern "C" int  FastJniHits();
extern "C" int  FastJniFlipAccNative(void* art_method);
extern "C" int  FastJniDirectInstall(JNIEnv* env, void* art_method, void* trampoline);
extern "C" int  FastJniRestore();
extern "C" void* FastJniFindArtTrampoline();