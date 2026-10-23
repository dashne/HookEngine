#pragma once
#include <jni.h>

extern "C" int  InstallProcessHooks(JNIEnv* env);
extern "C" int  UninstallProcessHooks();
extern "C" int  ProcessHookCount();
int  SpoofProcessStatics(JNIEnv* env, jint fake_uid, jint fake_pid);
void RestoreProcessStatics();