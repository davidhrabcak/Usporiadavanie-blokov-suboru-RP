#include "scoring/continuity.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include "mp3/reservoir.hpp"

using namespace std;

static Continuity::Class classOf(const int slack) {
    return slack == 0 ? Continuity::Tight : slack == -2 ? Continuity::AtCap
         : slack == -3 ? Continuity::Unknown : Continuity::Hole;
}

Continuity::Continuity(const vector<vector<uint8_t>>& chunks, const vector<ChunkMeta>& metas,
                       const vector<vector<int>>& adj, const StreamProfile& profile, const string& mode,
                       const vector<int>& origIndexOf) {
    if (!profile.isLayerIII()) return;
    const int side = sideInfoSize(profile.versionID, profile.isMono());
    const int maxR = maxReservoirBytes(profile.versionID);

    // the encoder's cap: largest main_data_begin of any complete frame
    int capEff = 0;
    for (const auto& m : metas) for (const auto& f : m.frames) capEff = std::max<int>(capEff, f.mainDataBegin);
    if (capEff == 0) capEff = maxR;

    // in-chunk consecutive frame pairs: true by construction
    auto inChunk = [&](const bool fullRule, long cnt[4], long& atCap, long& atCapExact) {
        cnt[0] = cnt[1] = cnt[2] = cnt[3] = 0; atCap = atCapExact = 0;
        for (const auto& m : metas)
            for (size_t i = 0; i + 1 < m.frames.size(); ++i) {
                int level = maxR * 8;
                if (!advanceReservoirFrame(level, m.frames[i], side, maxR)) continue;
                if (level / 8 >= capEff) { ++atCap; if (m.frames[i + 1].mainDataBegin == capEff) ++atCapExact; }
                ++cnt[classOf(frameSlack(level, m.frames[i + 1], side, maxR, capEff, fullRule))];
            }
    };
    long cnt[4], atCap, atCapExact;
    inChunk(false, cnt, atCap, atCapExact);
    const bool fullRule = atCap >= 20 && atCapExact >= 0.95 * atCap;
    if (fullRule) inChunk(true, cnt, atCap, atCapExact);

    long arcs[4] = {0, 0, 0, 0}, trueArcs[4] = {0, 0, 0, 0};
    for (size_t a = 0; a < adj.size(); ++a)
        for (const int b : adj[a]) {
            const Class c = classOf(junctionSlack(metas[a], metas[b], chunks[b], side, maxR, capEff, fullRule));
            cls_[(static_cast<uint64_t>(a) << 32) | static_cast<uint32_t>(b)] = c;
            ++arcs[c];
            if (origIndexOf[b] == origIndexOf[a] + 1) ++trueArcs[c];   // diagnostic only
        }

    if (mode == "fixed") {
        cost_[Tight] = 0; cost_[AtCap] = 0.5; cost_[Hole] = 2.0; cost_[Unknown] = 0.5;
    } else {
        constexpr double K = 0.5, MAX_COST = 4.0;
        const double nt = cnt[0] + cnt[1] + cnt[2], na = arcs[0] + arcs[1] + arcs[2];   // Unknown left out
        double llr[4], best = 0;   // Unknown: ratio 1
        llr[Unknown] = 0;
        for (int c = 0; c < 3; ++c) {
            llr[c] = std::log((cnt[c] + 1.0) / (nt + 3.0)) - std::log((arcs[c] + 1.0) / (na + 3.0));
            if (arcs[c] > 0) best = std::max(best, llr[c]);   // a class no arc has can't set the scale
        }
        for (int c = 0; c < 4; ++c) cost_[c] = std::clamp(K * (best - llr[c]), 0.0, MAX_COST);
    }
    cout << "Continuity (" << mode << "): cap " << capEff << (fullRule ? ", full rule ON" : ", full rule off")
         << " (" << atCapExact << "/" << atCap << " in-chunk pairs at the cap exact); tight/at cap/hole/unknown: in-chunk pairs "
         << cnt[0] << "/" << cnt[1] << "/" << cnt[2] << ", arcs " << arcs[0] << "/" << arcs[1] << "/" << arcs[2] << "/" << arcs[3]
         << " [vs source: true arcs " << trueArcs[0] << "/" << trueArcs[1] << "/" << trueArcs[2] << "/" << trueArcs[3]
         << "], cost " << fixed << setprecision(2) << cost_[0] << "/" << cost_[1] << "/" << cost_[2] << "/" << cost_[3] << "\n";
}
