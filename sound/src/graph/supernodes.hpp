#ifndef RP_SUPERNODES_HPP
#define RP_SUPERNODES_HPP

#include <vector>

// Forced-chain collapse: a supernode is a maximal chain in which every chunk but the last has
// exactly one successor, and that successor has exactly one predecessor. The search orders
// supernodes, not chunks.
struct Supernode {
    std::vector<int> chunks;   // chunk indices in order
};

// Builds the supernodes of `adj` and the adjacency list over them (`superAdj`). `start` (the
// first chunk) always begins a supernode.
std::vector<Supernode> buildSupernodes(const std::vector<std::vector<int>>& adj, int start,
                                       std::vector<std::vector<int>>& superAdj);

// The chunk sequence of a supernode order.
std::vector<int> chunksOf(const std::vector<Supernode>& supernodes, const std::vector<int>& order);

#endif
