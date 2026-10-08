#include "edge_lock.hpp"

#include <algorithm>
#include <limits>
#include <unordered_map>

#include "pcm_junction.hpp"

namespace EdgeLock {

static constexpr double INF = std::numeric_limits<double>::infinity();
static constexpr double FLOOR = 0.1; // keeps margins finite when a flux is ~0

std::vector<LockedEdge> findConfidentEdges(const std::vector<std::vector<uint8_t>>& chunks,
                                           const std::vector<ChunkMeta>& metas,
                                           const std::vector<std::vector<int>>& adj,
                                           int start, double tau, int maxCtx, Mode mode,
                                           long* decodes) {
    const int n = static_cast<int>(chunks.size());
    std::vector<std::vector<int>> preds(n);
    for (int a = 0; a < n; ++a) for (int b : adj[a]) preds[b].push_back(a);

    // lo/hi[a] keyed by successor: best/worst-case flux over a's predecessor contexts.
    // hi = INF when any context couldn't be measured (then a->b can never be locked).
    // lo = INF when no context could be measured (then a->b can't be safely out-competed).
    std::vector<std::unordered_map<int, std::pair<double, double>>> score(n);
    std::vector<bool> scored(n, false);
    long nDecodes = 0;

    for (int a = 0; a < n; ++a) {
        if (!metas[a].valid || adj[a].empty()) continue;
        std::vector<int> ctx;
        if (a == start) ctx.push_back(-1);
        else {
            if (preds[a].empty()) continue;
            if ((int)preds[a].size() <= maxCtx) ctx = preds[a];
            else if (mode == Mode::Robust) continue;
            else for (int k = 0; k < maxCtx; ++k)   // evenly spaced sample
                ctx.push_back(preds[a][(size_t)k * preds[a].size() / maxCtx]);
        }
        scored[a] = true;
        for (int b : adj[a]) {
            double lo = INF, hi = -INF;
            bool anyBad = false;
            std::vector<double> vals;
            for (int p : ctx) {
                if (p == b) { continue; } // a 2-cycle context can't be a real path
                std::vector<const std::vector<uint8_t>*> segs;
                int first = a;
                if (p >= 0 && metas[p].valid) { segs.push_back(&chunks[p]); first = p; }
                segs.push_back(&chunks[a]);
                segs.push_back(&chunks[b]);
                const PcmJunction::Metrics m =
                    PcmJunction::scoreJunction(segs, (int)segs.size() - 1, metas[first].headOffset);
                ++nDecodes;
                if (!m.valid || m.silent) { anyBad = true; continue; }
                const double f = m.flux + FLOOR;
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
    }
    if (decodes) *decodes = nDecodes;

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
