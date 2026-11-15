#ifndef RP_CONFIG_HPP
#define RP_CONFIG_HPP

#include <optional>
#include <string>
#include <vector>

// Every RP_* option rp reads, parsed once at startup (Config::fromEnv). The options table in
// CLAUDE.md describes each one. RP_REPORT is the exception: run_report.hpp reads it when the
// report is written. On/off options are on unless the variable starts with '0'.
struct Config {
    static constexpr long kDefaultChunkSize = 4096;
    static constexpr long kMinChunkSize = 1024;

    std::string input = "test/ci_sanity.mp3";       // argv[1]
    long chunkSize = kDefaultChunkSize;             // RP_CHUNK_SIZE
    unsigned seed = 1;                              // RP_SEED
    bool randomSeed = false;                        // RP_SEED=random: seed from the clock
    bool startPinned = false;                       // RP_START=pinned
    int threads = 1;                                // RP_THREADS (default: all cores)

    // Graph construction
    bool cbrFilter = true;                          // RP_CBR
    bool resvFilter = true;                         // RP_RESV_FILTER
    bool ac3 = true;                                // RP_AC3

    // Candidate ranking (junction cost = global_gain + spectral + PCM flux)
    bool rank = true;                               // RP_RANK
    bool spec = true;                               // RP_SPEC
    bool pcm = true;                                // RP_PCM
    std::optional<int> pcmLeft;                     // RP_PCM_LEFT (unset: from the frame length)

    struct Lock {
        bool on = true;                             // RP_LOCK
        std::vector<double> taus{1.5, 1.25, 1.1};   // RP_LOCK_TAU (comma list)
        long maxDecodes = 640000;                   // RP_LOCK_MAX_DECODES (0 = no cap)
    } lock;

    std::string cont;                               // RP_CONT=fixed|cal: continuity cost (proof of concept)
    bool chain = false;                             // RP_CHAIN=1: segment growing (proof of concept)

    std::optional<double> timeBudget;               // RP_TIME_BUDGET: DFS seconds per lock attempt

    struct Match {
        bool on = true;                             // RP_MATCH
        long maxDecodes = 100000;                   // RP_MATCH_MAX_DECODES
    } match;

    struct Repair {
        bool on = true;                             // RP_REPAIR
        double budget = 60;                         // RP_REPAIR_BUDGET, seconds
        double minGain = 1.0;                       // RP_REPAIR_MIN_GAIN
    } repair;

    bool scoreOrder = false;                        // RP_SCORE_ORDER (diagnostic; set = on)

    static Config fromEnv(int argc, char* argv[]);
};

#endif
