#ifndef RP_CHUNK_GRAPH_HPP
#define RP_CHUNK_GRAPH_HPP

#include <cstdint>
#include <vector>
#include "config.hpp"
#include "graph/adjacency.hpp"
#include "mp3/chunk_meta.hpp"

// The chunk graph the search runs on, built in stages:
//   1. structural: buildAdjacency (canFollow) and, with headerless chunks taken out, the gap
//      slots after their demanders and the gap edges a => b (PLAN_headerless_chunks.md);
//   2. reservoir arc filter: arcs that fail the bit-reservoir check from any entry level;
//   3. arc consistency (AC-3).
// Stages 2 and 3 are only sound when the true order passed the same checks, so they are gated
// on the diagnostic run on the unshuffled chunks.

// Results of the diagnostic on the true (unshuffled) order; -1 = not computed.
struct TrueOrderCheck {
    int breaks = -1;            // consecutive pairs canFollow / canFollowGap rejects
    int reservoirBreaks = -1;   // chunks where the reservoir model rejects the true order
};

struct StructuralGraph {
    std::vector<std::vector<int>> adj;
    GapSlots gaps;   // empty without a pool
};

// Stage 1. Sets metas[i].gapAfter on the demanders.
StructuralGraph buildStructuralGraph(std::vector<ChunkMeta>& metas,
                                     const std::vector<std::vector<uint8_t>>& pool, long chunkSize);

// Stage 2: drops every arc a->b that fails the reservoir check even when `a` is entered with a
// FULL reservoir. The check is monotone in the entry level (the only level-dependent test is
// mainDataBegin > level/8; a frame's output level doesn't depend on it), so such an arc fails in
// every context. Returns the number of arcs removed; `removedTrue` counts the true arcs among
// them (diagnostic, by `origIndexOf`).
long filterReservoirArcs(std::vector<std::vector<int>>& adj, const std::vector<ChunkMeta>& metas,
                         const std::vector<std::vector<uint8_t>>& chunks, int sideInfo, int maxReservoir,
                         const std::vector<int>& origIndexOf, long& removedTrue);

struct ChunkGraph {
    std::vector<std::vector<int>> adj;       // all stages: what locking and the search start from
    std::vector<std::vector<int>> adjFull;   // before AC-3 (and locking): the repair's move space
    GapSlots gaps;
};

// All three stages, with their log lines. `splitTailsBefore`: split-tail chunks promoted on the
// true order, re-promoted here on the shuffled chunks (and checked to match).
ChunkGraph buildGraph(std::vector<ChunkMeta>& metas, const std::vector<std::vector<uint8_t>>& chunks,
                      const std::vector<std::vector<uint8_t>>& pool, const StreamProfile& profile,
                      long chunkSize, int splitTailsBefore, const TrueOrderCheck& truth,
                      const Config& cfg, const std::vector<int>& origIndexOf);

#endif
