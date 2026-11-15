#ifndef RP_ADJACENCY_HPP
#define RP_ADJACENCY_HPP

#include <vector>
#include "mp3/chunk_meta.hpp"

// The structural rule: chunk b can follow chunk a when a's cut tail frame ends exactly at one of
// b's frame starts.
//   Case 1: the tail frame's header is in a (or a ends on a frame boundary), so its length is
//           known: b needs a frame start at tailRemaining(a).
//   Case 2: only 1-3 header bytes are in a: the header is completed with b's first bytes, and b
//           needs a frame start where that frame ends.
bool canFollow(const ChunkMeta& a, const ChunkMeta& b);

// adj[i] = every chunk that can follow chunk i (canFollow on all pairs, through an index).
std::vector<std::vector<int>> buildAdjacency(const std::vector<ChunkMeta>& metas);

// Arc-consistency prune: in any full ordering every chunk but the last has one successor and
// every chunk but `start` one predecessor, so a chunk whose in- or out-neighbourhood is already a
// singleton forces that edge and lets the competing edges be deleted; deletions cascade to a
// fixpoint. Also deletes every edge into `start`. Returns the number of edges removed, or -1
// (with `adj` restored) if propagation hit a contradiction - which means `canFollow` is missing a
// real edge (e.g. a false sync), so forcing on the survivors would be unsafe.
long propagateArcConsistency(std::vector<std::vector<int>>& adj, int start);

#endif
