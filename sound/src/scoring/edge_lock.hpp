#ifndef RP_EDGE_LOCK_HPP
#define RP_EDGE_LOCK_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <thread>
#include <cstdint>
#include <unordered_map>
#include <vector>
#include "mp3/chunk_meta.hpp"

// Confident-edge locking from the full-PCM junction flux (HOW_IT_WORKS 4.9).
//
// The flux score depends on the path context before the junction, which isn't known before
// the search. So every edge a->b is scored under the structural predecessors p of a (window
// [p, a, b]; at most `maxCtx` of them, evenly sampled), summarised by the median, and an edge is
// only locked when it wins clearly:
//   - successor side: flux(a->b) beats every other successor of a by a factor >= tau;
//   - predecessor side: likewise against every other predecessor a' of b.
// Mutual + margin means each chunk locks at most one successor and one predecessor. A locked
// edge is forced in the graph (all competing edges removed) before the search, so a wrong lock
// is unrecoverable there - precision is everything (measure with pcm_probe first).

namespace EdgeLock {

// Runs f(i) for i in [0, n) on up to `threads` threads.
template <class F> void parallelFor(const size_t n, int threads, F&& f) {
    threads = std::max(1, static_cast<int>(std::min<size_t>(threads, n)));
    if (threads == 1) { for (size_t i = 0; i < n; ++i) f(i); return; }
    std::atomic<size_t> next(0);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t)
        pool.emplace_back([&] { for (size_t i; (i = next.fetch_add(1)) < n;) f(i); });
    for (auto& th : pool) th.join();
}

struct LockedEdge {
    int a = -1, b = -1;
    double margin = 0;   // min(successor-side, predecessor-side) margin
};

// Flux of window [p, a, b] (p = -1: none), keyed by packKey. A window's flux depends only on
// those three chunks, so one memo can be shared across lock rounds and tau attempts.
using FluxMemo = std::unordered_map<uint64_t, float>;
// Warm-up chunks decoded before p (oldest first), when [p, a] alone holds too few frames
// (large frames vs the chunk size, PcmJunction::leftChunksNeeded). warm[p] must stay fixed for
// the memo's lifetime, so that a window is still determined by (p, a, b). nullptr / empty: none.
using WarmChains = std::vector<std::vector<int>>;
// Warm-up chain of `len` chunks for every p: a backward walk on `adj` that takes a forced
// predecessor (one whose only successor is the current chunk) when there is one, else the
// first. Usually not the true chunks, but the decoder only needs warming up, and the pre-side
// flux baseline is shared by every candidate b of the same context. Empty when len <= 0.
WarmChains buildWarmChains(const std::vector<std::vector<int>>& adj, int len);
inline uint64_t packKey(int p, int a, int b) {
    return ((uint64_t)(unsigned)(p + 1) << 42) | ((uint64_t)(unsigned)a << 21) | (uint64_t)(unsigned)b;
}

// `start` is the first chunk (scored with no context). `memo` is read and extended; `decodes`
// counts real decodes (memo misses). `threads` > 1 scores chunks in parallel (the result is
// identical to a serial run).
std::vector<LockedEdge> findConfidentEdges(const std::vector<std::vector<uint8_t>>& chunks,
                                           const std::vector<ChunkMeta>& metas,
                                           const std::vector<std::vector<int>>& adj,
                                           int start, double tau, int maxCtx, FluxMemo& memo,
                                           int threads, const WarmChains* warm, long* decodes = nullptr);

// Scores every window [p, a, b] in `windows` (p = -1: none) that isn't in `memo` yet, on
// `threads` threads, and adds the results to `memo` (NaN = unmeasurable). Returns the number
// of decodes. Used by the global successor matching for its edge costs.
long scoreWindows(const std::vector<std::vector<uint8_t>>& chunks,
                  const std::vector<ChunkMeta>& metas,
                  const std::vector<std::array<int, 3>>& windows,
                  FluxMemo& memo, int threads, const WarmChains* warm = nullptr);

// Forces every locked edge into `adj`: a's only successor becomes b, and b is removed from
// every other chunk's successor list. Returns the number of edges removed.
long applyLocks(std::vector<std::vector<int>>& adj, const std::vector<LockedEdge>& locks);

} // namespace EdgeLock

#endif
