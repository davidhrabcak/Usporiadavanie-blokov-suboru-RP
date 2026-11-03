#ifndef RP_GLOBAL_MATCH_HPP
#define RP_GLOBAL_MATCH_HPP

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>
#include "graph/supernodes.hpp"
#include "mp3/chunk_meta.hpp"
#include "scoring/edge_lock.hpp"

// Global min-cost successor matching (method 1 of "global search on summed flux").
// The DFS uses flux locally: one wrong early pick strands it. Instead, give EVERY supernode
// exactly one successor and one predecessor so that the summed edge cost (log1p of the median
// junction flux) is minimal - an assignment problem, solved exactly by successive shortest
// paths on the sparse bipartite graph. The start supernode has no predecessor; a virtual END
// takes the last one (only the final-fragment chunk's supernode if there is one). The result
// is a path from the start plus possibly some cycles; each cycle is cut at its best edge and
// spliced into the path where both new junctions are structural edges, at minimal added cost.
// The caller then checks the bit reservoir and runs the repair. Edge context: a supernode of
// >= 2 chunks gives its own second-to-last chunk as the window's warm-up chunk; a single-chunk
// one uses the median over (up to 8 sampled) structural predecessors, as in edge locking.

struct MatchResult {
    std::vector<int> order;            // supernode order (start first), empty on failure
    std::vector<int> succ;             // matched successor per supernode (-1 = END)
    int cycles = 0, spliced = 0;
    double cost = 0;
    long decodes = 0;
    int contexts = 8;                  // predecessor contexts per single-chunk supernode (decode cap)
    std::string failure;
};

// `chunkAdj`: the chunk graph the supernodes were built from (for single-chunk contexts).
// Window fluxes are read from and added to `memo`, scored on `threads` threads. `maxDecodes`
// (RP_MATCH_MAX_DECODES) caps the new window decodes. `forbidden` (optional): supernode arcs
// s->t, keyed (s << 32) | t, the matching may not use (the iterated matching forbids the arcs
// at reservoir breaks).
MatchResult globalMatch(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<Supernode>& sn,
                        const std::vector<std::vector<int>>& superAdj, int startSuper,
                        const std::vector<std::vector<int>>& chunkAdj, const std::vector<ChunkMeta>& metas,
                        EdgeLock::FluxMemo& memo, int threads, const EdgeLock::WarmChains* warm, long maxDecodes,
                        const std::unordered_set<uint64_t>* forbidden = nullptr);

#endif
