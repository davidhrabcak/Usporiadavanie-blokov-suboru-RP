#ifndef RP_EDGE_LOCK_HPP
#define RP_EDGE_LOCK_HPP

#include <cstdint>
#include <unordered_map>
#include <vector>
#include "chunk_meta.hpp"

// Confident-edge locking from the full-PCM junction flux (PLAN_full_pcm_decode.md, step 5).
//
// The flux score depends on the path context before the junction, which isn't known before
// the search. So every edge a->b is scored under EVERY structural predecessor p of a (window
// [p, a, b], 1 warm-up chunk), and an edge is only locked when it wins robustly:
//   - successor side: worst-case flux(a->b) beats the best-case flux of every other
//     successor of a by a factor >= tau;
//   - predecessor side: likewise against every other predecessor a' of b.
// Mutual + margin means each chunk locks at most one successor and one predecessor. A locked
// edge is meant to be forced in the graph (all competing edges removed) before the DFS, so a
// wrong lock is unrecoverable - precision is everything (measure with pcm_probe first).

namespace EdgeLock {

struct LockedEdge {
    int a = -1, b = -1;
    double margin = 0;   // min(successor-side, predecessor-side) margin
};

// How an edge's per-context fluxes are summarised before comparing.
//   Robust: the locked edge's WORST context must beat every competitor's BEST context.
//           Chunks with > maxCtx predecessors are skipped entirely (never lock, never
//           out-competed). Very conservative.
//   Median: compare per-edge medians over the contexts; chunks with > maxCtx predecessors
//           are scored on an evenly spaced sample of maxCtx of them.
enum class Mode { Robust, Median };

// Flux of window [p, a, b] (p = -1: none), keyed by packKey. A window's flux depends only on
// those three chunks, so one memo can be shared across lock rounds and tau attempts.
using FluxMemo = std::unordered_map<uint64_t, float>;
inline uint64_t packKey(int p, int a, int b) {
    return ((uint64_t)(unsigned)(p + 1) << 42) | ((uint64_t)(unsigned)a << 21) | (uint64_t)(unsigned)b;
}

// `start` is the pinned first chunk (scored with no context). `memo` (optional) is read and
// extended; `decodes` counts real decodes (memo misses). `threads` > 1 scores chunks in
// parallel (the result is identical to a serial run).
std::vector<LockedEdge> findConfidentEdges(const std::vector<std::vector<uint8_t>>& chunks,
                                           const std::vector<ChunkMeta>& metas,
                                           const std::vector<std::vector<int>>& adj,
                                           int start, double tau, int maxCtx,
                                           Mode mode = Mode::Robust, long* decodes = nullptr,
                                           FluxMemo* memo = nullptr, int threads = 1);

// Forces every locked edge into `adj`: a's only successor becomes b, and b is removed from
// every other chunk's successor list. Returns the number of edges removed.
long applyLocks(std::vector<std::vector<int>>& adj, const std::vector<LockedEdge>& locks);

} // namespace EdgeLock

#endif
