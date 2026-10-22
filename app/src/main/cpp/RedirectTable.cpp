#include "RedirectTable.h"
#include <cstring>

RedirectTable& RedirectTable::GetInstance() {
    static RedirectTable _Instance;
    return _Instance;
}

void RedirectTable::AddRule(const std::string& originalPrefix, const std::string& virtualPrefix) {
    std::lock_guard<std::mutex> guard(_Lock);
    std::string o = originalPrefix;
    std::string v = virtualPrefix;
    // Strip trailing slash so "/a/b/" and "/a/b" match consistently,
    // and "/a/b/child" still resolves correctly.
    if (!o.empty() && o.back() == '/') o.pop_back();
    if (!v.empty() && v.back() == '/') v.pop_back();
    _Rules[o] = v;
}

std::string RedirectTable::ResolvePath(const char* originalPath) {
    if (!originalPath) return "";
    std::string path(originalPath);
    std::lock_guard<std::mutex> guard(_Lock);
    for (const auto& pair : _Rules) {
        const std::string& prefix = pair.first;
        if (path.size() < prefix.size()) continue;
        if (path.compare(0, prefix.size(), prefix) != 0) continue;
        // Boundary: exact match, or next char is '/'. Prevents "/a/bad"
        // from matching a rule for "/a/b".
        if (path.size() == prefix.size() || path[prefix.size()] == '/') {
            return pair.second + path.substr(prefix.size());
        }
    }
    return "";
}