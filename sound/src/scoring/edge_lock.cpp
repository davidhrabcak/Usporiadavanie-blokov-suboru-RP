#include "scoring/edge_lock.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>
#include <limits>
#include <memory>
#include <unordered_map>

#include "scoring/pcm_junction.hpp"

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
    for (size_t k = f; k < ids.size(); ++k) segs.push_back(&chunks[ids[k]]);
    return metas[ids[f]].headOffset;
}

static float fluxOf(const PcmJunction::Metrics& m) {
    return (!m.valid || m.silent) ? std::numeric_limits<float>::quiet_NaN() : static_cast<float>(m.flux);
}

std::vector<LockedEdge> findConfidentEdges(const std::vector<std::vector<uint8_t>>& chunks,
                                           const std::vector<ChunkMeta>& metas,
                                           const std::vector<std::vector<int>>& adj,
                                           int start, double tau, int maxCtx, FluxMemo& memo,
                                           int threads, const WarmChains* warm, long* decodes) {
    const int n = static_cast<int>(chunks.size());
    std::vector<std::vector<int>> preds(n);
    for (int a = 0; a < n; ++a) for (int b : adj[a]) preds[b].push_back(a);

    // score[a][b]: median flux of a->b over a's predecessor contexts; INF when no context could
    // be measured (then a->b can't be locked, and can't be out-competed either).
    std::vector<std::unordered_map<int, double>> score(n);
    std::vector<char> scored(n, 0);
    // Per-chunk new memo entries (merged after the parallel section; NaN = unmeasurable).
    std::vector<std::vector<std::pair<uint64_t, float>>> fresh(n);

    auto scoreChunk = [&](const size_t ai) {
        const int a = static_cast<int>(ai);
        if (!metas[a].valid || adj[a].empty()) return;
        std::vector<int> ctx;
        if (a == start) ctx.push_back(-1);
        else {
            if (preds[a].empty()) return;
            if ((int)preds[a].size() <= maxCtx) ctx = preds[a];
            else for (int k = 0; k < maxCtx; ++k)   // evenly spaced sample
                ctx.push_back(preds[a][(size_t)k * preds[a].size() / maxCtx]);
        }
        scored[a] = 1;
        // left side [p, a] decoded once per context, on its first memo miss
        std::vector<std::unique_ptr<PcmJunction::LeftContext>> left(ctx.size());
        for (int b : adj[a]) {
            std::vector<double> vals;
            for (size_t ci = 0; ci < ctx.size(); ++ci) {
                const int p = ctx[ci];
                if (p == b) continue;   // a 2-cycle context can't be a real path
                const int pp = (p >= 0 && metas[p].valid) ? p : -1;
                const uint64_t key = packKey(pp, a, b);
                float fx;
                const auto it = memo.find(key);
                if (it != memo.end()) {
                    fx = it->second;
                } else {
                    if (!left[ci]) {
                        std::vector<const std::vector<uint8_t>*> segs;
                        const int off = leftSegs(chunks, metas, pp, a, warm, segs);
                        left[ci].reset(new PcmJunction::LeftContext(segs, off));
                    }
                    fx = fluxOf(left[ci]->score(chunks[b]));
                    fresh[a].push_back({key, fx});
                }
                if (!std::isnan(fx)) vals.push_back(fx + FLOOR);
            }
            if (vals.empty()) { score[a][b] = INF; continue; }
            std::nth_element(vals.begin(), vals.begin() + vals.size() / 2, vals.end());
            score[a][b] = vals[vals.size() / 2];
        }
    };

    // Each chunk writes only its own slots (score[a], scored[a], fresh[a]) and the memo is
    // read-only until the merge below, so the parallel run is race-free.
    parallelFor(n, threads, scoreChunk);
    long total = 0;
    for (int a = 0; a < n; ++a) {
        total += static_cast<long>(fresh[a].size());
        for (const auto& kv : fresh[a]) memo.emplace(kv.first, kv.second);
    }
    if (decodes) *decodes = total;

    std::vector<LockedEdge> out;
    for (int a = 0; a < n; ++a) {
        if (!scored[a] || adj[a].empty()) continue;
        // successor side
        int best = -1; double bestFlux = INF;
        for (int b : adj[a]) {
            const double f = score[a][b];
            if (f < bestFlux) { bestFlux = f; best = b; }
        }
        if (best < 0) continue;
        double succMargin = INF;
        for (int c : adj[a]) if (c != best) succMargin = std::min(succMargin, score[a][c] / bestFlux);
        // predecessor side: every other predecessor a' of `best` must have been scored
        double predMargin = INF;
        bool predOk = true;
        for (int a2 : preds[best]) {
            if (a2 == a) continue;
            if (!scored[a2]) { predOk = false; break; }
            predMargin = std::min(predMargin, score[a2][best] / bestFlux);
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
        const PcmJunction::LeftContext left(segs, off);
        for (size_t i = groups[g]; i < groups[g + 1]; ++i) out[i] = fluxOf(left.score(chunks[uniq[i][2]]));
    };
    parallelFor(groups.size() - 1, threads, one);
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
