#ifndef RP_CHUNK_GRAPH_HPP
#define RP_CHUNK_GRAPH_HPP

#include <cstdint>
#include <vector>
#include "config.hpp"
#include "mp3/chunk_meta.hpp"

// The chunk graph the search runs on, built in stages:
//   1. structural: buildAdjacency (canFollow);
//   2. reservoir arc filter: arcs that fail the bit-reservoir check from any entry level;
//   3. arc consistency (AC-3).
struct ChunkGraph {
    std::vector<std::vector<int>> adj;       // all stages: what locking and the search start from
    std::vector<std::vector<int>> adjFull;   // before AC-3 (and locking): the repair's move space
};

// Stage 2: drops every arc a->b that fails the reservoir check even when `a` is entered with a
// FULL reservoir. The check is monotone in the entry level (the only level-dependent test is
// mainDataBegin > level/8; a frame's output level doesn't depend on it), so such an arc fails in
// every context. Returns the number of arcs removed; `removedTrue` counts the true arcs among
// them (diagnostic, by `origIndexOf`).
long filterReservoirArcs(std::vector<std::vector<int>>& adj, const std::vector<ChunkMeta>& metas,
                         const std::vector<std::vector<uint8_t>>& chunks, int sideInfo, int maxReservoir,
                         const std::vector<int>& origIndexOf, long& removedTrue);

// All three stages, with their log lines. Chunk 0 is the start.
ChunkGraph buildGraph(const std::vector<ChunkMeta>& metas, const std::vector<std::vector<uint8_t>>& chunks,
                      const StreamProfile& profile, const Config& cfg, const std::vector<int>& origIndexOf);

#endif
