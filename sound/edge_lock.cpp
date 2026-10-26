#include "edge_lock.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <limits>
#include <memory>
#include <unordered_map>

#include "pcm_junction.hpp"

namespace EdgeLock {

static constexpr double INF = std::numeric_limits<double>::infinity();
static constexpr double FLOOR = 0.1; // keeps margins finite when a flux is ~0

WarmChains buildWarmChains(const std::vector<std::vector<int>>& adj, int len) {
    WarmChains warm;
    if (len <= 0) return warm;
    const int n = static_cast<int>(adj.size());
    std::vector<std::vector<int>> preds(n);
    for (int a = 0; a < n; ++a) for (int b : adj[a]) preds[b].push_back(a);
    warm.assign(n, {});
    for (int p = 0; p < n; ++p) {
        std::vector<int> chain;   // newest first
        int x = p;
        for (int d = 0; d < len; ++d) {
            int q = -1;
            for (int c : preds[x]) {
                if (c == p || std::find(chain.begin(), chain.end(), c) != chain.end()) continue;
                if (adj[c].size() == 1) { q = c; break; }
                if (q < 0) q = c;
            }
            if (q < 0) break;
            chain.push_back(q);
            x = q;
        }
        warm[p].assign(chain.rbegin(), chain.rend());
    }
    return warm;
}

// Left side of window [p, a, .]: warm[p] (oldest first), p, a. Returns the decode start offset
// (headOffset of the first chunk). Invalid leading chunks are dropped (no usable headOffset).
static int leftSegs(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
                    int p, int a, const WarmChains* warm, std::vector<const std::vector<uint8_t>*>& segs) {
    std::vector<int> ids;
    if (p >= 0) {
        if (warm && p < (int)warm->size()) {
            const auto& w = (*warm)[p];
            // a chain reached through a cycle can contain `a`: keep only what follows it
            auto it = std::find(w.rbegin(), w.rend(), a);
            ids.assign(it.base(), w.end());
        }
        ids.push_back(p);
    }
    ids.push_back(a);
    size_t f = 0;
    while (f + 1 < ids.size() && !metas[ids[f]].valid) ++f;
    segs.clear();
    // spans a slot (headerless chunk not placed yet): no decodable window
    for (size_t k = f; k < ids.size(); ++k) if (metas[ids[k]].gapAfter) return -1;
    for (size_t k = f; k < ids.size(); ++k) segs.push_back(&chunks[ids[k]]);
    return metas[ids[f]].headOffset;
}

std::vector<LockedEdge> findConfidentEdges(const std::vector<std::vector<uint8_t>>& chunks,
                                           const std::vector<ChunkMeta>& metas,
                                           const std::vector<std::vector<int>>& adj,
                                           int start, double tau, int maxCtx, Mode mode,
                                           long* decodes, FluxMemo* memo, int threads,
                                           const WarmChains* warm) {
    const int n = static_cast<int>(chunks.size());
    std::vector<std::vector<int>> preds(n);
    for (int a = 0; a < n; ++a) for (int b : adj[a]) preds[b].push_back(a);

    // lo/hi[a] keyed by successor: best/worst-case flux over a's predecessor contexts.
    // hi = INF when any context couldn't be measured (then a->b can never be locked).
    // lo = INF when no context could be measured (then a->b can't be safely out-competed).
    std::vector<std::unordered_map<int, std::pair<double, double>>> score(n);
    std::vector<char> scored(n, 0);
    // Per-chunk new memo entries (merged after the parallel section; NaN = unmeasurable).
    std::vector<std::vector<std::pair<uint64_t, float>>> fresh(n);
    std::vector<long> nDec(n, 0);

    auto scoreChunk = [&](int a) {
        if (!metas[a].valid || adj[a].empty()) return;
        // A demander's edges cross a slot: never scored, so never locked - and, unscored, it
        // blocks every lock into its successors on the predecessor side below, where its
        // invisible true edge could otherwise be out-competed (PLAN_headerless_chunks.md).
        if (metas[a].gapAfter) return;
        std::vector<int> ctx;
        if (a == start) ctx.push_back(-1);
        else {
            if (preds[a].empty()) return;
            if ((int)preds[a].size() <= maxCtx) ctx = preds[a];
            else if (mode == Mode::Robust) return;
            else for (int k = 0; k < maxCtx; ++k)   // evenly spaced sample
                ctx.push_back(preds[a][(size_t)k * preds[a].size() / maxCtx]);
        }
        scored[a] = 1;
        // left side [p, a] decoded once per context, on its first memo miss (PcmJunction::LeftContext)
        std::vector<std::unique_ptr<PcmJunction::LeftContext>> left(ctx.size());
        for (int b : adj[a]) {
            double lo = INF, hi = -INF;
            bool anyBad = false;
            std::vector<double> vals;
            for (size_t ci = 0; ci < ctx.size(); ++ci) {
                const int p = ctx[ci];
                if (p == b) { continue; } // a 2-cycle context can't be a real path
                const int pp = (p >= 0 && metas[p].valid) ? p : -1;
                const uint64_t key = packKey(pp, a, b);
                float fx;
                auto it = memo ? memo->find(key) : FluxMemo::iterator();
                if (memo && it != memo->end()) {
                    fx = it->second;
                } else {
                    if (!left[ci]) {
                        std::vector<const std::vector<uint8_t>*> segs;
                        const int off = leftSegs(chunks, metas, pp, a, warm, segs);
                        if (off >= 0) left[ci].reset(new PcmJunction::LeftContext(segs, off));
                    }
                    if (left[ci]) {
                        const PcmJunction::Metrics m = left[ci]->score(chunks[b]);
                        ++nDec[a];
                        fx = (!m.valid || m.silent) ? std::numeric_limits<float>::quiet_NaN() : (float)m.flux;
                    } else {
                        fx = std::numeric_limits<float>::quiet_NaN();   // context spans a slot
                    }
                    if (memo) fresh[a].push_back({key, fx});
                }
                if (std::isnan(fx)) { anyBad = true; continue; }
                const double f = fx + FLOOR;
                lo = std::min(lo, f);
                hi = std::max(hi, f);
                vals.push_back(f);
            }
            if (mode == Mode::Median) {
                // Both "best" and "worst" collapse to the median of the measurable contexts;
                // unmeasurable everywhere -> INF on both (can't lock, can't out-compete).
                if (vals.empty()) { score[a][b] = {INF, INF}; continue; }
                std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
                const double med = vals[vals.size() / 2];
                score[a][b] = {med, med};
                continue;
            }
            if (anyBad || hi < 0) hi = INF;
            score[a][b] = {lo, hi};
        }
    };

    // Each chunk writes only its own slots (score[a], scored[a], fresh[a], nDec[a]) and the
    // memo is read-only until the merge below, so the parallel run is race-free.
    threads = std::max(1, std::min(threads, n));
    if (threads == 1) {
        for (int a = 0; a < n; ++a) scoreChunk(a);
    } else {
        std::atomic<int> next(0);
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&] { for (int a; (a = next.fetch_add(1)) < n;) scoreChunk(a); });
        for (auto& th : pool) th.join();
    }
    long total = 0;
    for (int a = 0; a < n; ++a) {
        total += nDec[a];
        if (memo) for (const auto& kv : fresh[a]) memo->emplace(kv.first, kv.second);
    }
    if (decodes) *decodes = total;

    std::vector<LockedEdge> out;
    for (int a = 0; a < n; ++a) {
        if (!scored[a] || adj[a].empty()) continue;
        // successor side
        int best = -1; double bestHi = INF;
        for (int b : adj[a]) {
            const double h = score[a][b].second;
            if (h < bestHi) { bestHi = h; best = b; }
        }
        if (best < 0 || bestHi == INF) continue;
        double succMargin = INF;
        for (int c : adj[a]) if (c != best) succMargin = std::min(succMargin, score[a][c].first / bestHi);
        // predecessor side: every other predecessor a' of `best` must have been scored
        double predMargin = INF;
        bool predOk = true;
        for (int a2 : preds[best]) {
            if (a2 == a) continue;
            if (!scored[a2]) { predOk = false; break; }
            predMargin = std::min(predMargin, score[a2][best].first / bestHi);
        }
        if (!predOk) continue;
        if (succMargin == INF && predMargin == INF) continue; // already forced, nothing to lock
        const double m = std::min(succMargin, predMargin);
        if (m >= tau) out.push_back({a, best, m});
    }
    return out;
}

long scoreWindows(const std::vector<std::vector<uint8_t>>& chunks,
                  const std::vector<ChunkMeta>& metas,
                  const std::vector<std::array<int, 3>>& windows,
                  FluxMemo& memo, int threads, const WarmChains* warm) {
    std::vector<std::array<int, 3>> todo;
    for (const auto& w : windows) {
        const int p = (w[0] >= 0 && metas[w[0]].valid) ? w[0] : -1;
        if (memo.count(packKey(p, w[1], w[2]))) continue;
        todo.push_back({p, w[1], w[2]});
    }
    std::vector<std::array<int, 3>> uniq;
    { std::unordered_map<uint64_t, char> seen;
      for (const auto& w : todo) if (seen.emplace(packKey(w[0], w[1], w[2]), 1).second) uniq.push_back(w); }
    // Windows sharing a left side [p, a] are scored from one decoded LeftContext.
    std::sort(uniq.begin(), uniq.end());
    std::vector<size_t> groups;   // start index of each (p, a) group
    for (size_t i = 0; i < uniq.size(); ++i)
        if (i == 0 || uniq[i][0] != uniq[i - 1][0] || uniq[i][1] != uniq[i - 1][1]) groups.push_back(i);
    groups.push_back(uniq.size());
    std::vector<float> out(uniq.size());
    auto one = [&](size_t g) {
        const auto& w0 = uniq[groups[g]];
        std::vector<const std::vector<uint8_t>*> segs;
        const int off = leftSegs(chunks, metas, w0[0], w0[1], warm, segs);
        if (off < 0) {   // spans a slot
            for (size_t i = groups[g]; i < groups[g + 1]; ++i) out[i] = std::numeric_limits<float>::quiet_NaN();
            return;
        }
        const PcmJunction::LeftContext left(segs, off);
        for (size_t i = groups[g]; i < groups[g + 1]; ++i) {
            const PcmJunction::Metrics m = left.score(chunks[uniq[i][2]]);
            out[i] = (!m.valid || m.silent) ? std::numeric_limits<float>::quiet_NaN() : (float)m.flux;
        }
    };
    const size_t nGroups = groups.size() - 1;
    threads = std::max(1, std::min<int>(threads, (int)nGroups));
    if (threads <= 1) { for (size_t g = 0; g < nGroups; ++g) one(g); }
    else {
        std::atomic<size_t> next(0);
        std::vector<std::thread> pool;
        for (int t = 0; t < threads; ++t)
            pool.emplace_back([&] { for (size_t g; (g = next.fetch_add(1)) < nGroups;) one(g); });
        for (auto& th : pool) th.join();
    }
    for (size_t i = 0; i < uniq.size(); ++i) memo.emplace(packKey(uniq[i][0], uniq[i][1], uniq[i][2]), out[i]);
    return (long)uniq.size();
}

long applyLocks(std::vector<std::vector<int>>& adj, const std::vector<LockedEdge>& locks) {
    const int n = static_cast<int>(adj.size());
    std::vector<int> lockedPred(n, -1);
    for (const auto& e : locks) lockedPred[e.b] = e.a;
    long removed = 0;
    for (const auto& e : locks) {
        removed += (long)adj[e.a].size() - 1;
        adj[e.a] = {e.b};
    }
    for (int x = 0; x < n; ++x) {
        auto& v = adj[x];
        const size_t before = v.size();
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](int b) { return lockedPred[b] >= 0 && lockedPred[b] != x; }),
                v.end());
        removed += (long)(before - v.size());
    }
    return removed;
}

} // namespace EdgeLock
