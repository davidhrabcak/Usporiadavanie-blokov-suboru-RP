#include "scoring/junction_scorer.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>

using namespace std;

static constexpr int BV_DIV = 32;

JunctionScorer::JunctionScorer(const vector<vector<uint8_t>>& chunks, const vector<ChunkMeta>& metas,
                               const StreamProfile& profile, const long chunkSize, const Config& cfg)
    : chunks_(chunks), metas_(metas), isLayerIII_(profile.isLayerIII()), rank_(cfg.rank),
      specScaleOverride_(cfg.specScale), pcmWeight_(cfg.pcmWeight), boundaryGain_(chunks.size()) {
    // PCM window left side: grows past 3 chunks when frames are large vs the chunk size (320 kbps
    // at 1 KB measured ~0% of junctions otherwise). Sized on the 90th-percentile frame length so
    // a few large frames in a VBR stream don't widen every window. Shuffle-invariant.
    {
        vector<int> lens;
        for (const auto& m : metas) {   // + the spilling tail: a frame > chunk is never whole in one
            for (const auto& f : m.frames) lens.push_back(f.length);
            if (m.valid && m.tailPartialLen > 0) lens.push_back(m.tailPartialLen);
        }
        int p90 = 0;
        if (!lens.empty()) {
            std::nth_element(lens.begin(), lens.begin() + lens.size() * 9 / 10, lens.end());
            p90 = lens[lens.size() * 9 / 10];
        }
        pcmLeft_ = std::max(3, PcmJunction::leftChunksNeeded(p90, chunkSize));
        if (cfg.pcmLeft) pcmLeft_ = *cfg.pcmLeft;
        pcmLeft_ = std::min(std::max(pcmLeft_, 2), PCM_LEFT_MAX);
        if (pcmLeft_ != 3)
            cout << "PCM window: " << pcmLeft_ << " left chunks (p90 frame " << p90 << " B)\n";
    }

    sampleRate_ = profile.sampleRate;
    spec_ = cfg.spec;
    if (cfg.specScale) specScale_ = *cfg.specScale;
    pcm_ = cfg.pcm;
    if (isLayerIII_) {
        for (size_t i = 0; i < metas.size(); ++i) {
            if (!metas[i].valid || metas[i].frames.empty()) continue;
            const FrameSideInfo f0 = parseFrameSideInfoAt(chunks[i], metas[i].frames.front().offset);
            const FrameSideInfo f1 = parseFrameSideInfoAt(chunks[i], metas[i].frames.back().offset);
            if (f0.valid) {
                boundaryGain_[i].ggFirst = f0.gr[0][0].globalGain;
                boundaryGain_[i].bvFirst = f0.gr[0][0].bigValues;
            }
            if (f1.valid) {
                boundaryGain_[i].ggLast = f1.gr[f1.numGranules - 1][0].globalGain;
                boundaryGain_[i].bvLast = f1.gr[f1.numGranules - 1][0].bigValues;
            }
        }
    }
}

int JunctionScorer::gainCost(const int a, const int b) const {
    const vector<BoundaryGain>& bg = boundaryGain_;
    if (bg[a].ggLast < 0 || bg[b].ggFirst < 0) return UNKNOWN_JUNCTION_COST;
    int cost = std::abs(bg[a].ggLast - bg[b].ggFirst);
    if (bg[a].bvLast >= 0 && bg[b].bvFirst >= 0)
        cost += std::abs(bg[a].bvLast - bg[b].bvFirst) / BV_DIV;
    return cost;
}

const Mp3MainData::GranuleSpectrum* JunctionScorer::memoSpec(const bool head, const int prev, const int chunk) {
    if (chunk < 0 || !metas_[chunk].valid || metas_[chunk].frames.empty()) return nullptr;
    auto& memo = head ? specHead_ : specTail_;
    const long long key = ((long long)(prev + 1) << 20) | (unsigned)chunk;
    auto it = memo.find(key);
    if (it == memo.end()) {
        static const vector<uint8_t> empty;
        // across a slot (headerless chunk not placed yet) the predecessor's bytes don't continue
        const bool usePrev = prev >= 0 && !metas_[prev].gapAfter;
        const vector<uint8_t>& pv = usePrev ? chunks_[prev] : empty;
        const ChunkMeta* pm = usePrev ? &metas_[prev] : nullptr;
        const int off = head ? metas_[chunk].frames.front().offset : metas_[chunk].frames.back().offset;
        const Mp3MainData::FrameSpectra fsp =
            Mp3MainData::decodeChunkFrame(pm, pv, chunks_[chunk], metas_[chunk], off, sampleRate_);
        const bool mpeg1 = metas_[chunk].profile.versionID == 0b11;
        Mp3MainData::GranuleSpectrum gs; // gs.valid == false by default
        if (fsp.valid) gs = head ? fsp.gr[0][0] : fsp.gr[mpeg1 ? 1 : 0][0];
        it = memo.emplace(key, gs).first;
    }
    return it->second.valid ? &it->second : nullptr;
}

double JunctionScorer::spectralCost(const int curPrev, const int curTail, const int candHead) {
    const Mp3MainData::GranuleSpectrum* A = memoSpec(false, curPrev, curTail);
    const Mp3MainData::GranuleSpectrum* B = memoSpec(true, curTail, candHead);
    if (!A || !B) return 0.0; // no spectrum on one side -> spectral term contributes nothing
    double s = 0;
    for (int q = 0; q < Mp3MainData::NBANDS; ++q) {
        const double d = A->bandEnergy[q] - B->bandEnergy[q];
        s += d * d;
    }
    return specScale_ * std::sqrt(s);
}

double JunctionScorer::pcmRaw(const int* left, int nLeft, const int cand) {
    while (nLeft > 1 && !metas_[left[0]].valid) { ++left; --nLeft; }
    // A window spanning a slot (a headerless chunk not placed yet) can't be decoded: neutral
    // once calibrated, never "unmeasured" - the repair would otherwise bunch slots together
    // to share such windows (PLAN_headerless_chunks.md). Not memoised: neutral is set later.
    for (int k = 0; k < nLeft; ++k)
        if (metas_[left[k]].gapAfter) return pcmNeutral_ > 0 ? pcmNeutral_ : -1.0;
    PcmKey key;
    key.id.fill(-1);
    for (int k = 0; k < nLeft; ++k) key.id[k] = left[k];
    key.id[PCM_LEFT_MAX] = cand;
    auto it = pcmMemo_.find(key);
    if (it != pcmMemo_.end()) return it->second;
    PcmKey lkey = key;
    lkey.id[PCM_LEFT_MAX] = -1;   // the left ids only
    auto lit = leftCache_.find(lkey);
    if (lit == leftCache_.end()) {
        vector<const vector<uint8_t>*> segs;
        for (int k = 0; k < nLeft; ++k) segs.push_back(&chunks_[left[k]]);
        if (leftOrder_.size() >= 64) { leftCache_.erase(leftOrder_.front()); leftOrder_.pop_front(); }
        lit = leftCache_.emplace(lkey, std::unique_ptr<PcmJunction::LeftContext>(
                  new PcmJunction::LeftContext(segs, metas_[left[0]].headOffset))).first;
        leftOrder_.push_back(lkey);
    }
    const PcmJunction::Metrics m = lit->second->score(chunks_[cand]);
    const float v = (m.valid && !m.silent) ? (float)std::log1p(m.flux) : -1.0f;
    pcmMemo_.emplace(key, v);
    return v;
}

double JunctionScorer::pcmCost(const int* left, const int nLeft, const int cand) {
    const double v = pcmRaw(left, nLeft, cand);
    return pcmScale_ * (v < 0 ? pcmNeutral_ : v);
}

int JunctionScorer::leftOf(const vector<int>& seq, const int k, int left[PCM_LEFT_MAX]) const {
    int nl = 0;
    for (int j = k - pcmLeft_; j < k; ++j) if (j >= 0) left[nl++] = seq[j];
    return nl;
}

FluxSum JunctionScorer::summedFlux(const vector<int>& seq) {
    FluxSum c;
    for (size_t k = 1; k < seq.size(); ++k) {
        int left[PCM_LEFT_MAX];
        const int nl = leftOf(seq, (int)k, left);
        const double v = pcmRaw(left, nl, seq[k]);
        if (v < 0) ++c.unmeasured; else c.sum += v;
    }
    return c;
}

// Sample random (prev, a, b) triples, take the median raw L2 distance, and scale so a median
// junction contributes SPEC_TARGET (~a gentle nudge, well below a typical |Δglobal_gain|).
// Removes the magic constant and adapts per file. RP_SPEC_SCALE overrides the result.
void JunctionScorer::calibrateSpectral() {
    if (!spectralOn() || specScaleOverride_) return;
    vector<int> validChunks;
    for (int i = 0; i < static_cast<int>(chunks_.size()); ++i)
        if (metas_[i].valid && !metas_[i].frames.empty()) validChunks.push_back(i);
    if (validChunks.size() < 8) return;
    const double savedScale = specScale_;
    specScale_ = 1.0; // raw L2
    std::mt19937 rc(12345);
    std::uniform_int_distribution<int> pc(0, static_cast<int>(validChunks.size()) - 1);
    vector<double> d;
    for (int t = 0; t < 3000 && d.size() < 800; ++t) {
        const int p = validChunks[pc(rc)], a = validChunks[pc(rc)], b = validChunks[pc(rc)];
        if (a == b || a == p) continue;
        const double v = spectralCost(p, a, b);
        if (v > 0) d.push_back(v);
    }
    if (d.size() >= 30) {
        std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
        const double med = d[d.size() / 2];
        constexpr double SPEC_TARGET = 3.0;
        specScale_ = med > 1e-6 ? SPEC_TARGET / med : savedScale;
    } else {
        specScale_ = savedScale;
    }
    specHead_.clear();
    specTail_.clear();
    cout << "Spectral term: calibrated scale = " << fixed << setprecision(2) << specScale_
         << " (" << d.size() << " samples)\n";
}

// z-score style: over a sample of real structural edges a->b with a plausible
// (pcmLeft - 1)-chunk path context (... ->pp ->p->a, drawn from the graph's in-edges), scale
// log1p(flux) so its spread is RP_PCM_W times the spread of gain + spectral. Unmeasurable
// junctions get the median (neutral) value.
void JunctionScorer::calibratePcm(const vector<vector<int>>& adj) {
    if (!(pcm_ && rank_)) {
        pcm_ = false;
        return;
    }
    const double weight = pcmWeight_;
    const int n = static_cast<int>(chunks_.size());
    vector<vector<int>> preds(n);
    for (int a = 0; a < n; ++a) for (int b : adj[a]) preds[b].push_back(a);
    std::mt19937 rc(777);
    std::uniform_int_distribution<int> pa(0, n - 1);
    vector<double> pv, ov;
    for (int t = 0; t < 20000 && pv.size() < 600; ++t) {
        const int a = pa(rc);
        if (adj[a].size() < 2 || preds[a].empty()) continue;
        // backward walk a <- p <- pp <- ... of pcmLeft chunks, no repeats
        int walk[PCM_LEFT_MAX], nw = 0;
        walk[nw++] = a;
        bool ok = true;
        for (int d = 1; d < pcmLeft_ && ok; ++d) {
            const int x = walk[nw - 1];
            const int q = preds[x][rc() % preds[x].size()];
            ok = std::find(walk, walk + nw, q) == walk + nw && (d == pcmLeft_ - 1 || !preds[q].empty());
            walk[nw++] = q;
        }
        if (!ok) continue;
        const int b = adj[a][rc() % adj[a].size()];
        if (std::find(walk, walk + nw, b) != walk + nw) continue;
        const int p = nw > 1 ? walk[1] : -1;
        int left[PCM_LEFT_MAX];
        for (int k = 0; k < nw; ++k) left[k] = walk[nw - 1 - k];
        const double raw = pcmRaw(left, nw, b);
        if (raw < 0) continue;
        double o = gainCost(a, b);
        if (spectralOn()) o += spectralCost(p, a, b);
        pv.push_back(raw);
        ov.push_back(o);
    }
    auto sdev = [](const vector<double>& v) {
        double m = 0, s = 0;
        for (double x : v) m += x;
        m /= std::max<size_t>(1, v.size());
        for (double x : v) s += (x - m) * (x - m);
        return std::sqrt(s / std::max<size_t>(1, v.size()));
    };
    if (pv.size() >= 30) {
        const double sp = sdev(pv), so = sdev(ov);
        pcmScale_ = sp > 1e-9 ? weight * std::max(so, 1.0) / sp : 0.0;
        vector<double> tmp = pv;
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        pcmNeutral_ = tmp[tmp.size() / 2];
    } else {
        pcmScale_ = weight * 10.0;   // too few samples: just make it dominant-ish
        pcmNeutral_ = 0.7;
    }
    pcmMemo_.clear();
    cout << "PCM term: weight " << weight << ", scale = " << fixed << setprecision(2) << pcmScale_
         << ", neutral = " << pcmNeutral_ << " (" << pv.size() << " calibration edges)\n";
}
