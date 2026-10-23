#include "test_suite.h"
#include <cstring>
#include "TestReport.h"

extern "C" int RunItem1ForHarness();
extern "C" int RunItem2ForHarness();
extern "C" int RunItem3ForHarness();
extern "C" int RunItem4ForHarness(JNIEnv* env);
extern "C" int RunItem5ForHarness(JNIEnv* env);
extern "C" int RunRegressionBug1(JNIEnv*);
extern "C" int RunRegressionBug2();
extern "C" int RunRegressionBug4();
extern "C" int RunRegressionBug5();

extern "C" int RunTestSuite(JNIEnv* env, const char* suite) {
    testreport::Reset();
    if (!suite) return -1;
    if (!strcmp(suite, "item1")) return RunItem1ForHarness();
    if (!strcmp(suite, "item2")) return RunItem2ForHarness();
    if (!strcmp(suite, "item3")) return RunItem3ForHarness();
    if (!strcmp(suite, "item4")) return RunItem4ForHarness(env);
    if (!strcmp(suite, "item5")) return RunItem5ForHarness(env);
    if (!strcmp(suite, "regression_bug1")) return RunRegressionBug1(env);
    if (!strcmp(suite, "regression_bug2")) return RunRegressionBug2();
    if (!strcmp(suite, "regression_bug4")) return RunRegressionBug4();
    if (!strcmp(suite, "regression_bug5")) return RunRegressionBug5();
    return -1;
}