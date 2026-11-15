#include "search/dfs.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include "mp3/reservoir.hpp"

using namespace std;

DfsSearch::DfsSearch(const vector<vector<uint8_t>>& chunks, const vector<ChunkMeta>& metas,
                     JunctionScorer& scorer, const StreamProfile& profile, const bool rank)
    : chunks_(chunks), metas_(metas), scorer_(scorer), isLayerIII_(profile.isLayerIII()),
      sideInfo_(sideInfoSize(profile.versionID, profile.isMono())),
      maxReservoir_(maxReservoirBytes(profile.versionID)), rank_(rank) {}

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
         << " (continuity ranking " << (rank_ ? "ON" : "OFF")
         << (scorer_.spectralOn() ? ", +spectral" : "")
         << (scorer_.pcmOn() ? ", +pcm" : "") << ")\n";

    for (const int beam : schedule) {
        if (haveDeadline_ && chrono::steady_clock::now() >= deadline_) {
            cout << "  (time budget spent - skipping remaining beam widths)\n";
            break;
        }
        beamK_   = beam;
        counter_ = 0;
        aborted_ = false;
        DfsState state = start;
        start_ = chrono::steady_clock::now();

        cout << "  beam=" << (beamK_ ? std::to_string(beamK_) : string("unlimited")) << " ... " << flush;
        const bool found = dfs(state, result);
        cout << counter_ << " nodes, " << fixed << setprecision(2) << elapsedSec() << "s"
             << (found ? " -> FOUND" : (aborted_ ? " (stopped: budget)" : " (exhausted, no ordering)"))
             << "\n" << flush;
        if (found) return true;
    }
    return false;
}

bool DfsSearch::dfs(DfsState& state, vector<int>& result) {
    const vector<Supernode>& sn = *supernodes_;
    const vector<vector<int>>& adj = *superAdj_;

    if (aborted_) return false;
    if (state.order.size() > deepest_.size()) deepest_ = state.order;

    if (state.order.size() == sn.size()) {   // the reservoir was checked chunk by chunk on the way
        result = state.order;
        return true;
    }

    const int cur = state.order.back();
    const int curTailChunk = sn[cur].chunks.back();

    // Chunk immediately preceding curTailChunk in the current path (for the trailing spectrum's
    // cross-chunk main-data assembly). -1 when cur is the start.
    int curPrevChunk = -1;
    if (sn[cur].chunks.size() >= 2)
        curPrevChunk = sn[cur].chunks[sn[cur].chunks.size() - 2];
    else if (state.order.size() >= 2)
        curPrevChunk = sn[state.order[state.order.size() - 2]].chunks.back();

    // Try structurally-legal successors in order of junction continuity (smallest cost first).
    // Pure exploration ordering - every candidate is still tried (subject to the beam cap),
    // and the reservoir check below is unchanged.
    vector<int> candidates(adj[cur].begin(), adj[cur].end());
    if (rank_ && candidates.size() > 1) {
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

    int recursed = 0; // reservoir-valid candidates descended into so far, for the beam cap
    for (int si : candidates) {
        if (state.used[si]) continue;

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
        // Wall-clock stop, sampled every 64 placements: with the PCM term a single placement can
        // cost tens of ms (a decode per candidate).
        ++counter_;
        if (haveDeadline_ && (counter_ & 0x3F) == 0 && chrono::steady_clock::now() >= deadline_)
            aborted_ = true;

        const bool hit = !aborted_ && dfs(state, result);

        state.order.pop_back();
        state.used[si] = false;
        state.reservoirLevel = savedLevel;

        if (hit) return true;
        if (aborted_) return false;
        if (beamK_ && ++recursed >= beamK_) break;
    }
    return false;
}
