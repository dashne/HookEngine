#include "test_suite.h"
#include <cstring>
#include "TestReport.h"

extern "C" int RunItem1ForHarness();
extern "C" int RunItem2ForHarness();
extern "C" int RunItem3ForHarness();
extern "C" int RunItem4ForHarness(JNIEnv* env);
extern "C" int RunItem5ForHarness(JNIEnv* env);

extern "C" int RunTestSuite(JNIEnv* env, const char* suite) {
    testreport::Reset();
    if (!suite) return -1;
    if (!strcmp(suite, "item1")) return RunItem1ForHarness();
    if (!strcmp(suite, "item2")) return RunItem2ForHarness();
    if (!strcmp(suite, "item3")) return RunItem3ForHarness();
    if (!strcmp(suite, "item4")) return RunItem4ForHarness(env);
    if (!strcmp(suite, "item5")) return RunItem5ForHarness(env);
    return -1;
}