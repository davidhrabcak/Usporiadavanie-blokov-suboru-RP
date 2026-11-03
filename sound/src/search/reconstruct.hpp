#ifndef RP_RECONSTRUCT_HPP
#define RP_RECONSTRUCT_HPP

#include <cstddef>
#include <limits>
#include <vector>
#include "config.hpp"
#include "graph/chunk_graph.hpp"
#include "pipeline/input.hpp"
#include "scoring/junction_scorer.hpp"

// The search for the chunk order, from the finished chunk graph:
//   for each lock threshold tau of the schedule (strictest first):
//     lock confident edges (edge_lock) + AC-3, collapse forced chains into supernodes,
//     DFS with iterative broadening; if it fails, the global successor matching (+ repair),
//     and if the matching's order is still reservoir-invalid, the DFS again with the matching
//     as ranking hint;
//   then the block-move repair of the order found.
// A wrong lock makes the true order unreachable for the DFS, hence the schedule: if no complete
// ordering is found at one tau, the whole lock + search is retried at the next one, from the
// unlocked graph. A complete-but-wrong ordering from a too-low tau is the repair's job (it
// moves blocks on the graph before AC-3 and locking, so it can undo a wrong lock).

struct Reconstruction {
    enum class Source { Dfs, DfsMatchRanked, Matching, SlotOracle };

    bool found = false;
    std::vector<int> seq;                 // chunk order
    Source source = Source::Dfs;
    int repairMoves = 0;                  // repair moves applied to `seq` (matching or final repair)
    double gap = std::numeric_limits<double>::quiet_NaN();   // repair's stability margin

    // when not found: the deepest partial path over all lock attempts
    bool budgetExhausted = false;         // the last search hit its node cap or deadline
    std::vector<int> partial;             // chunk indices
    size_t partialSupers = 0, partialOf = 0;
};

// Returns false on a fatal error (printed, run report "error" set). `graph.adj` is the
// starting graph of every lock attempt; the scorer must already be calibrated.
bool reconstruct(const Problem& p, const ChunkGraph& graph, JunctionScorer& scorer, const Config& cfg,
                 Reconstruction& out);

#endif
