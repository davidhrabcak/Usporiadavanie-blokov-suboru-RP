#ifndef RP_ADJACENCY_HPP
#define RP_ADJACENCY_HPP

#include "chunk_meta.hpp"
#include <vector>

// True if chunk b can legally follow chunk a.
bool canFollow(const ChunkMeta& a, const ChunkMeta& b);

// adj[i] = list of chunk indices that can follow chunk i.
std::vector<std::vector<int>> buildAdjacency(const std::vector<ChunkMeta>& metas);

std::vector<int> computeInDegree(const std::vector<std::vector<int>>& adj, int n);
std::vector<int> computeOutDegree(const std::vector<std::vector<int>>& adj, int n);

#endif
