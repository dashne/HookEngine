//
// Created by ibrahim on 10/23/2026.
//

#ifndef HOOKENGINE_TESTREPORT_H
#define HOOKENGINE_TESTREPORT_H

#endif //HOOKENGINE_TESTREPORT_H

#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <android/log.h>

// Single reporting path for every [PASS]/[FAIL] site. Two effects:
//   1. Prints the same line to logcat — format unchanged from v28.
//   2. Stores it in an in-memory list for the JNI bridge (later step).
//
// Uses ANDROID_LOG_DEBUG (not _INFO) to match the existing LOGD macro
// in test_itemN.cpp, so logcat output stays byte-identical.
namespace testreport {

    struct Line { bool pass; std::string name; std::string detail; };

    inline std::mutex& Mu()          { static std::mutex m; return m; }
    inline std::vector<Line>& Sink() { static std::vector<Line> v; return v; }

    inline void Reset() {
        std::lock_guard<std::mutex> l(Mu());
        Sink().clear();
    }

    inline void Report(const char* name, bool pass, const std::string& detail) {
        __android_log_print(ANDROID_LOG_DEBUG, "DualCore",
                            "[%s] %s: %s",
                            pass ? "PASS" : "FAIL", name, detail.c_str());
        std::lock_guard<std::mutex> l(Mu());
        Sink().push_back({pass, name, detail});
    }

// Non-destructive snapshot. RunTestSuite calls Reset() at the start of
// each suite, so a clear-on-drain would be redundant and the name
// "Drain" would be misleading. Named Snapshot to reflect behaviour.
    inline std::vector<Line> Snapshot() {
        std::lock_guard<std::mutex> l(Mu());
        return Sink();
    }

} // namespace testreport
