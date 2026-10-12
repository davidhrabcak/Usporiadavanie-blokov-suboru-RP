// pcm_probe <fixture.mp3> [maxJunctions]
//
// PLAN_full_pcm_decode.md steps 1-2. Runs on the TRUE (unshuffled) chunk order - adjacency
// is content-based and shuffle-invariant, so chunk i's true successor is i+1 and adj[i] is
// exactly the candidate set the DFS sees.
//
//   Gate 1 (correctness): decode the window [i-1, i, i+1] with a fresh decoder and compare
//     every frame from chunk i onward against a full-file decode, sample for sample.
//   Gate 2 (signal): for every chunk i with >= 2 structural successors (true one included),
//     score each candidate c with the PCM junction metrics on [i-1, i, c] and report AUC
//     (true vs WRONG STRUCTURAL candidates - never random pairs), rank-1 and mean rank vs
//     the (deg+1)/2 random baseline. Milestone 2's band-energy L2 on the same candidate sets
//     is the baseline to beat.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "frame_scanner.hpp"
#include "chunk_meta.hpp"
#include "adjacency.hpp"
#include "main_data_decoder.hpp"
#include "pcm_junction.hpp"
#include "edge_lock.hpp"

using namespace std;

static int CHUNK_SIZE = 1024; // RP_CHUNK_SIZE overrides (as in rp)

static double auc(vector<double> trueD, vector<double> wrongD) {
    if (trueD.empty() || wrongD.empty()) return 0.5;
    sort(wrongD.begin(), wrongD.end());
    const double n = double(wrongD.size());
    double acc = 0.0;
    for (double t : trueD) {
        const auto lo = lower_bound(wrongD.begin(), wrongD.end(), t);
        const auto hi = upper_bound(wrongD.begin(), wrongD.end(), t);
        const double eq = double(hi - lo);
        acc += ((n - double(lo - wrongD.begin()) - eq) + 0.5 * eq) / n;
    }
    return acc / double(trueD.size());
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <fixture.mp3> [maxJunctions]\n", argv[0]); return 2; }
    const string filename = argv[1];
    const int maxJ = argc > 2 ? atoi(argv[2]) : 1 << 30;
    // Number of warm-up chunks before chunk i (PROBE_CTX, default 2: i-2 and i-1).
    const int ctx = getenv("PROBE_CTX") ? max(1, atoi(getenv("PROBE_CTX"))) : 2;
    if (const char* cs = getenv("RP_CHUNK_SIZE")) CHUNK_SIZE = max(1024, atoi(cs));

    Mp3FrameScanner scanner(filename);
    if (scanner.getFrameCount() == 0) { fprintf(stderr, "No frames found\n"); return 1; }
    ifstream file(filename, ios::binary);
    file.seekg(static_cast<long>(scanner.getFrames()[0].position), ios::beg);
    vector<vector<uint8_t>> chunks;
    {
        vector<uint8_t> buf(CHUNK_SIZE);
        while (file.read(reinterpret_cast<char*>(buf.data()), CHUNK_SIZE) || file.gcount() > 0)
            chunks.emplace_back(buf.begin(), buf.begin() + static_cast<long>(file.gcount()));
    }
    const int n = static_cast<int>(chunks.size());

    StreamProfile profile{};
    if (!deriveProfile(chunks[0], profile)) { fprintf(stderr, "no profile\n"); return 1; }
    const bool mpeg1 = profile.versionID == 0b11;
    printf("fixture=%s chunks=%d mpeg1=%d mono=%d sampleRate=%d\n",
           filename.c_str(), n, int(mpeg1), int(profile.isMono()), profile.sampleRate);

    vector<ChunkMeta> metas;
    for (int i = 0; i < n; ++i) metas.push_back(computeChunkMeta(i, chunks[i], profile, chunks[i].size() < chunks[0].size()));
    const auto adj = buildAdjacency(metas);

    // ---- Gate 1: windowed decode == full decode ---------------------------------------
    {
        vector<const vector<uint8_t>*> all;
        for (auto& c : chunks) all.push_back(&c);
        const PcmJunction::Window full = PcmJunction::decodeWindow(all, n, 0);
        map<long, int> fullByByte; // stream byte -> frame index in `full`
        for (int k = 0; k < (int)full.frames.size(); ++k) fullByByte[full.frames[k].byteStart] = k;
        printf("\n== Gate 1: windowed [i-%d..i,i+1] decode vs full-file decode ==\n", ctx);
        printf("  full decode: %zu frames, %zu samples/ch, channels=%d\n",
               full.frames.size(), full.mono.size(), full.channels);

        long cmpFrames = 0, exactFrames = 0, noOutput = 0;
        long cmpCur = 0, exactCur = 0;   // frames that start inside chunk i (the "pre" side)
        double maxDiff = 0;
        for (int i = 1; i + 1 < n; ++i) {
            const int first = max(0, i - ctx);
            if (!metas[first].valid) continue;
            vector<const vector<uint8_t>*> segs;
            for (int k = first; k <= i + 1; ++k) segs.push_back(&chunks[k]);
            const int curSeg = i - first;
            const PcmJunction::Window w = PcmJunction::decodeWindow(segs, curSeg + 1, metas[first].headOffset);
            const long base = (long)first * CHUNK_SIZE;
            for (const auto& fr : w.frames) {
                if (fr.byteStart < (long)curSeg * CHUNK_SIZE) continue; // warm-up chunks
                auto it = fullByByte.find(base + fr.byteStart);
                if (it == fullByByte.end()) continue;
                const auto& ff = full.frames[it->second];
                ++cmpFrames;
                const bool inCur = fr.byteStart < (long)(curSeg + 1) * CHUNK_SIZE;
                if (inCur) ++cmpCur;
                if (fr.samples == 0 || ff.samples == 0) { ++noOutput; continue; }
                const int ch = w.channels;
                double d = 0;
                for (int s = 0; s < fr.samples * ch; ++s)
                    d = max(d, (double)fabs(w.interleaved[(size_t)fr.sampleStart * ch + s] -
                                            full.interleaved[(size_t)ff.sampleStart * ch + s]));
                maxDiff = max(maxDiff, d);
                if (d == 0) { ++exactFrames; if (inCur) ++exactCur; }
            }
        }
        printf("  frames compared: %ld, sample-exact: %ld (%.2f%%), no output: %ld, max |diff| = %.3g\n",
               cmpFrames, exactFrames, cmpFrames ? 100.0 * exactFrames / cmpFrames : 0.0, noOutput, maxDiff);
        printf("  of which in chunk i (pre side): %ld, sample-exact: %ld (%.2f%%)\n",
               cmpCur, exactCur, cmpCur ? 100.0 * exactCur / cmpCur : 0.0);
    }

    // ---- Gate 2: signal on structural candidate sets ------------------------------------
    using namespace Mp3MainData;
    auto m2tail = [&](int prev, int c) -> GranuleSpectrum {
        if (!metas[c].valid || metas[c].frames.empty()) return {};
        const FrameSpectra f = decodeChunkFrame(prev >= 0 ? &metas[prev] : nullptr,
                                                prev >= 0 ? chunks[prev] : vector<uint8_t>{},
                                                chunks[c], metas[c], metas[c].frames.back().offset,
                                                profile.sampleRate);
        return f.valid ? f.gr[mpeg1 ? 1 : 0][0] : GranuleSpectrum{};
    };
    auto m2head = [&](int prev, int c) -> GranuleSpectrum {
        if (!metas[c].valid || metas[c].frames.empty()) return {};
        const FrameSpectra f = decodeChunkFrame(&metas[prev], chunks[prev], chunks[c], metas[c],
                                                metas[c].frames.front().offset, profile.sampleRate);
        return f.valid ? f.gr[0][0] : GranuleSpectrum{};
    };

    struct MetricDef { const char* name; function<double(const PcmJunction::Metrics&)> f; };
    const vector<MetricDef> defs = {
        {"flux",     [](const PcmJunction::Metrics& m) { return m.flux; }},
        {"specDist", [](const PcmJunction::Metrics& m) { return m.specDist; }},
        {"rmsJump",  [](const PcmJunction::Metrics& m) { return m.rmsJump; }},
        {"hfBurst",  [](const PcmJunction::Metrics& m) { return m.hfBurst; }},
        {"peak",     [](const PcmJunction::Metrics& m) { return m.peak; }},
    };
    const int NM = (int)defs.size();
    const int M2 = NM, COMBO = NM + 1, TOT = NM + 2; // extra columns: Milestone-2 L2, z-sum combo

    // Collect raw per-junction candidate scores first (combo needs global z-scoring).
    struct Cand { int c; bool truth; double v[16]; bool m2ok; int postFailed; };
    vector<vector<Cand>> junctions;
    int silentJ = 0, skippedJ = 0, considered = 0;
    long postFailTrue = 0, postFailWrong = 0, candTrue = 0, candWrong = 0;
    for (int i = 1; i + 1 < n && considered < maxJ; ++i) {
        const auto& cand = adj[i];
        if (cand.size() < 2 || find(cand.begin(), cand.end(), i + 1) == cand.end()) continue;
        const int first = max(0, i - ctx);
        if (!metas[first].valid) { ++skippedJ; continue; }
        ++considered;
        const GranuleSpectrum A = m2tail(i - 1, i);
        vector<Cand> cs;
        bool trueOk = false, silent = false;
        for (int c : cand) {
            vector<const vector<uint8_t>*> segs;
            for (int k = first; k <= i; ++k) segs.push_back(&chunks[k]);
            segs.push_back(&chunks[c]);
            const PcmJunction::Metrics m = PcmJunction::scoreJunction(segs, i - first + 1, metas[first].headOffset);
            if (!m.valid) continue;
            Cand x; x.c = c; x.truth = (c == i + 1); x.postFailed = m.postFailed;
            for (int k = 0; k < NM; ++k) x.v[k] = defs[k].f(m);
            const GranuleSpectrum B = m2head(i, c);
            x.m2ok = A.valid && B.valid;
            double s = 0;
            if (x.m2ok) for (int q = 0; q < NBANDS; ++q) { const double d = A.bandEnergy[q] - B.bandEnergy[q]; s += d * d; }
            x.v[M2] = sqrt(s);
            if (x.truth) { trueOk = true; silent = m.silent; ++candTrue; postFailTrue += m.postFailed; }
            else { ++candWrong; postFailWrong += m.postFailed; }
            cs.push_back(x);
        }
        if (!trueOk || cs.size() < 2) { ++skippedJ; continue; }
        if (silent) { ++silentJ; continue; }
        junctions.push_back(std::move(cs));
    }

    // z-score each PCM metric over all candidates (log1p first: they're heavy-tailed), sum.
    {
        vector<double> mu(NM, 0), sd(NM, 0); long cnt = 0;
        for (auto& cs : junctions) for (auto& x : cs) { ++cnt; for (int k = 0; k < NM; ++k) mu[k] += log1p(x.v[k]); }
        for (int k = 0; k < NM; ++k) mu[k] /= max(1L, cnt);
        for (auto& cs : junctions) for (auto& x : cs) for (int k = 0; k < NM; ++k) sd[k] += pow(log1p(x.v[k]) - mu[k], 2);
        for (int k = 0; k < NM; ++k) sd[k] = sqrt(sd[k] / max(1L, cnt)) + 1e-12;
        for (auto& cs : junctions) for (auto& x : cs) {
            double z = 0;
            for (int k = 0; k < NM; ++k) z += (log1p(x.v[k]) - mu[k]) / sd[k];
            x.v[COMBO] = z;
        }
    }

    printf("\n== Gate 2: rank of true successor among structural candidates ==\n");
    printf("  junctions with deg>=2: %d considered, %zu scored, %d silent (excluded), %d skipped\n",
           considered, junctions.size(), silentJ, skippedJ);
    printf("  frames with no output on the post side: true %.3f/junction, wrong %.3f/candidate\n",
           candTrue ? double(postFailTrue) / candTrue : 0.0, candWrong ? double(postFailWrong) / candWrong : 0.0);
    printf("  %-10s %7s %8s %9s %9s %8s\n", "metric", "AUC", "rank-1", "meanRank", "baseline", "medTrue/medWrong");
    for (int k = 0; k < TOT; ++k) {
        vector<double> tv, wv;
        long cnt = 0, r1 = 0; double rankSum = 0, baseSum = 0;
        for (auto& cs : junctions) {
            if (k == M2) {
                bool ok = true;
                for (auto& x : cs) if (x.truth && !x.m2ok) ok = false;
                if (!ok) continue;
            }
            double tval = 0; vector<double> others;
            for (auto& x : cs) {
                if (k == M2 && !x.m2ok) continue;
                if (x.truth) tval = x.v[k]; else others.push_back(x.v[k]);
            }
            if (others.empty()) continue;
            tv.push_back(tval);
            wv.insert(wv.end(), others.begin(), others.end());
            int less = 0, eq = 0;
            for (double o : others) { if (o < tval) ++less; else if (o == tval) ++eq; }
            const double rank = 1 + less + 0.5 * eq;
            ++cnt; rankSum += rank; baseSum += (others.size() + 2) / 2.0;
            if (less == 0 && eq == 0) ++r1;
        }
        auto med = [](vector<double> v) { if (v.empty()) return 0.0; nth_element(v.begin(), v.begin() + v.size() / 2, v.end()); return v[v.size() / 2]; };
        const char* name = k < NM ? defs[k].name : (k == M2 ? "M2-L2" : "pcmCombo");
        printf("  %-10s %7.3f %7.1f%% %9.2f %9.2f   %.3g / %.3g   (n=%ld)\n", name, auc(tv, wv),
               cnt ? 100.0 * r1 / cnt : 0.0, cnt ? rankSum / cnt : 0.0, cnt ? baseSum / cnt : 0.0,
               med(tv), med(wv), cnt);
    }

    // DFS-relevant, like-for-like comparison: only candidates whose post side decoded fully
    // (postFailed == 0 - a reservoir underflow there is what the DFS reservoir check already
    // rejects, and its zero-filled gap hands flux a free spike) AND that Milestone 2 could
    // score; only junctions where the true successor qualifies and >= 1 wrong one remains.
    // flux and M2 are ranked on exactly the same candidate sets.
    {
        vector<double> tf, wf, tm, wm;
        long cnt = 0, r1f = 0, r1m = 0; double rf = 0, rm = 0, base = 0;
        for (auto& cs : junctions) {
            vector<const Cand*> keep; const Cand* tr = nullptr;
            for (auto& x : cs) if (x.postFailed == 0 && x.m2ok) { keep.push_back(&x); if (x.truth) tr = &x; }
            if (!tr || keep.size() < 2) continue;
            ++cnt; base += (keep.size() + 1) / 2.0;
            for (int col : {0, M2}) {
                int less = 0, eq = 0;
                for (auto* x : keep) if (x != tr) {
                    if (x->v[col] < tr->v[col]) ++less; else if (x->v[col] == tr->v[col]) ++eq;
                    (col == 0 ? wf : wm).push_back(x->v[col]);
                }
                (col == 0 ? tf : tm).push_back(tr->v[col]);
                const double rank = 1 + less + 0.5 * eq;
                if (col == 0) { rf += rank; if (!less && !eq) ++r1f; } else { rm += rank; if (!less && !eq) ++r1m; }
            }
        }
        printf("\n  like-for-like (reservoir-decodable + M2-scorable candidates, same sets): %ld junctions, "
               "random mean rank %.2f\n", cnt, cnt ? base / cnt : 0.0);
        printf("    flux   AUC %.3f  rank-1 %5.1f%%  meanRank %.2f\n", auc(tf, wf), cnt ? 100.0 * r1f / cnt : 0.0, cnt ? rf / cnt : 0.0);
        printf("    M2-L2  AUC %.3f  rank-1 %5.1f%%  meanRank %.2f\n", auc(tm, wm), cnt ? 100.0 * r1m / cnt : 0.0, cnt ? rm / cnt : 0.0);
    }

    // ---- Objective alignment (2-opt swap test): for true edges a->b and c->d, where the
    // crossed edges a->d and c->b are also structural, does the true pair have the lower
    // summed flux? (Same true-context flux as above.) If the true order isn't (near) the
    // argmin of summed flux, global objective searches (matching/beam) are a dead end.
    {
        map<pair<int,int>, double> fx; // (i, cand) -> flux, true context
        int ji = 0;
        for (int i = 1; i + 1 < n; ++i) (void)i;
        for (auto& cs : junctions) {
            int i = -1;
            for (auto& x : cs) if (x.truth) i = x.c - 1;
            for (auto& x : cs) fx[{i, x.c}] = x.v[0];
            ++ji;
        }
        long swaps = 0, trueWins = 0, ties = 0;
        for (auto& kv : fx) {
            const int a = kv.first.first, d = kv.first.second;
            if (d == a + 1) continue;
            const int b = a + 1, c = d - 1;
            if (c < 0 || c == a) continue;
            auto ab = fx.find({a, b}), cd = fx.find({c, d}), cb = fx.find({c, b});
            if (ab == fx.end() || cd == fx.end() || cb == fx.end()) continue;
            const double tr = ab->second + cd->second, sw = kv.second + cb->second;
            ++swaps;
            if (tr < sw) ++trueWins; else if (tr == sw) ++ties;
        }
        printf("\n  2-opt swap test: %ld structural swaps, true pair cheaper in %ld (%.1f%%), ties %ld\n",
               swaps, trueWins, swaps ? 100.0 * trueWins / swaps : 0.0, ties);
    }

    // ---- Confident-edge locking (edge_lock, exactly what rp would apply). Precision is the
    // gate: a wrong lock makes the true order unreachable.
    {
        int maxCtx = getenv("PROBE_MAXCTX") ? atoi(getenv("PROBE_MAXCTX")) : 8;
        vector<vector<int>> preds(n);
        for (int a = 0; a < n; ++a) for (int b : adj[a]) preds[b].push_back(a);
        long branchTrue = 0, branchOut = 0;
        for (int i = 0; i + 1 < n; ++i) {
            if (find(adj[i].begin(), adj[i].end(), i + 1) == adj[i].end()) continue;
            if (adj[i].size() >= 2 || preds[i + 1].size() >= 2) ++branchTrue;
        }
        for (int i = 0; i < n; ++i) if (adj[i].size() >= 2) ++branchOut;
        for (auto mode : {EdgeLock::Mode::Robust, EdgeLock::Mode::Median}) {
            long dec = 0;
            const auto all = EdgeLock::findConfidentEdges(chunks, metas, adj, 0, 1.0, maxCtx, mode, &dec);
            printf("\n== Confident-edge locking, %s mode (maxCtx=%d, %ld window decodes) ==\n",
                   mode == EdgeLock::Mode::Robust ? "robust" : "median", maxCtx, dec);
            printf("  true edges at a branch point (out- or in-degree >= 2): %ld; chunks with out-degree >= 2: %ld\n",
                   branchTrue, branchOut);
            printf("  %6s %7s %8s %9s %10s %16s\n", "tau", "locks", "wrong", "precision", "coverage", "outdeg>=2 after");
            for (double tau : {1.0, 1.1, 1.25, 1.5, 2.0, 3.0, 4.0}) {
                vector<EdgeLock::LockedEdge> L;
                for (auto& e : all) if (e.margin >= tau) L.push_back(e);
                long wrong = 0;
                for (auto& e : L) if (e.b != e.a + 1) ++wrong;
                auto adj2 = adj;
                EdgeLock::applyLocks(adj2, L);
                long after = 0;
                for (int i = 0; i < n; ++i) if (adj2[i].size() >= 2) ++after;
                printf("  %6.2f %7zu %8ld %8.2f%% %9.1f%% %16ld\n", tau, L.size(), wrong,
                       L.empty() ? 100.0 : 100.0 * (L.size() - wrong) / L.size(),
                       branchTrue ? 100.0 * (L.size() - wrong) / branchTrue : 0.0, after);
                if (wrong && getenv("PROBE_DBG"))
                    for (auto& e : L) if (e.b != e.a + 1) printf("      wrong lock %d->%d margin %.2f\n", e.a, e.b, e.margin);
            }
        }
    }

    // Could flux be a hard filter? Distribution of the TRUE junctions' flux vs wrong candidates.
    {
        vector<double> tv, wv;
        for (auto& cs : junctions) for (auto& x : cs) (x.truth ? tv : wv).push_back(x.v[0]);
        sort(tv.begin(), tv.end()); sort(wv.begin(), wv.end());
        auto q = [](const vector<double>& v, double p) { return v.empty() ? 0.0 : v[min(v.size() - 1, (size_t)(p * v.size()))]; };
        printf("\n  flux quantiles  true: p50=%.2f p90=%.2f p99=%.2f max=%.2f | wrong: p10=%.2f p50=%.2f\n",
               q(tv, .5), q(tv, .9), q(tv, .99), tv.empty() ? 0 : tv.back(), q(wv, .1), q(wv, .5));
        for (double thr : {2.0, 3.0, 4.0, 6.0}) {
            long tk = 0, wk = 0;
            for (double t : tv) if (t > thr) ++tk;
            for (double w : wv) if (w > thr) ++wk;
            printf("  flux > %.0f: rejects %ld/%zu true junctions, %ld/%zu (%.1f%%) wrong candidates\n",
                   thr, tk, tv.size(), wk, wv.size(), wv.empty() ? 0.0 : 100.0 * wk / wv.size());
        }
    }
    return 0;
}
