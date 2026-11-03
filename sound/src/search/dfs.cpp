#include "search/dfs.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include "mp3/reservoir.hpp"

using namespace std;

DfsSearch::DfsSearch(const vector<vector<uint8_t>>& chunks, const vector<ChunkMeta>& metas,
                     JunctionScorer& scorer, const StreamProfile& profile, const Options& opt)
    : chunks_(chunks), metas_(metas), scorer_(scorer), isLayerIII_(profile.isLayerIII()),
      sideInfo_(sideInfoSize(profile.versionID, profile.isMono())),
      maxReservoir_(maxReservoirBytes(profile.versionID)), opt_(opt) {}

void DfsSearch::setDeadline(const double seconds) {
    deadline_ = chrono::steady_clock::now() +
                chrono::duration_cast<chrono::steady_clock::duration>(chrono::duration<double>(seconds));
    haveDeadline_ = true;
}

double DfsSearch::elapsedSec() const {
    return chrono::duration<double>(chrono::steady_clock::now() - start_).count();
}

bool DfsSearch::runSchedule(const vector<Supernode>& supernodes, const vector<vector<int>>& superAdj,
                            const DfsState& start, const vector<int>& schedule, vector<int>& result) {
    supernodes_ = &supernodes;
    superAdj_ = &superAdj;
    cout << "Starting search over " << supernodes.size() << " supernodes"
         << " (continuity ranking " << (opt_.rank ? "ON" : "OFF")
         << ", " << (opt_.bestOf ? "best-of" : "first-valid")
         << (scorer_.spectralOn() ? ", +spectral" : "")
         << (scorer_.pcmOn() ? ", +pcm" : "") << ")\n";

    bool found = false;
    bestScore_ = LLONG_MAX;
    bestOrder_.clear();
    for (size_t attempt = 0; attempt < schedule.size(); ++attempt) {
        if (haveDeadline_ && chrono::steady_clock::now() >= deadline_) {
            cout << "  (time budget spent - skipping remaining beam widths)\n";
            break;
        }
        beamK_   = schedule[attempt];
        counter_ = 0;
        aborted_ = false;
        DfsState state = start;
        vector<int> res;
        start_ = chrono::steady_clock::now();

        cout << "  beam=" << (beamK_ ? std::to_string(beamK_) : string("unlimited")) << " ... " << flush;
        const bool hit = dfs(state, res);

        if (opt_.bestOf) {
            found = !bestOrder_.empty();
            cout << counter_ << " nodes, " << fixed << setprecision(2) << elapsedSec() << "s";
            if (found) cout << ", best score=" << bestScore_;
            else       cout << (aborted_ ? " (stopped: budget)" : " (exhausted, no ordering)");
            cout << "\n" << flush;
            if (found && bestScore_ == 0) break;       // perfect continuity - cannot improve
        } else {
            if (hit) { result = res; found = true; }
            cout << counter_ << " nodes, " << fixed << setprecision(2) << elapsedSec() << "s"
                 << (found ? " -> FOUND" : (aborted_ ? " (stopped: budget)" : " (exhausted, no ordering)"))
                 << "\n" << flush;
            if (found) break;
        }
    }
    if (opt_.bestOf && found) result = bestOrder_;
    return found;
}

bool DfsSearch::dfs(DfsState& state, vector<int>& result) {
    const vector<Supernode>& sn = *supernodes_;
    const vector<vector<int>>& adj = *superAdj_;

    if (aborted_) return false;
    if (state.order.size() > deepest_.size()) deepest_ = state.order;

    if (state.order.size() == sn.size()) {
        // Reservoir validity has already been established incrementally for every chunk
        // placed so far - nothing left to verify.
        if (!opt_.bestOf) {
            result = state.order;
            return true;
        }
        if (state.score < bestScore_) {
            bestScore_ = state.score;
            bestOrder_ = state.order;
            cout << "  [ordering found: score=" << state.score
                 << " nodes=" << counter_
                 << " elapsed=" << fixed << setprecision(1) << elapsedSec() << "s]\n" << flush;
        }
        return bestScore_ == 0; // score 0 = perfect continuity, cannot be beaten - stop early
    }

    const int cur = state.order.back();
    const int curTailChunk = sn[cur].chunks.back();

    // Chunk immediately preceding curTailChunk in the current path (for the trailing spectrum's
    // cross-chunk main-data assembly). -1 when cur is the pinned start.
    int curPrevChunk = -1;
    if (sn[cur].chunks.size() >= 2)
        curPrevChunk = sn[cur].chunks[sn[cur].chunks.size() - 2];
    else if (state.order.size() >= 2)
        curPrevChunk = sn[state.order[state.order.size() - 2]].chunks.back();

    // Try structurally-legal successors in order of junction continuity (smallest cost first).
    // Pure exploration ordering - every candidate is still tried (subject to the beam cap),
    // and the reservoir check below is unchanged.
    vector<int> candidates(adj[cur].begin(), adj[cur].end());
    if (opt_.rank && candidates.size() > 1) {
        const bool useSpec = scorer_.spectralOn();
        const bool usePcm  = scorer_.pcmOn();
        // Last (up to) pcmLeft chunks of the path, oldest first - the PCM window's left side.
        int left[PCM_LEFT_MAX], nLeft = 0;
        if (usePcm) {
            int tmp[PCM_LEFT_MAX];
            const int pcmLeft = scorer_.pcmLeft();
            for (int oi = (int)state.order.size() - 1; oi >= 0 && nLeft < pcmLeft; --oi) {
                const auto& ch = sn[state.order[oi]].chunks;
                for (int k = (int)ch.size() - 1; k >= 0 && nLeft < pcmLeft; --k) tmp[nLeft++] = ch[k];
            }
            for (int k = 0; k < nLeft; ++k) left[k] = tmp[nLeft - 1 - k];
        }
        // Each candidate's cost computed once (the PCM term is a full decode per edge).
        vector<pair<double, int>> keyed;
        keyed.reserve(candidates.size());
        for (int x : candidates) {
            const int head = sn[x].chunks.front();
            double c = scorer_.gainCost(curTailChunk, head);
            if (useSpec) c += scorer_.spectralCost(curPrevChunk, curTailChunk, head);
            if (usePcm && !state.used[x]) c += scorer_.pcmCost(left, nLeft, head);
            if (!matchSucc_.empty() && matchSucc_[cur] == x) c -= 1e9;   // global matching hint
            keyed.push_back({c, x});
        }
        stable_sort(keyed.begin(), keyed.end(),
                    [](const pair<double, int>& a, const pair<double, int>& b) { return a.first < b.first; });
        for (size_t k = 0; k < keyed.size(); ++k) candidates[k] = keyed[k].second;
    }

    int recursed = 0; // reservoir-valid candidates descended into so far, for the RP_BEAM cap
    for (int si : candidates) {
        if (state.used[si]) continue;

        const int jc = scorer_.gainCost(curTailChunk, sn[si].chunks.front());
        // Branch-and-bound: this prefix can only get more expensive, so if it already ties or
        // beats the best complete ordering there is no point descending. Candidates are sorted
        // ascending by jc when ranking is on, so every later one is >= this: break, not continue.
        if (opt_.bestOf && state.score + jc >= bestScore_) {
            if (opt_.rank) break;
            continue;
        }

        const int savedLevel = state.reservoirLevel;
        bool reservoirOk = true;
        if (isLayerIII_) {
            int prevCi = curTailChunk; // last chunk of `cur`'s supernode directly precedes si's first chunk
            for (int ci : sn[si].chunks) {
                if (!advanceReservoirChunk(state.reservoirLevel, &metas_[prevCi], metas_[ci], chunks_[ci], sideInfo_, maxReservoir_)) {
                    reservoirOk = false;
                    break;
                }
                prevCi = ci;
            }
        }
        if (!reservoirOk) {
            state.reservoirLevel = savedLevel;
            continue;
        }

        state.used[si] = true;
        state.order.push_back(si);
        state.score += jc;

        if (++counter_ % opt_.progressEvery == 0) {
            cout << "  [nodes=" << counter_ << " depth=" << state.order.size()
                 << "/" << sn.size() << " elapsed="
                 << fixed << setprecision(1) << elapsedSec() << "s]\n" << flush;
        }
        if (opt_.maxNodes && counter_ >= opt_.maxNodes) aborted_ = true;
        // Wall-clock stop. Sampled every 64 placements: with the PCM term a single placement can
        // cost tens of ms (a decode per candidate), so the old every-2^18 sampling overshot a
        // 45 s budget by minutes on dense graphs. steady_clock::now is a vDSO call (~20 ns).
        if (haveDeadline_ && (counter_ & 0x3F) == 0 && chrono::steady_clock::now() >= deadline_)
            aborted_ = true;

        const bool hit = !aborted_ && dfs(state, result);

        state.order.pop_back();
        state.used[si] = false;
        state.reservoirLevel = savedLevel;
        state.score -= jc;

        if (hit) return true;              // first-valid mode, or best-of early-out (score 0)
        if (aborted_) return false;
        if (beamK_ && ++recursed >= beamK_) break;
    }
    return false;
}
