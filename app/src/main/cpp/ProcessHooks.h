#pragma once
#include <jni.h>

extern "C" int  InstallProcessHooks(JNIEnv* env);
extern "C" int  UninstallProcessHooks();
extern "C" int  ProcessHookCount();
extern "C" void SetVirtualPid(int pid);
extern "C" int  GetVirtualPid();