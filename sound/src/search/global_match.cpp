#include "search/global_match.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <queue>
#include <unordered_map>

using namespace std;

namespace {

class Matcher {
public:
    Matcher(const vector<Supernode>& sn, const vector<vector<int>>& superAdj, const int startSuper,
            const vector<vector<int>>& chunkAdj, const vector<ChunkMeta>& metas,
            const std::unordered_set<uint64_t>* forbidden)
        : sn_(sn), superAdj_(superAdj), startSuper_(startSuper), metas_(metas), forbidden_(forbidden),
          N_(static_cast<int>(sn.size())), preds_(chunkAdj.size()), ctxOf_(N_) {
        // chunk-level predecessors in the current (locked) graph, for single-chunk contexts
        for (size_t a = 0; a < chunkAdj.size(); ++a) for (int b : chunkAdj[a]) preds_[b].push_back((int)a);
        for (int s = 0; s < N_; ++s) if (metas[sn[s].chunks.back()].finalFragment) fragSuper_ = s;
    }

    // Decode cap: dense low-bitrate graphs (bohemoan 32 kbps at 1-2 KB: ~700 supernodes x ~140
    // successors x 8 contexts) took > 15 min just to score. Halve the number of predecessor
    // contexts per single-chunk supernode until the windows not yet in the memo fit under
    // `cap`; fail if even one context doesn't fit. Count-based (not time-based) so whether and
    // how the matching runs doesn't depend on the core count.
    bool chooseContexts(const EdgeLock::FluxMemo& memo, const long cap, vector<std::array<int, 3>>& windows,
                        MatchResult& res) {
        // The windows are only materialised once a context count fits: on a dense graph (Siddhartha
        // 256 kbps at 1 KB, 12.5M edges) building them and the miss set for every count before
        // finding that none fits took 5 GB and 86 s. Counting stops as soon as the cap is passed.
        for (int maxCtx = 8; ; maxCtx /= 2) {
            for (int s = 0; s < N_; ++s) ctxOf_[s] = contexts(s, maxCtx);
            std::unordered_set<uint64_t> miss;   // never grows past cap + 1
            const bool fits = forEachWindow([&](int p, int a, int b) {
                const uint64_t k = EdgeLock::packKey((p >= 0 && metas_[p].valid) ? p : -1, a, b);
                if (!memo.count(k)) miss.insert(k);
                return (long)miss.size() <= cap;
            });
            res.contexts = maxCtx;
            if (fits) {
                windows.clear();
                forEachWindow([&](int p, int a, int b) { windows.push_back({p, a, b}); return true; });
                cout << "Global matching: scoring " << miss.size() << " new windows (<= " << maxCtx
                     << " contexts per edge) ...\n" << flush;
                return true;
            }
            if (maxCtx == 1) {
                res.failure = "skipped (> " + std::to_string(cap) +
                              " window decodes even with 1 context; RP_MATCH_MAX_DECODES raises the cap)";
                return false;
            }
        }
    }

    // Calls f(p, a, b) for every window [p, a, b] of the current contexts, in a fixed order;
    // stops and returns false as soon as f does.
    template <class F> bool forEachWindow(F&& f) const {
        for (int s = 0; s < N_; ++s) {
            const int a = sn_[s].chunks.back();
            for (int t : superAdj_[s]) {
                const int b = sn_[t].chunks.front();
                for (int p : ctxOf_[s]) if (p != b && !f(p, a, b)) return false;
            }
        }
        return true;
    }

    // Edge cost s -> t: log1p of the median flux over the contexts; unmeasurable edges (and
    // END) get the median edge cost.
    void edgeCosts(const EdgeLock::FluxMemo& memo) {
        vector<double> finite;
        for (int s = 0; s < N_; ++s) {
            const int a = sn_[s].chunks.back();
            for (int t : superAdj_[s]) {
                const int b = sn_[t].chunks.front();
                vector<double> v;
                for (int p : ctxOf_[s]) {
                    if (p == b) continue;
                    const int pp = (p >= 0 && metas_[p].valid) ? p : -1;
                    auto it = memo.find(EdgeLock::packKey(pp, a, b));
                    if (it != memo.end() && !std::isnan(it->second)) v.push_back(it->second);
                }
                double c = std::numeric_limits<double>::quiet_NaN();
                if (!v.empty()) {
                    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                    c = std::log1p(v[v.size() / 2]);
                    finite.push_back(c);
                }
                cost_[ekey(s, t)] = c;
            }
        }
        neutral_ = 1.0;
        if (!finite.empty()) {
            std::nth_element(finite.begin(), finite.begin() + finite.size() / 2, finite.end());
            neutral_ = finite[finite.size() / 2];
        }
        for (auto& kv : cost_) if (std::isnan(kv.second)) kv.second = neutral_;
    }

    // Min-cost perfect matching, left = supernodes as predecessors, right = supernodes as
    // successors, with right slot `startSuper` standing for END. SSP + Dijkstra + potentials.
    bool assignSuccessors(MatchResult& res) {
        const int N = N_;
        const int S = 2 * N, T = 2 * N + 1, V = 2 * N + 2;
        struct E { int to, cap; long long w; };
        vector<E> e;
        vector<vector<int>> g(V);
        auto add = [&](int u, int v, long long w) {
            g[u].push_back((int)e.size()); e.push_back({v, 1, w});
            g[v].push_back((int)e.size()); e.push_back({u, 0, -w});
        };
        const double SCALE = 1e4;
        for (int s = 0; s < N; ++s) {
            add(S, s, 0);
            add(N + s, T, 0);
            for (int t : superAdj_[s]) if (t != startSuper_ && !banned(s, t)) add(s, N + t, std::llround(c(s, t) * SCALE));
            if (mayEnd(s)) add(s, N + startSuper_, std::llround(neutral_ * SCALE));
        }
        const long long INF = std::numeric_limits<long long>::max() / 4;
        vector<long long> h(V, 0), dist(V);
        vector<int> pe(V);
        int flow = 0;
        while (flow < N) {
            std::fill(dist.begin(), dist.end(), INF);
            dist[S] = 0;
            std::priority_queue<pair<long long, int>, vector<pair<long long, int>>, std::greater<>> pq;
            pq.push({0, S});
            while (!pq.empty()) {
                const long long d = pq.top().first; const int u = pq.top().second; pq.pop();
                if (d > dist[u]) continue;
                for (int id : g[u]) {
                    const E& ed = e[id];
                    if (!ed.cap) continue;
                    const long long nd = d + ed.w + h[u] - h[ed.to];
                    if (nd < dist[ed.to]) { dist[ed.to] = nd; pe[ed.to] = id; pq.push({nd, ed.to}); }
                }
            }
            if (dist[T] >= INF) break;
            for (int v = 0; v < V; ++v) if (dist[v] < INF) h[v] += dist[v];
            for (int v = T; v != S; v = e[pe[v] ^ 1].to) { e[pe[v]].cap -= 1; e[pe[v] ^ 1].cap += 1; }
            ++flow;
        }
        if (flow < N) { res.failure = "no perfect successor matching"; return false; }
        res.succ.assign(N, -1);
        for (int s = 0; s < N; ++s)
            for (int id : g[s]) {
                const E& ed = e[id];
                if (ed.to >= N && ed.to < 2 * N && ed.cap == 0 && (id % 2 == 0)) {
                    const int t = ed.to - N;
                    res.succ[s] = (t == startSuper_) ? -1 : t;
                    res.cost += (t == startSuper_) ? neutral_ : c(s, t);
                }
            }
        return true;
    }

    // The path from the start, then the cycles; each cycle is spliced in, cheapest first, until
    // none is left (or none fits).
    bool spliceCycles(MatchResult& res) {
        const int N = N_;
        vector<char> seen(N, 0);
        vector<int> path;
        for (int s = startSuper_; s >= 0 && !seen[s]; s = res.succ[s]) { seen[s] = 1; path.push_back(s); }
        vector<vector<int>> cyc;
        for (int s = 0; s < N; ++s) {
            if (seen[s]) continue;
            vector<int> cy;
            for (int x = s; !seen[x]; x = res.succ[x]) { seen[x] = 1; cy.push_back(x); }
            cyc.push_back(cy);
        }
        res.cycles = (int)cyc.size();
        std::unordered_set<uint64_t> isEdge;
        vector<vector<int>> spred(N);
        for (int s = 0; s < N; ++s) for (int t : superAdj_[s]) { isEdge.insert(ekey(s, t)); spred[t].push_back(s); }
        auto edge = [&](int s, int t) { return t < 0 ? mayEnd(s) : isEdge.count(ekey(s, t)) > 0 && !banned(s, t); };
        while (!cyc.empty()) {
            vector<int> pos(N, -1);
            for (int i = 0; i < (int)path.size(); ++i) pos[path[i]] = i;
            double best = std::numeric_limits<double>::infinity();
            int bc = -1, bk = -1, bi = -1;
            for (int ci = 0; ci < (int)cyc.size(); ++ci) {
                const auto& cy = cyc[ci];
                const int m = (int)cy.size();
                for (int k = 0; k < m; ++k) {        // cut edge cy[k] -> cy[k+1]: chain cy[k+1] .. cy[k]
                    const int u = cy[k], v = cy[(k + 1) % m];
                    for (int x : spred[v]) {
                        const int i = pos[x];
                        if (i < 0 || banned(x, v)) continue;
                        const int y = (i + 1 < (int)path.size()) ? path[i + 1] : -1;
                        if (!edge(u, y)) continue;
                        const double d = c(x, v) + c(u, y) - c(x, y) - c(u, v);
                        if (d < best) { best = d; bc = ci; bk = k; bi = i; }
                    }
                }
            }
            if (bc < 0) break;
            const auto cy = cyc[bc];
            const int m = (int)cy.size();
            vector<int> chain;
            for (int j = 1; j <= m; ++j) chain.push_back(cy[(bk + j) % m]);
            path.insert(path.begin() + bi + 1, chain.begin(), chain.end());
            res.cost += best;
            cyc.erase(cyc.begin() + bc);
            ++res.spliced;
        }
        if (!cyc.empty()) { res.failure = std::to_string(cyc.size()) + " cycle(s) could not be spliced in"; return false; }
        res.order = path;
        return true;
    }

private:
    bool banned(int s, int t) const {
        return forbidden_ && forbidden_->count(((uint64_t)(unsigned)s << 32) | (unsigned)t) > 0;
    }
    uint64_t ekey(int s, int t) const { return (uint64_t)s * (uint64_t)(N_ + 1) + (uint64_t)t; }
    double c(int s, int t) const {   // t == -1: END
        if (t < 0) return neutral_;
        auto it = cost_.find(ekey(s, t));
        return it == cost_.end() ? std::numeric_limits<double>::infinity() : it->second;
    }
    bool mayEnd(int s) const { return fragSuper_ < 0 || s == fragSuper_; }
    vector<int> contexts(int s, int maxCtx) const {
        const auto& ch = sn_[s].chunks;
        vector<int> ctx;
        if (ch.size() >= 2) ctx.push_back(ch[ch.size() - 2]);
        else if (s == startSuper_) ctx.push_back(-1);
        else {
            const auto& pr = preds_[ch.back()];
            if ((int)pr.size() <= maxCtx) ctx = pr;
            else for (int k = 0; k < maxCtx; ++k) ctx.push_back(pr[(size_t)k * pr.size() / maxCtx]);
            if (ctx.empty()) ctx.push_back(-1);
        }
        return ctx;
    }

    const vector<Supernode>& sn_;
    const vector<vector<int>>& superAdj_;
    const int startSuper_;
    const vector<ChunkMeta>& metas_;
    const std::unordered_set<uint64_t>* forbidden_;
    const int N_;
    vector<vector<int>> preds_;
    vector<vector<int>> ctxOf_;
    int fragSuper_ = -1;
    std::unordered_map<uint64_t, double> cost_;
    double neutral_ = 1.0;
};

} // namespace

MatchResult globalMatch(const vector<vector<uint8_t>>& chunks, const vector<Supernode>& sn,
                        const vector<vector<int>>& superAdj, const int startSuper,
                        const vector<vector<int>>& chunkAdj, const vector<ChunkMeta>& metas,
                        EdgeLock::FluxMemo& memo, const int threads, const EdgeLock::WarmChains* warm,
                        const long maxDecodes, const std::unordered_set<uint64_t>* forbidden) {
    MatchResult res;
    Matcher m(sn, superAdj, startSuper, chunkAdj, metas, forbidden);
    vector<std::array<int, 3>> windows;
    if (!m.chooseContexts(memo, maxDecodes, windows, res)) return res;
    res.decodes = EdgeLock::scoreWindows(chunks, metas, windows, memo, threads, warm);
    m.edgeCosts(memo);
    if (!m.assignSuccessors(res)) return res;
    m.spliceCycles(res);
    return res;
}
