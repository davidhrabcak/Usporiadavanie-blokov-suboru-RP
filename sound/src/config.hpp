#ifndef RP_CONFIG_HPP
#define RP_CONFIG_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Every RP_* option rp reads, parsed once at startup (Config::fromEnv). The full description of
// each one, with the measurements behind its default, is the "Environment variables" table in
// CLAUDE.md. RP_REPORT is the exception: run_report.hpp reads it when the report is written.

// A pass that runs only when its soundness gate holds (e.g. 0 pre-shuffle breaks). "0" in the
// environment turns it off, "1" forces it past the gate, anything else (or unset) is Auto.
enum class Switch { Auto, Off, On };

struct Config {
    static constexpr long kDefaultChunkSize = 1024;

    std::string input = "test/test7_shorter.mp3";   // argv[1]
    long chunkSize = kDefaultChunkSize;             // RP_CHUNK_SIZE (>= 1024)
    unsigned seed = 1;                              // RP_SEED
    bool randomSeed = false;                        // RP_SEED=random: seed from the clock
    bool startPinned = false;                       // RP_START=pinned

    // Graph construction
    bool cbrFilter = true;                          // RP_CBR
    bool gap = true;                                // RP_GAP
    Switch resvFilter = Switch::Auto;               // RP_RESV_FILTER
    Switch ac3 = Switch::Auto;                      // RP_AC3

    // Candidate ranking (junction cost = global_gain + spectral + PCM flux)
    bool rank = true;                               // RP_RANK
    bool spec = true;                               // RP_SPEC
    std::optional<double> specScale;                // RP_SPEC_SCALE (unset: calibrated)
    bool pcm = true;                                // RP_PCM
    double pcmWeight = 2.0;                         // RP_PCM_W
    std::optional<int> pcmLeft;                     // RP_PCM_LEFT (unset: from the frame length)

    struct Lock {
        Switch mode = Switch::Auto;                 // RP_LOCK
        std::vector<double> taus{1.5, 1.25, 1.1};   // RP_LOCK_TAU (comma list)
        int rounds = 8;                             // RP_LOCK_ROUNDS
        long maxDecodes = 640000;                   // RP_LOCK_MAX_DECODES
    } lock;
    int threads = 1;                                // RP_THREADS (default: all cores)

    struct Search {
        std::optional<int> beam;                    // RP_BEAM
        std::optional<bool> broaden;                // RP_BROADEN (unset: > 120 supernodes)
        std::optional<double> timeBudget;           // RP_TIME_BUDGET, seconds
        uint64_t maxNodes = 0;                      // RP_MAX_NODES (0 = unlimited)
        uint64_t progressEvery = 9999999;           // RP_PROGRESS
        bool bestOf = false;                        // RP_BESTOF
    } search;

    struct Match {
        int mode = 1;                               // RP_MATCH: 0 off, 1 after a failed DFS, 2 before
        long maxDecodes = 100000;                   // RP_MATCH_MAX_DECODES
        int iters = 0;                              // RP_MATCH_ITERS
    } match;

    struct Repair {
        Switch mode = Switch::Auto;                 // RP_REPAIR
        int passes = 50;                            // RP_REPAIR_PASSES
        std::optional<int> suspects;                // RP_REPAIR_SUSPECTS (unset: max(16, n/20))
        double budget = 60;                         // RP_REPAIR_BUDGET, seconds
        double minGain = 1.0;                       // RP_REPAIR_MIN_GAIN
    } repair;

    // Headerless chunks (frames larger than the chunk)
    bool slotFill = true;                           // RP_SLOT_FILL
    bool slotOracle = false;                        // RP_SLOT_ORACLE=1 (diagnostic)

    bool scoreOrder = false;                        // RP_SCORE_ORDER (diagnostic; set = on)

    static Config fromEnv(int argc, char* argv[]);
};

#endif
