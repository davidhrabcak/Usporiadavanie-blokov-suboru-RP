#include "config.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace {

const char* env(const char* name) { return std::getenv(name); }

// Set: on unless it starts with '0'.
bool flag(const char* name, const bool def) {
    const char* v = env(name);
    return v ? v[0] != '0' : def;
}

Switch gate(const char* name) {
    const char* v = env(name);
    if (v && v[0] == '0') return Switch::Off;
    if (v && v[0] == '1') return Switch::On;
    return Switch::Auto;
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
    if (const char* v = env("RP_CHUNK_SIZE")) c.chunkSize = std::max(1024L, std::strtol(v, nullptr, 10));
    if (const char* v = env("RP_SEED")) {
        c.randomSeed = std::string(v) == "random";
        if (c.randomSeed)
            c.seed = static_cast<unsigned>(std::chrono::system_clock::now().time_since_epoch().count());
        else
            c.seed = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
    }
    if (const char* v = env("RP_START")) c.startPinned = std::string(v) == "pinned";

    c.cbrFilter  = flag("RP_CBR", true);
    c.gap        = flag("RP_GAP", true);
    c.resvFilter = gate("RP_RESV_FILTER");
    c.ac3        = gate("RP_AC3");

    c.rank = flag("RP_RANK", true);
    c.spec = flag("RP_SPEC", true);
    if (const char* v = env("RP_SPEC_SCALE")) c.specScale = std::atof(v);
    c.pcm = flag("RP_PCM", true);
    if (const char* v = env("RP_PCM_W")) c.pcmWeight = std::atof(v);
    if (const char* v = env("RP_PCM_LEFT")) c.pcmLeft = std::atoi(v);

    c.lock.mode = gate("RP_LOCK");
    if (const char* v = env("RP_LOCK_TAU")) c.lock.taus = parseTaus(v);
    if (const char* v = env("RP_LOCK_ROUNDS")) c.lock.rounds = std::max(1, std::atoi(v));
    if (const char* v = env("RP_LOCK_MAX_DECODES")) c.lock.maxDecodes = std::atol(v);
    c.threads = std::max(1u, std::thread::hardware_concurrency());
    if (const char* v = env("RP_THREADS")) c.threads = std::max(1, std::atoi(v));

    if (const char* v = env("RP_BEAM")) c.search.beam = std::max(0, std::atoi(v));
    if (env("RP_BROADEN")) c.search.broaden = flag("RP_BROADEN", false);
    if (const char* v = env("RP_TIME_BUDGET")) c.search.timeBudget = std::atof(v);
    if (const char* v = env("RP_MAX_NODES")) c.search.maxNodes = std::strtoull(v, nullptr, 10);
    if (const char* v = env("RP_PROGRESS")) {
        const uint64_t n = std::strtoull(v, nullptr, 10);
        if (n) c.search.progressEvery = n;
    }
    c.search.bestOf = flag("RP_BESTOF", false);

    if (const char* v = env("RP_MATCH")) c.match.mode = std::atoi(v);
    if (const char* v = env("RP_MATCH_MAX_DECODES")) c.match.maxDecodes = std::atol(v);
    if (const char* v = env("RP_MATCH_ITERS")) c.match.iters = std::max(0, std::atoi(v));

    c.repair.mode = gate("RP_REPAIR");
    if (const char* v = env("RP_REPAIR_PASSES")) c.repair.passes = std::max(1, std::atoi(v));
    if (const char* v = env("RP_REPAIR_SUSPECTS")) c.repair.suspects = std::atoi(v);
    if (const char* v = env("RP_REPAIR_BUDGET")) c.repair.budget = std::atof(v);
    if (const char* v = env("RP_REPAIR_MIN_GAIN")) c.repair.minGain = std::atof(v);

    c.slotFill = flag("RP_SLOT_FILL", true);
    if (const char* v = env("RP_SLOT_ORACLE")) c.slotOracle = v[0] == '1';
    c.scoreOrder = env("RP_SCORE_ORDER") != nullptr;
    return c;
}
