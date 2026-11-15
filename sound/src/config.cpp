#include "config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace {

const char* env(const char* name) { return std::getenv(name); }

// On unless set to something starting with '0'.
bool flag(const char* name) {
    const char* v = env(name);
    return !v || v[0] != '0';
}

// "1.5,1.25,1.1" -> {1.5, 1.25, 1.1}; non-positive entries are dropped, an empty list is {1.5}.
std::vector<double> parseTaus(const std::string& spec) {
    std::vector<double> taus;
    size_t at = 0;
    while (at < spec.size()) {
        size_t comma = spec.find(',', at);
        if (comma == std::string::npos) comma = spec.size();
        const double v = std::atof(spec.substr(at, comma - at).c_str());
        if (v > 0) taus.push_back(v);
        at = comma + 1;
    }
    if (taus.empty()) taus = {1.5};
    return taus;
}

} // namespace

Config Config::fromEnv(int argc, char* argv[]) {
    Config c;
    if (argc > 1) c.input = argv[1];
    if (const char* v = env("RP_CHUNK_SIZE")) c.chunkSize = std::max(kMinChunkSize, std::strtol(v, nullptr, 10));
    if (const char* v = env("RP_SEED")) {
        c.randomSeed = std::string(v) == "random";
        if (c.randomSeed)
            c.seed = static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count());
        else
            c.seed = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    }
    if (const char* v = env("RP_START")) c.startPinned = std::string(v) == "pinned";
    c.threads = std::max(1u, std::thread::hardware_concurrency());
    if (const char* v = env("RP_THREADS")) c.threads = std::max(1, std::atoi(v));

    c.cbrFilter  = flag("RP_CBR");
    c.resvFilter = flag("RP_RESV_FILTER");
    c.ac3        = flag("RP_AC3");

    c.rank = flag("RP_RANK");
    c.spec = flag("RP_SPEC");
    c.pcm  = flag("RP_PCM");
    if (const char* v = env("RP_PCM_LEFT")) c.pcmLeft = std::atoi(v);

    c.lock.on = flag("RP_LOCK");
    if (const char* v = env("RP_LOCK_TAU")) c.lock.taus = parseTaus(v);
    if (const char* v = env("RP_LOCK_MAX_DECODES")) c.lock.maxDecodes = std::atol(v);

    if (const char* v = env("RP_CHAIN")) c.chain = v[0] == '1';

    if (const char* v = env("RP_TIME_BUDGET")) c.timeBudget = std::atof(v);

    c.match.on = flag("RP_MATCH");
    if (const char* v = env("RP_MATCH_MAX_DECODES")) c.match.maxDecodes = std::atol(v);

    c.repair.on = flag("RP_REPAIR");
    if (const char* v = env("RP_REPAIR_BUDGET")) c.repair.budget = std::atof(v);
    if (const char* v = env("RP_REPAIR_MIN_GAIN")) c.repair.minGain = std::atof(v);

    c.scoreOrder = env("RP_SCORE_ORDER") != nullptr;
    return c;
}
