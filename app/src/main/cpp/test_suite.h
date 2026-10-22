//
// Created by ibrahim on 10/23/2026.
//

#ifndef HOOKENGINE_TEST_SUITE_H
#define HOOKENGINE_TEST_SUITE_H

#endif //HOOKENGINE_TEST_SUITE_H

#pragma once
#include <jni.h>

// Harness dispatch. Called by the JNI bridge (added in §7.3) — not by
// anything in §7.2. Each suite resets the report sink, runs, and returns
// the pass count. Failures are visible via testreport::Snapshot().
//
// env is required only by item4 (read_via_java) and item5 (FromReflectedMethod).
// Items 1–3 ignore it.
extern "C" int RunTestSuite(JNIEnv* env, const char* suite);
