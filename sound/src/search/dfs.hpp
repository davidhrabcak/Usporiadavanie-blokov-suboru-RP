#ifndef RP_DFS_HPP
#define RP_DFS_HPP

#include <chrono>
#include <cstdint>
#include <vector>
#include "graph/supernodes.hpp"
#include "mp3/chunk_meta.hpp"
#include "scoring/junction_scorer.hpp"

// DFS over the supernode graph, from the supernode holding chunk 0; returns the first complete
// ordering. Candidates are tried in order of junction cost (JunctionScorer) and pruned by the
// bit-reservoir check, run per placed chunk (HOW_IT_WORKS 4.4, 4.11). Bounded by a wall-clock
// deadline and a beam width; iterative broadening runs it at growing beam widths (runSchedule).

struct DfsState {
    std::vector<int> order;    // supernode indices placed so far
    std::vector<bool> used;
    int reservoirLevel = 0;    // bit-reservoir fill level after everything placed so far
};

class DfsSearch {
public:
    // `chunks`, `metas` and `scorer` are referenced and must outlive the search. `rank`
    // (RP_RANK): order candidates by junction cost.
    DfsSearch(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
              JunctionScorer& scorer, const StreamProfile& profile, bool rank);

    // Runs the DFS from `start` at each beam width of `schedule` (0 = unlimited) until one
    // finds an ordering or the deadline passes, printing one line per width. `result` gets the
    // supernode order.
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

    bool aborted() const { return aborted_; }       // the last attempt hit the deadline

private:
    // `state.reservoirLevel` must already reflect every chunk in `state.order`. True (with
    // `result` filled) once every supernode is placed.
    bool dfs(DfsState& state, std::vector<int>& result);
    double elapsedSec() const;

    const std::vector<std::vector<uint8_t>>& chunks_;
    const std::vector<ChunkMeta>& metas_;
    JunctionScorer& scorer_;
    const bool isLayerIII_;
    const int sideInfo_;
    const int maxReservoir_;
    const bool rank_;

    // the graph of the current runSchedule call
    const std::vector<Supernode>* supernodes_ = nullptr;
    const std::vector<std::vector<int>>* superAdj_ = nullptr;

    uint64_t counter_ = 0;     // supernode placements tried in the current beam attempt
    bool aborted_ = false;     // unwinds the whole recursion once the deadline has passed
    std::vector<int> deepest_;
    std::chrono::steady_clock::time_point start_;
    std::chrono::steady_clock::time_point deadline_;
    bool haveDeadline_ = false;
    std::vector<int> matchSucc_;

    // Beam width: at each DFS node, recurse into at most this many reservoir-valid candidates,
    // taken in continuity-rank order (0 = unlimited = exhaustive). A small beam turns the DFS
    // into near-greedy descent - it reaches full depth fast when the heuristic is right, but can
    // dead-end with no complete ordering when it is wrong at some node, so runSchedule widens it.
    int beamK_ = 0;
};

#endif
