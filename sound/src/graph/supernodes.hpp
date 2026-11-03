#ifndef RP_SUPERNODES_HPP
#define RP_SUPERNODES_HPP

#include <vector>

// Greedy forced-chain collapse: a supernode is a maximal forced chain, every interior node has
// out-degree 1 and its successor has in-degree 1. The search orders supernodes, not chunks.
struct Supernode {
    std::vector<int> chunks;   // chunk indices in order
};

/**
 * Build supernodes and the adjacency list over them.
 * @param adj Adjacency list of the input graph.
 * @param indeg List of degrees of only incoming edges for each vertex.
 * @param outdeg List of degrees of only outgoing edges for each vertex.
 * @param n Number of vertices in the graph.
 * @param start The index of the first chunk (pinned, never shuffled).
 * @param superAdj Output adjacency list.
 * @return List of created supernodes.
 */
std::vector<Supernode> buildSupernodes(const std::vector<std::vector<int>>& adj,
                                       const std::vector<int>& indeg,
                                       const std::vector<int>& outdeg,
                                       int n,
                                       int start,
                                       std::vector<std::vector<int>>& superAdj);

// The chunk sequence of a supernode order.
std::vector<int> chunksOf(const std::vector<Supernode>& supernodes, const std::vector<int>& order);

#endif
