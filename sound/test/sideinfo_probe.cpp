// sideinfo_probe <fixture.mp3> [num_random_pairs]
//
// Step 1 of PLAN_sideinfo_mainbits.md: measure whether Layer III *side-info* fields (no
// Huffman / main-data decode) discriminate a chunk's true successor from other chunks, to
// decide whether the full spectral decode in Milestone 2 is worth building.
//
// Runs on the TRUE (unshuffled) chunk order - `buildAdjacency`/`canFollow` are content-based
// and shuffle-invariant, so the true successor of chunk i is always chunk i+1 and the
// candidate graph `adj` is identical to what `main.cpp` sees after shuffling (CLAUDE.md item 7).
//
// Two measurements:
//   A. Per-feature separability: AUC (Mann-Whitney) between the junction-delta distribution
//      for true consecutive pairs vs. random non-consecutive pairs. 0.5 = no signal, 1.0 =
//      true junctions always have the smaller delta.
//   B. Ranking power: for each chunk i whose true successor i+1 is among its structural
//      candidates adj[i] (degree >= 2), rank i+1 among adj[i] by a continuity score. Compared
//      against the random-order baseline mean rank (degree+1)/2. This is the metric that
//      predicts DFS branching reduction if `dfs()` sorts candidates by the score.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "frame_scanner.hpp"
#include "chunk_meta.hpp"
#include "adjacency.hpp"

using namespace std;

static constexpr int CHUNK_SIZE = 1024;

// ---- feature vector extracted from a single granule/channel -------------------------------

struct Feat {
    double globalGain;
    double bigValues;
    double part23Length;
    double scalefacCompress;
    double region0Count;
    double region1Count;
    double blockType;        // categorical (compared as equal / not-equal)
    double windowSwitching;  // categorical
    double count1TableSel;   // categorical
    double tableSel0;        // categorical
    double bitrate = 0;      // frame-header kbps (not side info) - filled in from ChunkMeta::frames
};

static Feat toFeat(const GranuleSideInfo& g) {
    return Feat{
        double(g.globalGain), double(g.bigValues), double(g.part23Length),
        double(g.scalefacCompress), double(g.region0Count), double(g.region1Count),
        double(g.blockType), double(g.windowSwitching),
        double(g.count1TableSelect), double(g.tableSelect[0]), 0.0,
    };
}

struct NamedFeature {
    const char* name;
    bool categorical;
    double Feat::* member;
};

static const NamedFeature kFeatures[] = {
    {"globalGain",       false, &Feat::globalGain},
    {"bigValues",        false, &Feat::bigValues},
    {"part23Length",     false, &Feat::part23Length},
    {"scalefacCompress", false, &Feat::scalefacCompress},
    {"region0Count",     false, &Feat::region0Count},
    {"region1Count",     false, &Feat::region1Count},
    {"blockType",        true,  &Feat::blockType},
    {"windowSwitching",  true,  &Feat::windowSwitching},
    {"count1TableSel",   true,  &Feat::count1TableSel},
    {"tableSel0",        true,  &Feat::tableSel0},
    {"bitrate",          false, &Feat::bitrate},
};
static constexpr int kNumFeatures = sizeof(kFeatures) / sizeof(kFeatures[0]);

static double featDelta(const NamedFeature& f, const Feat& a, const Feat& b) {
    const double d = a.*(f.member) - b.*(f.member);
    if (f.categorical) return d == 0.0 ? 0.0 : 1.0;
    return fabs(d);
}

// AUC that `trueD` < `randD` (ties 0.5), i.e. P(a true junction's delta is smaller than a
// random pair's). > 0.5 means true junctions are more "continuous" in this feature.
static double auc(vector<double> trueD, vector<double> randD) {
    if (trueD.empty() || randD.empty()) return 0.5;
    sort(randD.begin(), randD.end());
    const double n = double(randD.size());
    double acc = 0.0;
    for (double t : trueD) {
        const auto lo = lower_bound(randD.begin(), randD.end(), t);
        const auto hi = upper_bound(randD.begin(), randD.end(), t);
        const double less = double(lo - randD.begin());        // randD strictly greater than t
        const double eq   = double(hi - lo);
        acc += ( (n - (less + eq)) + 0.5 * eq ) / n;           // P(randD > t) + 0.5 P(==)
    }
    return acc / double(trueD.size());
}

// ---- per-chunk boundary side info -------------------------------------------------------

struct ChunkSI {
    bool ok = false;
    Feat first;   // granule 0 of the chunk's first complete frame
    Feat last;    // last granule of the chunk's last complete frame
};

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <fixture.mp3> [num_random_pairs]\n", argv[0]); return 2; }
    const string filename = argv[1];
    const int numRandomPairs = argc > 2 ? atoi(argv[2]) : 200000;

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
    if (!profile.isLayerIII()) { fprintf(stderr, "not Layer III - probe is Layer III only\n"); return 1; }
    const bool mpeg1 = profile.versionID == 0b11;
    const bool mono  = profile.isMono();
    printf("fixture=%s chunks=%d mpeg1=%d mono=%d sampleRate=%d\n",
           filename.c_str(), n, int(mpeg1), int(mono), profile.sampleRate);

    vector<ChunkMeta> metas;
    metas.reserve(n);
    for (int i = 0; i < n; ++i) metas.push_back(computeChunkMeta(i, chunks[i], profile));

    vector<ChunkSI> si(n);
    for (int i = 0; i < n; ++i) {
        if (!metas[i].valid || metas[i].frames.empty()) continue;
        const FrameSideInfo f0 = parseFrameSideInfoAt(chunks[i], metas[i].frames.front().offset, mpeg1, mono);
        const FrameSideInfo f1 = parseFrameSideInfoAt(chunks[i], metas[i].frames.back().offset,  mpeg1, mono);
        if (!f0.valid || !f1.valid) continue;
        si[i].ok = true;
        si[i].first = toFeat(f0.gr[0][0]);
        si[i].last  = toFeat(f1.gr[f1.numGranules - 1][0]);
        si[i].first.bitrate = metas[i].frames.front().bitrate;
        si[i].last.bitrate  = metas[i].frames.back().bitrate;
    }
    int okChunks = 0;
    for (int i = 0; i < n; ++i) if (si[i].ok) ++okChunks;
    printf("chunks with usable boundary side info: %d/%d\n", okChunks, n);

    // ---- A3a probe: how many frameStarts a bitrate filter in StreamProfile::matches would
    // reject as false positives (headers that decode as the profile stream but at a different
    // bitrate). For CBR this is the whole point - see CLAUDE.md TODO "Bitrate as a
    // discriminator". Also reports the bitrate distribution over all real complete frames.
    {
        int brHist[16] = {0};                 // by bitrate index is awkward; bucket by kbps/8 -> coarse
        int total = 0, nonModal = 0;
        // modal bitrate over complete frames
        int cnt[512] = {0}, modal = 0, modalKbps = 0;
        for (int i = 0; i < n; ++i)
            for (const auto& fr : metas[i].frames)
                if (fr.bitrate > 0 && fr.bitrate < 512) { if (++cnt[fr.bitrate] > modal) { modal = cnt[fr.bitrate]; modalKbps = fr.bitrate; } }
        for (int i = 0; i < n; ++i) {
            if (!metas[i].valid) continue;
            for (int off : metas[i].frameStarts) {
                if (off + 4 > (int)chunks[i].size()) continue;
                bool ok = false;
                Header h(packHeaderBytes(&chunks[i][off]), ok);
                if (!ok) continue;
                const int br = h.getBitrate();
                ++total;
                if (br != modalKbps) ++nonModal;
                (void)brHist;
            }
        }
        printf("frameStarts: %d total, modal bitrate %d kbps, %d (%.1f%%) at a different bitrate "
               "(a matches() bitrate filter would drop these)\n",
               total, modalKbps, nonModal, total ? 100.0 * nonModal / total : 0.0);
    }

    const auto adj = buildAdjacency(metas);
    const auto outdeg = computeOutDegree(adj, n);
    {
        int hist[8] = {0};
        for (int i = 0; i < n; ++i) hist[min(outdeg[i], 7)]++;
        printf("out-degree histogram: {0:%d 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7+:%d}\n",
               hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7]);
    }

    // ---- Measurement A -------------------------------------------------------------------
    mt19937 rng(12345);
    uniform_int_distribution<int> pick(0, n - 1);

    vector<vector<double>> trueD(kNumFeatures), randD(kNumFeatures);
    int truePairs = 0;
    for (int i = 0; i + 1 < n; ++i) {
        if (!si[i].ok || !si[i + 1].ok) continue;
        ++truePairs;
        for (int k = 0; k < kNumFeatures; ++k)
            trueD[k].push_back(featDelta(kFeatures[k], si[i].last, si[i + 1].first));
    }
    for (int r = 0; r < numRandomPairs; ++r) {
        const int a = pick(rng), b = pick(rng);
        if (a == b || b == a + 1 || !si[a].ok || !si[b].ok) continue;
        for (int k = 0; k < kNumFeatures; ++k)
            randD[k].push_back(featDelta(kFeatures[k], si[a].last, si[b].first));
    }

    printf("\n== Measurement A: junction-delta separability ==\n");
    printf("true consecutive pairs=%d   random pairs sampled=%zu\n", truePairs, randD[0].size());
    printf("%-18s %10s %10s %8s\n", "feature", "med(true)", "med(rand)", "AUC");
    auto median = [](vector<double> v) {
        if (v.empty()) return 0.0;
        nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
        return v[v.size() / 2];
    };
    for (int k = 0; k < kNumFeatures; ++k) {
        printf("%-18s %10.2f %10.2f %8.3f\n", kFeatures[k].name,
               median(trueD[k]), median(randD[k]), auc(trueD[k], randD[k]));
    }

    // ---- Measurement B ----------------------------------------------------------------
    // Per-random-pair mean/std for each feature delta, to z-score into a combined score.
    double mean[kNumFeatures] = {0}, sd[kNumFeatures] = {0};
    for (int k = 0; k < kNumFeatures; ++k) {
        double s = 0, s2 = 0;
        for (double d : randD[k]) { s += d; s2 += d * d; }
        const double m = s / randD[k].size();
        mean[k] = m;
        sd[k]   = sqrt(max(1e-9, s2 / randD[k].size() - m * m));
    }
    // Combined score = sum of z-scored deltas over the features that Measurement A tends to
    // favour (loudness + spectral occupancy + bit budget). Kept simple/unweighted on purpose.
    const int combo[] = {0, 1, 2}; // globalGain, bigValues, part23Length
    auto scoreCombined = [&](int i, int j) {
        double s = 0;
        for (int c : combo)
            s += featDelta(kFeatures[c], si[i].last, si[j].first) / sd[c];
        return s;
    };

    struct RankStat { long cnt = 0, rank1 = 0, rank2 = 0; double rankSum = 0, baseSum = 0;
                      long cnt3 = 0, rank1_3 = 0; };
    auto evalScorer = [&](const char* label, const function<double(int,int)>& score) {
        RankStat st;
        for (int i = 0; i + 1 < n; ++i) {
            if (!si[i].ok) continue;
            const auto& cand = adj[i];
            if (cand.size() < 2) continue;
            if (find(cand.begin(), cand.end(), i + 1) == cand.end()) continue;
            bool allOk = true;
            for (int j : cand) if (!si[j].ok) { allOk = false; break; }
            if (!allOk) continue;

            vector<pair<double,int>> ranked;
            ranked.reserve(cand.size());
            for (int j : cand) ranked.push_back({score(i, j), j});
            sort(ranked.begin(), ranked.end(),
                 [](const pair<double,int>& x, const pair<double,int>& y){ return x.first < y.first; });
            int rank = 1;
            for (auto& pr : ranked) { if (pr.second == i + 1) break; ++rank; }

            st.cnt++;
            st.rankSum += rank;
            st.baseSum += (cand.size() + 1) / 2.0;
            if (rank == 1) st.rank1++;
            if (rank <= 2) st.rank2++;
            if (cand.size() >= 3) { st.cnt3++; if (rank == 1) st.rank1_3++; }
        }
        if (st.cnt == 0) { printf("%-20s (no eligible chunks)\n", label); return; }
        printf("%-20s N=%4ld  meanRank=%.2f (base %.2f)  rank1=%4.1f%%  rank<=2=%4.1f%%   [deg>=3: N=%ld rank1=%.1f%%]\n",
               label, st.cnt, st.rankSum / st.cnt, st.baseSum / st.cnt,
               100.0 * st.rank1 / st.cnt, 100.0 * st.rank2 / st.cnt,
               st.cnt3, st.cnt3 ? 100.0 * st.rank1_3 / st.cnt3 : 0.0);
    };

    printf("\n== Measurement B: rank of true successor among structural candidates ==\n");
    for (int k = 0; k < kNumFeatures; ++k) {
        const NamedFeature& f = kFeatures[k];
        evalScorer(f.name, [&, f](int i, int j){ return featDelta(f, si[i].last, si[j].first); });
    }
    evalScorer("COMBINED(z:gg+bv+p23)", scoreCombined);

    return 0;
}
