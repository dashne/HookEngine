#pragma once
#include <jni.h>

extern "C" int  InstallAntiDetection(JNIEnv* env);
extern "C" int  UninstallAntiDetection();

// Test hooks — expose decision logic without needing a live socket
extern "C" int  ShouldBlockConnectForTest(const char* ip, int port);
extern "C" int  IsConnectHookInstalled();