#ifndef RP_RUN_REPORT_HPP
#define RP_RUN_REPORT_HPP

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

// RP_REPORT=path.json: one JSON object per run for test/corpus.py (PLAN_test_automation.md).
// main() sets fields where it prints the matching log line; the object is written when the
// process exits normally (any return from main), so a missing report means a crash or a kill.
// Keys keep their first-set order; setting a key again overwrites its value.
extern char** environ;   // POSIX

namespace RunReport {

struct Report {
    std::vector<std::pair<std::string, std::string>> fields;   // key -> JSON-encoded value
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

    void put(const std::string& key, const std::string& json) {
        for (auto& kv : fields) if (kv.first == key) { kv.second = json; return; }
        fields.emplace_back(key, json);
    }
    ~Report() {
        const char* path = std::getenv("RP_REPORT");
        if (!path || !*path) return;
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        FILE* f = std::fopen(path, "w");
        if (!f) return;
        std::fputs("{", f);
        for (const auto& kv : fields) std::fprintf(f, "\"%s\": %s, ", kv.first.c_str(), kv.second.c_str());
        std::fprintf(f, "\"wall_s\": %.3f}\n", wall);
        std::fclose(f);
    }
};

inline Report& report() { static Report r; return r; }

inline std::string quote(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += static_cast<char>(c);
    }
    return o + "\"";
}
inline void set(const std::string& k, const std::string& v) { report().put(k, quote(v)); }
inline void set(const std::string& k, const char* v)        { report().put(k, quote(v)); }
inline void set(const std::string& k, long long v)          { report().put(k, std::to_string(v)); }
inline void set(const std::string& k, int v)                { report().put(k, std::to_string(v)); }
inline void set(const std::string& k, size_t v)             { report().put(k, std::to_string(v)); }
inline void set(const std::string& k, bool v)               { report().put(k, v ? "true" : "false"); }
inline void set(const std::string& k, double v) {
    if (!(v == v) || v > 1e300 || v < -1e300) { report().put(k, "null"); return; }   // NaN / inf
    char b[32]; std::snprintf(b, sizeof b, "%.6g", v); report().put(k, b);
}
inline void setNull(const std::string& k)                   { report().put(k, "null"); }
inline void setRaw(const std::string& k, const std::string& json) { report().put(k, json); }
inline void set(const std::string& k, const std::vector<int>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) { if (i) o += ","; o += std::to_string(v[i]); }
    report().put(k, o + "]");
}

// The run's RP_* environment variables (RP_REPORT itself left out), as the "env" object.
inline void recordEnvironment() {
    std::string env = "{";
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        if (kv.compare(0, 3, "RP_") != 0 || kv.compare(0, 10, "RP_REPORT=") == 0) continue;
        const size_t eq = kv.find('=');
        if (env.size() > 1) env += ", ";
        env += quote(kv.substr(0, eq)) + ": " + quote(eq == std::string::npos ? "" : kv.substr(eq + 1));
    }
    setRaw("env", env + "}");
}

} // namespace RunReport

#endif
