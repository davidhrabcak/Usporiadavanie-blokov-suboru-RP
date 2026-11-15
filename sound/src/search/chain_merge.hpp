#ifndef RP_CHAIN_MERGE_HPP
#define RP_CHAIN_MERGE_HPP

#include <cstdint>
#include <vector>
#include "mp3/chunk_meta.hpp"
#include "scoring/edge_lock.hpp"

// PROOF OF CONCEPT (RP_CHAIN=1): segment growing, aimed at CBR streams, where every chunk has
// n / period structural successors and the per-edge decode caps switch locking and the matching
// off.
//
// Every chunk starts as a segment of its own. Each round scores every open junction
// tail(s) -> head(t) ONCE, with the tail segment's own last chunks as the decoder context (so the
// context is exact as far as the segment is right; edge locking instead takes the median over up
// to 8 guessed predecessor contexts). The flux of a junction whose main data continues exactly
// (junctionSlack == 0, reservoir.hpp) is divided by 8. A junction is "good" when its flux beats every other
// successor of that tail and every other predecessor of that head by a factor >= tau; good
// junctions are joined, the others stay open and are paired again in the next round against the
// candidates that are left, with the longer contexts. tau goes down the schedule when a round
// joins nothing. Returns the joined edges (forced like locks by the caller). RP_CHAIN_DIAG=1
// prints the rank of the true successor and the continuity statistics of the structural edges.
// Measured 2026-11-15 (CLAUDE.md, Findings, "CBR"): not good enough to be the default.
std::vector<EdgeLock::LockedEdge> chainMerge(const std::vector<std::vector<uint8_t>>& chunks,
                                             const std::vector<ChunkMeta>& metas,
                                             const std::vector<std::vector<int>>& adj, int start,
                                             const std::vector<double>& taus, int maxLeft, int threads,
                                             const std::vector<int>& origIndexOf);

#endif
