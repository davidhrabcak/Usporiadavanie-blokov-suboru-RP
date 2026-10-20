#ifndef RP_ADJACENCY_HPP
#define RP_ADJACENCY_HPP

#include "chunk_meta.hpp"
#include <vector>
//True if chunk b can legally follow chunk a.
bool canFollow(const ChunkMeta& a, const ChunkMeta& b);

std::vector<std::vector<int>> buildAdjacency(const std::vector<ChunkMeta>& metas);

// Arc-consistency prune (A4): in any full ordering every chunk but the last has one successor
// and every chunk but `start` one predecessor, so a chunk whose in- or out-neighbourhood is
// already a singleton forces that edge and lets the competing edges be deleted; deletions
// cascade to a fixpoint. Also deletes every edge into `start`. Returns the number of edges
// removed, or -1 (with `adj` restored) if propagation hit a contradiction - which means
// `canFollow` is missing a real edge (e.g. embedded non-audio, item 5), so forcing on the
// survivors would be unsafe.
long propagateArcConsistency(std::vector<std::vector<int>>& adj, int n, int start);

std::vector<int> computeInDegree(const std::vector<std::vector<int>>& adj, int n);
std::vector<int> computeOutDegree(const std::vector<std::vector<int>>& adj, int n);

#endif
