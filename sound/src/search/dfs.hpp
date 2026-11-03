#ifndef RP_DFS_HPP
#define RP_DFS_HPP

#include <chrono>
#include <climits>
#include <cstdint>
#include <vector>
#include "graph/supernodes.hpp"
#include "mp3/chunk_meta.hpp"
#include "scoring/junction_scorer.hpp"

// DFS over the supernode graph, from the supernode holding chunk 0. Candidates are tried in
// order of junction cost (JunctionScorer) and pruned by the bit-reservoir check, run per placed
// chunk. Bounded by a node cap, a wall-clock deadline and a beam width; iterative broadening
// runs it at growing beam widths (runSchedule).

struct DfsState {
    std::vector<int> order;    // supernode indices placed so far
    std::vector<bool> used;
    int reservoirLevel = 0;    // bit-reservoir fill level after everything placed so far
    long long score  = 0;      // summed gainCost over the junctions between placed supernodes
};

class DfsSearch {
public:
    struct Options {
        bool rank = true;                    // RP_RANK: order candidates by junction cost
        bool bestOf = false;                 // RP_BESTOF (see below)
        uint64_t maxNodes = 0;               // RP_MAX_NODES: placements per beam attempt, 0 = unlimited
        uint64_t progressEvery = 9999999;    // RP_PROGRESS
    };

    // `chunks`, `metas` and `scorer` are referenced and must outlive the search.
    DfsSearch(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
              JunctionScorer& scorer, const StreamProfile& profile, const Options& opt);

    // Runs the DFS from `start` at each beam width of `schedule` (0 = unlimited) until one
    // finds an ordering or the deadline passes, printing one line per width. `result` gets the
    // supernode order. With best-of, the lowest-scoring complete ordering found.
    bool runSchedule(const std::vector<Supernode>& supernodes, const std::vector<std::vector<int>>& superAdj,
                     const DfsState& start, const std::vector<int>& schedule, std::vector<int>& result);

    // Wall-clock stop for the next runSchedule calls; cleared by clearDeadline.
    void setDeadline(double seconds);
    void clearDeadline() { haveDeadline_ = false; }
    // Global matching hint: the matched successor of each supernode is tried first (-1 none).
    void setMatchHint(std::vector<int> succ) { matchSucc_ = std::move(succ); }
    void clearMatchHint() { matchSucc_.clear(); }

    // Deepest partial ordering reached since clearDeepest (supernode indices; diagnostic).
    const std::vector<int>& deepest() const { return deepest_; }
    void setDeepest(std::vector<int> order) { deepest_ = std::move(order); }
    void clearDeepest() { deepest_.clear(); }

    bool aborted() const { return aborted_; }       // the last attempt hit the node cap or deadline
    bool bestOf() const { return opt_.bestOf; }
    long long bestScore() const { return bestScore_; }
    uint64_t maxNodes() const { return opt_.maxNodes; }

private:
    /**
     * Exhaustive DFS over the supernode graph, starting from the supernode containing chunk 0.
     * The Layer III bit-reservoir check runs incrementally as each candidate supernode is
     * tried (see advanceReservoirChunk), instead of only once the whole order is complete - a
     * candidate that would break the reservoir chain is pruned immediately rather than explored
     * to the end, per CLAUDE.md TODO "Incremental validateReservoir inside the DFS".
     * @param state The current state of the search. state.reservoirLevel must already reflect
     *              every chunk in state.order.
     * @param result If found, the valid supernode sequence is filled into result.
     * @return True if a valid, decodable sequence of all supernodes exists.
     */
    bool dfs(DfsState& state, std::vector<int>& result);
    double elapsedSec() const;

    const std::vector<std::vector<uint8_t>>& chunks_;
    const std::vector<ChunkMeta>& metas_;
    JunctionScorer& scorer_;
    const bool isLayerIII_;
    const int sideInfo_;
    const int maxReservoir_;
    const Options opt_;

    // the graph of the current runSchedule call
    const std::vector<Supernode>* supernodes_ = nullptr;
    const std::vector<std::vector<int>>* superAdj_ = nullptr;

    // Instrumentation / bounding. `counter_` = number of supernode placements tried in the
    // current beam attempt; `aborted_` unwinds the whole recursion once the cap is hit, so a
    // bounded A/B run stops cleanly instead of being SIGTERM'd (which loses buffered stdout).
    uint64_t counter_ = 0;
    bool aborted_ = false;
    std::vector<int> deepest_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point deadline_;
    bool haveDeadline_ = false;
    std::vector<int> matchSucc_;

    // Best-of search (A1, RP_BESTOF=1): instead of returning the first complete valid ordering,
    // keep exploring (bounded by the deadline) and remember the lowest-scoring one, score =
    // summed gainCost over every supernode boundary; branch-and-bound prunes any prefix whose
    // partial score already meets/exceeds the best complete score.
    //
    // OFF by default: measured worse than first-valid. The summed-gainCost objective is
    // misaligned with truth - its global minimum is a "smoother than reality" reorder (sample-3s:
    // 25% of chunks in place vs 76-100% for first-valid), because a real stream's true order
    // genuinely contains large global_gain steps at transients/section edges. global_gain is a
    // good *local rank* signal, not a good *global objective*. Kept behind the flag for
    // experimenting with a better objective (reservoir margin, or a multi-feature score verified
    // to have the true order as its argmin). See CLAUDE.md item 7.
    long long bestScore_ = LLONG_MAX;
    std::vector<int> bestOrder_;

    // Beam width: at each DFS node, recurse into at most this many reservoir-valid candidates,
    // taken in continuity-rank order (0 = unlimited = exhaustive). A small beam turns the DFS
    // into near-greedy descent - it reaches full depth fast when the heuristic is right, but can
    // dead-end with no complete ordering when it is wrong at some node, so runSchedule widens it.
    int beamK_ = 0;
};

#endif
