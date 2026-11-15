#include "search/chain_merge.hpp"

#include <chrono>
#include <cstdlib>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include "mp3/reservoir.hpp"
#include "scoring/pcm_junction.hpp"

using namespace std;

vector<EdgeLock::LockedEdge> chainMerge(const vector<vector<uint8_t>>& chunks, const vector<ChunkMeta>& metas,
                                        const vector<vector<int>>& adj, const int start,
                                        const vector<double>& taus, const int maxLeft, const int threads,
                                        const vector<int>& origIndexOf) {
    const auto t0 = chrono::steady_clock::now();
    const int n = static_cast<int>(chunks.size());
    const float NaN = numeric_limits<float>::quiet_NaN();
    const double INF = numeric_limits<double>::infinity(), FLOOR = 0.1;

    vector<int> next(n, -1), prev(n, -1);   // the joined edges
    vector<int> headOf(n);                  // for a segment's tail: its head
    for (int i = 0; i < n; ++i) headOf[i] = i;
    vector<vector<float>> flux(n);          // flux[a][k]: a -> adj[a][k], NaN = not measurable
    vector<int> scoredCtx(n, 0);            // context length flux[a] was scored with (0 = never)
    vector<EdgeLock::LockedEdge> joined;
    long decodes = 0, correct = 0;

    auto context = [&](int a) {             // the tail segment's last chunks, oldest first
        vector<int> ctx;
        for (int c = a; c >= 0 && (int)ctx.size() < maxLeft; c = prev[c]) ctx.insert(ctx.begin(), c);
        while (ctx.size() > 1 && !metas[ctx[0]].valid) ctx.erase(ctx.begin());
        return ctx;
    };
    auto open = [&](int a, int b) { return prev[b] < 0 && b != start && headOf[a] != b; };

    // Main-data continuity (junctionSlack) of every structural edge: a tight edge has its flux
    // divided by TIGHT_BONUS. Wrong edges are tight 0.2-3% of the time, true ones whenever the
    // reservoir wasn't full.
    const double TIGHT_BONUS = 8.0;
    const int side = sideInfoSize(metas[start].profile.versionID, metas[start].profile.isMono());
    const int maxR = maxReservoirBytes(metas[start].profile.versionID);
    vector<vector<char>> isTight(n);
    for (int a = 0; a < n; ++a) {
        isTight[a].assign(adj[a].size(), 0);
        for (size_t k = 0; k < adj[a].size(); ++k)
            isTight[a][k] = junctionSlack(metas[a], metas[adj[a][k]], chunks[adj[a][k]], side, maxR) == 0;
    }
    auto eff = [&](int a, size_t k) { return (flux[a][k] + FLOOR) / (isTight[a][k] ? TIGHT_BONUS : 1.0); };
    if (getenv("RP_CHAIN_DIAG")) {
        long trueN = 0, trueTight = 0, trueUnk = 0, trueHole = 0, trueBad = 0, wrongN = 0, wrongTight = 0, wrongUnk = 0;
        vector<int> tightOut(n, 0), tightIn(n, 0), okOut(n, 0);
        vector<vector<char>> tight(n);
        for (int a = 0; a < n; ++a) {
            tight[a].assign(adj[a].size(), 0);
            for (size_t k = 0; k < adj[a].size(); ++k) {
                const int b = adj[a][k];
                const int sl = junctionSlack(metas[a], metas[b], chunks[b], side, maxR);
                const bool tr = origIndexOf[b] == origIndexOf[a] + 1;
                if (tr) { ++trueN; if (sl == 0) ++trueTight; else if (sl == -2) ++trueUnk; else if (sl > 0) ++trueHole; else ++trueBad; }
                else { ++wrongN; if (sl == 0) ++wrongTight; else if (sl == -2) ++wrongUnk; }
                if (sl == 0) { tight[a][k] = 1; ++tightOut[a]; ++tightIn[b]; }
                if (sl == 0 || sl == -2) ++okOut[a];
            }
        }
        long uniq = 0, uniqOk = 0, okSum = 0;
        for (int a = 0; a < n; ++a) {
            okSum += okOut[a];
            for (size_t k = 0; k < adj[a].size(); ++k)
                if (tight[a][k] && tightOut[a] == 1 && tightIn[adj[a][k]] == 1) {
                    ++uniq;
                    if (origIndexOf[adj[a][k]] == origIndexOf[a] + 1) ++uniqOk;
                }
        }
        cout << "  continuity diag: true edges " << trueN << ": tight " << trueTight << ", hole " << trueHole << ", untestable "
             << trueUnk << ", rejected " << trueBad << " | wrong edges " << wrongN << ": tight " << wrongTight << " ("
             << 100.0 * wrongTight / std::max(1L, wrongN) << "%), untestable " << wrongUnk << " | tight-or-untestable successors per chunk "
             << (double)okSum / n << " | mutually unique tight edges " << uniq << " [vs source: " << uniqOk << " correct]\n";
    }
    for (const double tau : taus) {
        for (int round = 1;; ++round) {
            // 1. score the open junctions of every tail whose context has grown
            vector<int> todo;
            for (int a = 0; a < n; ++a) {
                if (next[a] >= 0 || adj[a].empty() || !metas[a].valid) continue;
                if ((int)context(a).size() != scoredCtx[a]) todo.push_back(a);
            }
            vector<long> dec(todo.size(), 0);
            EdgeLock::parallelFor(todo.size(), threads, [&](const size_t i) {
                const int a = todo[i];
                const vector<int> ctx = context(a);
                vector<const vector<uint8_t>*> segs;
                for (int c : ctx) segs.push_back(&chunks[c]);
                const PcmJunction::LeftContext left(segs, metas[ctx[0]].headOffset);
                flux[a].assign(adj[a].size(), NaN);
                for (size_t k = 0; k < adj[a].size(); ++k) {
                    if (!open(a, adj[a][k])) continue;
                    const PcmJunction::Metrics m = left.score(chunks[adj[a][k]]);
                    ++dec[i];
                    if (m.valid && !m.silent) flux[a][k] = static_cast<float>(m.flux);
                }
                scoredCtx[a] = static_cast<int>(ctx.size());
            });
            for (long d : dec) decodes += d;

            if (getenv("RP_CHAIN_DIAG")) {   // rank of the true successor among each tail's open candidates
                long have = 0, r1 = 0, r3 = 0, nanTrue = 0; double rankSum = 0, degSum = 0, ratioSum = 0;
                for (int a = 0; a < n; ++a) {
                    if (next[a] >= 0 || flux[a].empty()) continue;
                    int tk = -1;
                    for (size_t k = 0; k < adj[a].size(); ++k) if (origIndexOf[adj[a][k]] == origIndexOf[a] + 1) tk = (int)k;
                    if (tk < 0 || !open(a, adj[a][tk])) continue;
                    if (std::isnan(flux[a][tk])) { ++nanTrue; continue; }
                    int less = 0, deg = 0; double best = INF;
                    for (size_t k = 0; k < adj[a].size(); ++k) {
                        if (!open(a, adj[a][k]) || std::isnan(flux[a][k])) continue;
                        ++deg;
                        if ((int)k != tk) { best = std::min(best, (double)flux[a][k]); if (flux[a][k] < flux[a][tk]) ++less; }
                    }
                    ++have; rankSum += less + 1; degSum += deg; if (!less) ++r1; if (less < 3) ++r3;
                    if (best < INF) ratioSum += std::log((best + FLOOR) / (flux[a][tk] + FLOOR));
                }
                cout << "  diag: tails with a measurable true successor " << have << " (unmeasurable " << nanTrue << "), rank-1 "
                     << 100.0 * r1 / std::max(1L, have) << "%, top-3 " << 100.0 * r3 / std::max(1L, have) << "%, mean rank "
                     << rankSum / std::max(1L, have) << " of " << degSum / std::max(1L, have)
                     << ", mean log(best wrong / true) " << ratioSum / std::max(1L, have) << "\n";
            }
            // 2. per head: the best and second-best flux over the tails that can precede it
            vector<double> in1(n, INF), in2(n, INF);
            vector<int> inBest(n, -1);
            for (int a = 0; a < n; ++a) {
                if (next[a] >= 0 || flux[a].empty()) continue;
                for (size_t k = 0; k < adj[a].size(); ++k) {
                    const int b = adj[a][k];
                    if (!open(a, b) || std::isnan(flux[a][k])) continue;
                    const double f = eff(a, k);
                    if (f < in1[b]) { in2[b] = in1[b]; in1[b] = f; inBest[b] = a; }
                    else if (f < in2[b]) in2[b] = f;
                }
            }
            // 3. good junctions: best on both sides by a factor >= tau
            vector<EdgeLock::LockedEdge> good;
            for (int a = 0; a < n; ++a) {
                if (next[a] >= 0 || flux[a].empty()) continue;
                double f1 = INF, f2 = INF;
                int b1 = -1;
                for (size_t k = 0; k < adj[a].size(); ++k) {
                    const int b = adj[a][k];
                    if (!open(a, b) || std::isnan(flux[a][k])) continue;
                    const double f = eff(a, k);
                    if (f < f1) { f2 = f1; f1 = f; b1 = b; }
                    else if (f < f2) f2 = f;
                }
                if (b1 < 0 || inBest[b1] != a) continue;
                const double margin = std::min(f2, in2[b1]) / f1;
                if (margin == INF && tau > 1.0) continue;   // the only candidate both ways: no evidence yet
                if (margin >= tau) good.push_back({a, b1, margin});
            }
            std::sort(good.begin(), good.end(),
                      [](const EdgeLock::LockedEdge& x, const EdgeLock::LockedEdge& y) { return x.margin > y.margin; });
            int applied = 0, ok = 0;
            for (const auto& e : good) {
                if (next[e.a] >= 0 || prev[e.b] >= 0 || headOf[e.a] == e.b) continue;   // would close a cycle
                int tb = e.b;
                while (next[tb] >= 0) tb = next[tb];
                next[e.a] = e.b;
                prev[e.b] = e.a;
                headOf[tb] = headOf[e.a];
                joined.push_back(e);
                ++applied;
                if (origIndexOf[e.b] == origIndexOf[e.a] + 1) ++ok;
            }
            correct += ok;
            int segments = 0;
            for (int i = 0; i < n; ++i) if (prev[i] < 0) ++segments;
            cout << "Chain merge tau " << tau << " round " << round << ": " << applied << " joined [vs source: "
                 << ok << " correct], " << segments << " segments left, " << decodes << " decodes so far\n" << flush;
            if (applied == 0) break;
        }
    }
    cout << "Chain merge total: " << joined.size() << " junctions joined [vs source: " << correct << " correct], "
         << decodes << " decodes, " << fixed << setprecision(2)
         << chrono::duration<double>(chrono::steady_clock::now() - t0).count() << "s\n";
    return joined;
}
