#include "search/repair.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <limits>
#include "mp3/reservoir.hpp"

using namespace std;

OrderRepair::OrderRepair(const vector<vector<uint8_t>>& chunks, const vector<ChunkMeta>& metas,
                         JunctionScorer& scorer, const vector<vector<int>>& adjFull,
                         const StreamProfile& profile, const Config::Repair& opt, const vector<int>* origIndexOf)
    : chunks_(chunks), metas_(metas), scorer_(scorer), adjFull_(adjFull), isLayerIII_(profile.isLayerIII()),
      sideInfo_(sideInfoSize(profile.versionID, profile.isMono())),
      maxReservoir_(maxReservoirBytes(profile.versionID)), opt_(opt), origIndexOf_(origIndexOf),
      preds_(adjFull.size()) {
    for (size_t a = 0; a < adjFull.size(); ++a)
        for (int b : adjFull[a]) { edge_.insert(((uint64_t)a << 32) | (unsigned)b); preds_[b].push_back((int)a); }
}

bool OrderRepair::isEdge(const int a, const int b) const {
    return edge_.count(((uint64_t)(unsigned)a << 32) | (unsigned)b) > 0;
}

int OrderRepair::trueJunctions(const vector<int>& seq) const {
    int tj = 0;
    if (origIndexOf_)
        for (size_t k = 1; k < seq.size(); ++k) if ((*origIndexOf_)[seq[k]] == (*origIndexOf_)[seq[k - 1]] + 1) ++tj;
    return tj;
}

void OrderRepair::prepare() {
    const vector<int>& seq = *seq_;
    for (int k = 0; k < n_; ++k) {
        double v = 0;
        if (k >= 1) {
            int left[PCM_LEFT_MAX];
            const int nl = scorer_.leftOf(seq, k, left);
            v = scorer_.pcmRaw(left, nl, seq[k]);
        }
        jv_[k] = v;
        PS_[k + 1] = PS_[k] + ((k >= 1 && v >= 0) ? v : 0);
        PU_[k + 1] = PU_[k] + ((k >= 1 && v < 0) ? 1 : 0);
    }
    int level = 0;
    const ChunkMeta* prev = nullptr;
    for (int k = 0; k < n_; ++k) {
        bool br = false;
        if (isLayerIII_ && !advanceReservoirChunk(level, prev, metas_[seq[k]], chunks_[seq[k]], sideInfo_, maxReservoir_)) {
            br = true;
            level = maxReservoir_ * 8;
        }
        lvlAfter_[k] = level;
        PB_[k + 1] = PB_[k] + (br ? 1 : 0);
        prev = &metas_[seq[k]];
    }
}

int OrderRepair::evalBreaks(const vector<Seg>& segs) const {
    if (!isLayerIII_) return 0;
    const vector<int>& seq = *seq_;
    int level = 0, breaks = 0;
    const ChunkMeta* prev = nullptr;
    for (const Seg& sg : segs) {
        for (int k = sg.first; k <= sg.second; ++k) {
            if (k > sg.first && level == lvlAfter_[k - 1]) {   // in sync with the current order
                breaks += PB_[sg.second + 1] - PB_[k];
                level = lvlAfter_[sg.second];
                break;
            }
            if (!advanceReservoirChunk(level, prev, metas_[seq[k]], chunks_[seq[k]], sideInfo_, maxReservoir_)) {
                ++breaks;
                level = maxReservoir_ * 8;
            }
            prev = &metas_[seq[k]];
        }
        prev = &metas_[seq[sg.second]];
    }
    return breaks;
}

void OrderRepair::evalFlux(const vector<Seg>& segs, Cost& c) {
    const vector<int>& seq = *seq_;
    int g = 0;              // position in the candidate
    int last[PCM_LEFT_MAX], nl = 0;    // last (up to) pcmLeft chunks placed, oldest first
    const int W = scorer_.pcmLeft();
    for (const Seg& sg : segs) {
        for (int k = sg.first; k <= sg.second; ++k, ++g) {
            if (g >= 1) {
                if (k - sg.first >= W || sg.first == 0) {   // same window as the current order
                    c.sum += PS_[sg.second + 1] - PS_[k];
                    c.unmeasured += PU_[sg.second + 1] - PU_[k];
                    g += sg.second - k;
                    k = sg.second;
                    // Only a segment of >= W chunks is skipped mid-way (or the order's
                    // prefix, which starts at 0), so the last W chunks are all in it.
                    nl = 0;
                    for (int j = std::max(sg.first, sg.second - (W - 1)); j <= sg.second; ++j) last[nl++] = seq[j];
                    goto nextSeg;
                }
                const double v = scorer_.pcmRaw(last, nl, seq[k]);
                if (v < 0) ++c.unmeasured; else c.sum += v;
            }
            if (nl < W) last[nl++] = seq[k];
            else { std::copy(last + 1, last + W, last); last[W - 1] = seq[k]; }
        }
        nextSeg:;
    }
}

vector<int> OrderRepair::build(const vector<Seg>& segs) const {
    const vector<int>& seq = *seq_;
    vector<int> t; t.reserve(n_);
    for (const Seg& sg : segs) t.insert(t.end(), seq.begin() + sg.first, seq.begin() + sg.second + 1);
    return t;
}

vector<char> OrderRepair::suspectJunctions() const {
    const int suspects = opt_.suspects ? *opt_.suspects : std::max(16, n_ / 20);
    vector<char> bad(n_ + 1, suspects <= 0 ? 1 : 0);   // bad[k]: junction seq[k-1] -> seq[k]
    if (suspects > 0) {
        vector<pair<double, int>> js;
        for (int k = 1; k < n_; ++k) js.push_back({jv_[k] < 0 ? 1e9 : jv_[k], k});
        std::sort(js.begin(), js.end(), [](const pair<double, int>& x, const pair<double, int>& y) { return x.first > y.first; });
        for (int q = 0; q < (int)js.size() && q < suspects; ++q) bad[js[q].second] = 1;
    }
    if (cur_.breaks > 0)
        for (int k = 0; k < n_; ++k)
            if (PB_[k + 1] > PB_[k]) for (int d = 0; d <= 8; ++d) if (k - d >= 1) bad[k - d] = 1;
    return bad;
}

void OrderRepair::consider(vector<Seg>&& segs, string&& what, vector<Move>& moves, long& candidates) {
    if (outOfTime_) return;
    if ((candidates & 0xFF) == 0 && chrono::steady_clock::now() >= deadline_) { outOfTime_ = true; return; }
    ++candidates;
    Cost c;
    c.breaks = evalBreaks(segs);
    // valid order: stay valid; invalid order: a move must reduce the breaks
    if (cur_.breaks > 0 ? c.breaks >= cur_.breaks : c.breaks > 0) return;
    if (!trackBreaks_) c.breaks = 0;
    evalFlux(segs, c);
    if (c < cur_) moves.push_back({c, std::move(segs), std::move(what)});
    else if (c.breaks == cur_.breaks && c.unmeasured == cur_.unmeasured && c.sum - cur_.sum < nearest_) {
        nearest_ = c.sum - cur_.sum;
        nearestWhat_ = what;
    }
}

void OrderRepair::scanMoves(const vector<int>& pos, const vector<char>& bad, vector<Move>& moves, long& candidates) {
    const vector<int>& seq = *seq_;
    const int n = n_;
    auto cut = [&](int k) { return k < n && bad[k]; };
    auto push = [](vector<Seg>& v, int a, int b) { if (a <= b) v.push_back({a, b}); };

    // block moves: take seq[i..j] out and reinsert it after position k
    for (int i = 1; i < n && !outOfTime_; ++i)
        for (int p : preds_[seq[i]]) {
            const int k = pos[p];
            if (k < 0 || k == i - 1) continue;
            const bool outerCut = cut(i) || cut(k + 1);
            for (int j = i; j < n && !outOfTime_; ++j) {
                if (k >= i && k <= j) break;
                if (!outerCut && !cut(j + 1)) continue;
                if (j + 1 < n && !isEdge(seq[i - 1], seq[j + 1])) continue;
                if (k + 1 < n && !isEdge(seq[j], seq[k + 1])) continue;
                vector<Seg> sg;
                if (k < i) { push(sg, 0, k); push(sg, i, j); push(sg, k + 1, i - 1); push(sg, j + 1, n - 1); }
                else       { push(sg, 0, i - 1); push(sg, j + 1, k); push(sg, i, j); push(sg, k + 1, n - 1); }
                consider(std::move(sg), "move [" + std::to_string(i) + ".." + std::to_string(j) +
                                        "] after " + std::to_string(k), moves, candidates);
            }
        }
    // block swaps: seq[i1..j1] <-> seq[i2..j2] with a non-empty middle
    for (int i1 = 1; i1 < n && !outOfTime_; ++i1)
        for (int s2 : adjFull_[seq[i1 - 1]]) {
            const int i2 = pos[s2];
            if (i2 < 0 || i2 <= i1 + 1) continue;
            if (!isEdge(seq[i2 - 1], seq[i1])) continue;
            const bool outerCut = cut(i1) || cut(i2);
            for (int j1 = i1; j1 + 1 < i2 && !outOfTime_; ++j1)
                for (int p : preds_[seq[j1 + 1]]) {
                    const int j2 = pos[p];
                    if (j2 < i2) continue;
                    if (!outerCut && !cut(j1 + 1) && !cut(j2 + 1)) continue;
                    if (j2 + 1 < n && !isEdge(seq[j1], seq[j2 + 1])) continue;
                    vector<Seg> sg;
                    push(sg, 0, i1 - 1); push(sg, i2, j2); push(sg, j1 + 1, i2 - 1);
                    push(sg, i1, j1); push(sg, j2 + 1, n - 1);
                    consider(std::move(sg), "swap [" + std::to_string(i1) + ".." + std::to_string(j1) +
                                            "] <-> [" + std::to_string(i2) + ".." + std::to_string(j2) + "]",
                             moves, candidates);
                }
        }
}

int OrderRepair::run(vector<int>& seq, double* gap) {
    seq_ = &seq;
    n_ = static_cast<int>(seq.size());
    const double minFluxGain = opt_.minGain;
    // Phase 0 of each pass only evaluates moves that cut a suspect junction; phase 1 scans
    // every move if phase 0 found nothing (opt.suspects = K, 0 = no filter). opt.budget (s)
    // bounds the whole repair - when spent, the best move found so far is applied.
    const int suspects = opt_.suspects ? *opt_.suspects : std::max(16, n_ / 20);
    deadline_ = chrono::steady_clock::now() +
        chrono::duration_cast<chrono::steady_clock::duration>(chrono::duration<double>(opt_.budget));
    outOfTime_ = false;
    if (gap) *gap = std::numeric_limits<double>::quiet_NaN();
    nearest_ = std::numeric_limits<double>::infinity();
    nearestWhat_.clear();
    jv_.assign(n_, 0);
    PS_.assign(n_ + 1, 0);
    PU_.assign(n_ + 1, 0);
    lvlAfter_.assign(n_, 0);
    PB_.assign(n_ + 1, 0);

    prepare();
    cur_ = Cost();
    cur_.sum = PS_[n_]; cur_.unmeasured = PU_[n_];
    trackBreaks_ = isLayerIII_ && PB_[n_] > 0;
    if (trackBreaks_) cur_.breaks = PB_[n_];
    int applied = 0;
    for (int pass = 0; pass < opt_.passes && !outOfTime_; ++pass) {
        if (pass > 0) prepare();
        vector<int> pos(chunks_.size(), -1);
        for (int k = 0; k < n_; ++k) pos[seq[k]] = k;
        vector<char> bad = suspectJunctions();
        vector<Move> moves;
        long candidates = 0;

        bool moved = false;
        double belowGain = -1;   // best rejected flux-only gain (< minFluxGain), -1 = none
        for (int phase = suspects > 0 ? 0 : 1; phase < 2 && !moved && !outOfTime_; ++phase) {
            if (phase == 1) std::fill(bad.begin(), bad.end(), 1);
            moves.clear();
            candidates = 0;
            nearest_ = std::numeric_limits<double>::infinity();
            scanMoves(pos, bad, moves, candidates);

            if (moves.empty()) continue;
            const Move& m = *std::min_element(moves.begin(), moves.end(),
                                              [](const Move& x, const Move& y) { return x.cost < y.cost; });
            // Flux-only move on a reservoir-valid order (same breaks and unmeasured count): apply
            // it only if it lowers the summed flux by >= minFluxGain - small gains are noise and
            // can turn a true order into a cheaper wrong one.
            if (minFluxGain > 0 && m.cost.breaks == 0 && cur_.breaks == 0 && m.cost.unmeasured == cur_.unmeasured &&
                cur_.sum - m.cost.sum < minFluxGain) {
                belowGain = std::max(belowGain, cur_.sum - m.cost.sum);
                continue;   // no acceptable move in this phase (phase 0 -> the full scan still runs)
            }
            cout << "  repair pass " << pass + 1 << (phase == 0 ? " (suspects)" : " (full scan)") << ": " << m.what
                 << ", cost (" << (trackBreaks_ ? "resv breaks, " : "") << "unmeasured, summed flux) ("
                 << (trackBreaks_ ? std::to_string(cur_.breaks) + ", " : string()) << cur_.unmeasured << ", "
                 << fixed << setprecision(3) << cur_.sum << ") -> ("
                 << (trackBreaks_ ? std::to_string(m.cost.breaks) + ", " : string()) << m.cost.unmeasured << ", "
                 << m.cost.sum << ") (" << candidates << " valid moves scanned, " << moves.size() << " improving)";
            const int tjBefore = trueJunctions(seq);
            seq = build(m.segs);
            cout << " [vs source: true junctions " << tjBefore << " -> " << trueJunctions(seq) << "]\n";
            cur_ = m.cost;
            ++applied;
            moved = true;
        }

        if (!moved && belowGain >= 0) {
            cout << "  repair pass " << pass + 1 << ": best flux-only move lowers the summed flux by only "
                 << fixed << setprecision(3) << belowGain << " (< " << minFluxGain << ") - stopping\n";
            break;
        }
        if (!moved) {
            cout << "  repair pass " << pass + 1 << ": no improving valid move";
            if (!outOfTime_) {
                if (gap) *gap = nearest_;
                if (std::isfinite(nearest_))
                    cout << " (nearest valid alternative: +" << fixed << setprecision(3) << nearest_
                         << " summed flux, " << nearestWhat_ << ")";
                else cout << " (no other valid ordering within one move)";
            }
            cout << "\n";
            break;
        }
        if (outOfTime_) cout << "  repair: time budget (" << opt_.budget << " s) spent - stopping\n";
    }
    return applied;
}
