#include "search/reconstruct.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include "graph/adjacency.hpp"
#include "graph/supernodes.hpp"
#include "mp3/reservoir.hpp"
#include "run_report.hpp"
#include "scoring/edge_lock.hpp"
#include "search/chain_merge.hpp"
#include "search/dfs.hpp"
#include "search/global_match.hpp"
#include "search/repair.hpp"

using namespace std;

namespace {

constexpr int LOCK_ROUNDS = 8;     // most lock + AC-3 rounds per attempt
constexpr int LOCK_CONTEXTS = 8;   // predecessor contexts per edge

class Reconstructor {
public:
    Reconstructor(const Problem& p, const ChunkGraph& graph, JunctionScorer& scorer, const Config& cfg)
        : chunks_(p.chunks), metas_(p.metas), origIndexOf_(p.origIndexOf), graph_(graph),
          scorer_(scorer), cfg_(cfg), isLayerIII_(p.profile.isLayerIII()),
          sideInfo_(sideInfoSize(p.profile.versionID, p.profile.isMono())),
          maxReservoir_(maxReservoirBytes(p.profile.versionID)),
          dfs_(p.chunks, p.metas, scorer, p.profile, cfg.rank),
          repair_(p.chunks, p.metas, scorer, graph.adjFull, p.profile, cfg.repair, p.origIndexOf) {
        lockTaus_ = lockSchedule();
        // Lock/matching windows are [p, a, b], one chunk less on the left than the DFS's; past 3
        // they get pcmLeft - 3 warm-up chunks before p (EdgeLock::buildWarmChains).
        warm_ = EdgeLock::buildWarmChains(graph.adj, scorer.pcmLeft() - 3);
        warmPtr_ = warm_.empty() ? nullptr : &warm_;
    }

    bool run(Reconstruction& out);

private:
    vector<double> lockSchedule() const;
    void lockEdges(double tau);
    bool collapse();
    enum class Start { Ok, Abandon, Fatal };
    Start primeStart(double tau);
    void chooseBeamSchedule();
    void search(size_t lockAttempt);
    bool tryMatch(size_t lockAttempt);
    void keepDeepest();
    void finalRepair(vector<int>& seq);
    int trueJunctions(const vector<int>& seq) const;

    const vector<vector<uint8_t>>& chunks_;
    const vector<ChunkMeta>& metas_;
    const vector<int>& origIndexOf_;
    const ChunkGraph& graph_;
    JunctionScorer& scorer_;
    const Config& cfg_;
    const bool isLayerIII_;
    const int sideInfo_;
    const int maxReservoir_;
    DfsSearch dfs_;
    OrderRepair repair_;

    vector<double> lockTaus_;
    // Window fluxes are shared across lock rounds and tau attempts (a window's flux depends only
    // on its three chunks), and scored on RP_THREADS threads (default: all cores).
    EdgeLock::FluxMemo lockMemo_;
    EdgeLock::WarmChains warm_;
    const EdgeLock::WarmChains* warmPtr_ = nullptr;

    // the current lock attempt
    vector<vector<int>> adj_;
    vector<Supernode> supernodes_;
    vector<vector<int>> superAdj_;
    int startSuper_ = -1;
    DfsState primed_;   // chunk 0's supernode placed, reservoir advanced through its own frames
    vector<int> beamSchedule_;
    bool broaden_ = false;

    // the result
    bool found_ = false;
    vector<int> result_;           // supernode order
    bool foundByMatch_ = false;    // ordering came from the global matching (already repaired)
    bool foundByHint_ = false;     // ordering came from the DFS rerun with the matching as ranking hint
    vector<int> matchSeq_;         // its chunk sequence (possibly already repaired)
    double resultGap_ = std::numeric_limits<double>::quiet_NaN();
    int resultRepairMoves_ = 0;
    // Deepest partial path over ALL lock attempts, in chunks (each attempt has its own
    // supernode numbering): a late attempt that dead-ends at once mustn't hide an earlier
    // attempt's long partial path in the failure report.
    vector<int> bestPartial_;
    size_t bestPartialSupers_ = 0, bestPartialOf_ = 0;
};

int Reconstructor::trueJunctions(const vector<int>& seq) const {
    int tj = 0;
    for (size_t k = 1; k < seq.size(); ++k) if (origIndexOf_[seq[k]] == origIndexOf_[seq[k - 1]] + 1) ++tj;
    return tj;
}

// Confident-edge locking (HOW_IT_WORKS 4.9): force edges whose full-PCM junction flux beats
// every competitor - on both the successor and predecessor side - by a factor >= tau (median
// over the predecessor contexts), iterated with AC-3. RP_LOCK_TAU sets the schedule (comma list,
// e.g. "1.5" for a single attempt); RP_LOCK=0 disables. Without locking: one attempt, tau 0.
vector<double> Reconstructor::lockSchedule() const {
    if (!cfg_.lock.on || !isLayerIII_ || cfg_.chain) return {0.0};
    // Cost cap: one round is ~sum_a min(|preds(a)|, 8) * |succ(a)| window decodes (~0.3-0.8 ms
    // each, chunk-size independent since the window is byte-capped), spread over RP_THREADS
    // threads, and later rounds/attempts mostly hit the memo. The cap is a decode count, not a
    // time, so that WHETHER locking runs - and so the result - doesn't depend on the core count.
    long estimate = 0;
    vector<int> indeg(chunks_.size(), 0);
    for (const auto& v : graph_.adj) for (int b : v) ++indeg[b];
    for (size_t x = 0; x < graph_.adj.size(); ++x)
        estimate += (long)std::min(indeg[x], LOCK_CONTEXTS) * (long)graph_.adj[x].size();
    const long cap = cfg_.lock.maxDecodes;
    if (cap > 0 && estimate > cap) {
        cout << "Edge locking: skipped (~" << estimate << " decodes/round > " << cap
             << "; RP_LOCK_MAX_DECODES raises the cap, 0 removes it)\n";
        return {0.0};
    }
    return cfg_.lock.taus;
}

// Locks on adj_ at `tau`, re-locking on the pruned graph until nothing new locks.
void Reconstructor::lockEdges(const double tau) {
    const auto t0 = chrono::steady_clock::now();
    long totalLocks = 0, totalTrue = 0, totalDec = 0;
    for (int round = 1; round <= LOCK_ROUNDS; ++round) {
        long dec = 0;
        const auto locks = EdgeLock::findConfidentEdges(chunks_, metas_, adj_, 0, tau, LOCK_CONTEXTS,
                                                        lockMemo_, cfg_.threads, warmPtr_, &dec);
        totalDec += dec;
        if (locks.empty()) break;
        const long removed = EdgeLock::applyLocks(adj_, locks);
        int lockTrue = 0; // diagnostic only: checked against the source order
        for (const auto& l : locks) if (origIndexOf_[l.b] == origIndexOf_[l.a] + 1) ++lockTrue;
        totalLocks += (long)locks.size();
        totalTrue += lockTrue;
        long acRemoved = 0;
        if (cfg_.ac3) acRemoved = propagateArcConsistency(adj_, 0);
        cout << "Edge locking round " << round << " (tau " << tau << "): " << locks.size()
             << " locked, " << removed << " competing edges removed, AC-3 removed "
             << acRemoved << " [vs source: " << lockTrue << "/" << locks.size() << " correct]\n";
        if (acRemoved < 0) break;
    }
    const double secs = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
    cout << "Edge locking total: " << totalLocks << " edges (" << totalDec << " decodes, " << fixed
         << setprecision(2) << secs << "s) [vs source: " << totalTrue << "/" << totalLocks << " correct]\n";
    RunReport::set("locks", static_cast<long long>(totalLocks));
    RunReport::set("locks_correct", static_cast<long long>(totalTrue));
}

// Forced-chain collapse of adj_ into supernodes_; finds the supernode holding chunk 0.
bool Reconstructor::collapse() {
    int forced = 0;
    long edges = 0;
    for (const auto& out : adj_) {
        if (out.size() == 1) ++forced;
        edges += static_cast<long>(out.size());
    }
    RunReport::set("edges", static_cast<long long>(edges));
    cout << "\nAdjacency graph: " << edges << " edges, "
         << forced << "/" << chunks_.size()
         << " chunks have exactly one valid successor\n";

    superAdj_.clear();
    supernodes_ = buildSupernodes(adj_, 0, superAdj_);
    cout << "Supernodes after chain collapse: " << supernodes_.size() << "\n";
    RunReport::set("supernodes", supernodes_.size());

    startSuper_ = -1;
    for (int si = 0; si < static_cast<int>(supernodes_.size()); ++si)
        if (supernodes_[si].chunks[0] == 0) { startSuper_ = si; break; }
    if (startSuper_ == -1) {
        cerr << "Could not find supernode for chunk 0\n";
        RunReport::set("error", "no supernode for chunk 0");
        return false;
    }
    return true;
}

// The DFS starting state: chunk 0's supernode placed, the reservoir advanced through it.
Reconstructor::Start Reconstructor::primeStart(const double tau) {
    primed_ = DfsState();
    primed_.used.assign(supernodes_.size(), false);
    primed_.order.push_back(startSuper_);
    primed_.used[startSuper_] = true;
    primed_.reservoirLevel = 0;
    if (!isLayerIII_) return Start::Ok;
    const ChunkMeta* prev = nullptr; // chunk 0 is always first placed - no predecessor
    bool startOk = true;
    for (int ci : supernodes_[startSuper_].chunks) {
        if (!advanceReservoirChunk(primed_.reservoirLevel, prev, metas_[ci], chunks_[ci], sideInfo_, maxReservoir_)) {
            startOk = false;
            break;
        }
        prev = &metas_[ci];
    }
    if (startOk) return Start::Ok;
    if (tau > 0) {
        // A wrong lock chained an impossible successor onto chunk 0's supernode: this
        // attempt can't succeed, the next (from the unlocked graph) still can.
        cout << "Start supernode fails the reservoir check (a wrong lock merged into it)"
             << " - abandoning this lock attempt\n";
        dfs_.setDeepest({startSuper_});
        return Start::Abandon;
    }
    RunReport::set("error", "reservoir check failed on the starting chunk");
    cerr << "Reservoir check failed on the starting chunk - "
            "mainDataBegin metadata is likely wrong for chunk 0\n";
    return Start::Fatal;
}

// Beam schedule: a graph small enough for the exhaustive DFS to terminate is searched
// unlimited; a larger one is iteratively broadened (near-greedy first, widening on dead-end) so
// a heuristic-guided answer comes back even when the exhaustive search never would.
void Reconstructor::chooseBeamSchedule() {
    broaden_ = supernodes_.size() > 120;
    beamSchedule_ = broaden_ ? vector<int>{1, 2, 3, 4, 6, 9, 0} : vector<int>{0};
}

// One DFS run under a wall-clock budget: RP_TIME_BUDGET, else 30 s for a lock attempt with a
// fallback left and 45 s otherwise. No run is unbounded: even a graph below the broadening
// threshold can make the exhaustive DFS run for hours (test7_short at tau 1.25: 108 supernodes,
// > 500 s; with locking off, 47-109 supernodes hit the 900 s test timeout).
void Reconstructor::search(const size_t lockAttempt) {
    if (cfg_.timeBudget) dfs_.setDeadline(*cfg_.timeBudget);
    else if (broaden_) dfs_.setDeadline(45.0);
    else if (lockAttempt + 1 < lockTaus_.size()) dfs_.setDeadline(30.0);
    else dfs_.setDeadline(45.0);   // an exhaustive DFS on <= 120 supernodes can still run for hours
    if (dfs_.runSchedule(supernodes_, superAdj_, primed_, beamSchedule_, result_)) found_ = true;
}

// Global successor matching. Its ordering is used if it passes the reservoir check (after the
// repair); otherwise the DFS is rerun (fresh budget) with each matched successor ranked first.
bool Reconstructor::tryMatch(const size_t lockAttempt) {
    const auto t0 = chrono::steady_clock::now();
    MatchResult mr = globalMatch(chunks_, supernodes_, superAdj_, startSuper_, adj_, metas_, lockMemo_,
                                 cfg_.threads, warmPtr_, cfg_.match.maxDecodes);
    const double secs = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
    cout << "Global matching: " << supernodes_.size() << " supernodes, " << mr.decodes << " decodes ("
         << mr.contexts << " ctx), "
         << mr.cycles << " cycle(s), " << mr.spliced << " spliced, cost " << fixed << setprecision(3)
         << mr.cost << ", " << setprecision(2) << secs << "s";
    RunReport::set("matching", mr.order.empty() ? mr.failure : string("ok"));
    if (mr.order.empty()) { cout << " - " << mr.failure << "\n"; }
    else {
        vector<int> seqm = chunksOf(supernodes_, mr.order);
        const bool ok = !isLayerIII_ || reservoirValid(metas_, chunks_, seqm, sideInfo_, maxReservoir_);
        cout << (ok ? " - reservoir OK" : " - reservoir check FAILED")
             << " [vs source: " << trueJunctions(seqm) << "/" << (seqm.size() - 1) << " true junctions]\n";
        // Repair (moves on the full structural graph). If the order is reservoir-invalid
        // - usual, it is nearly right but a few wrong junctions break the reservoir -
        // the cost is led by the number of reservoir breaks.
        if (cfg_.repair.on) {
            const auto r0 = chrono::steady_clock::now();
            double gap = std::numeric_limits<double>::quiet_NaN();
            const int mv = repair_.run(seqm, &gap);
            const bool ok2 = reservoirValid(metas_, chunks_, seqm, sideInfo_, maxReservoir_);
            cout << "Matching repair: " << mv << " move(s), " << fixed << setprecision(2)
                 << chrono::duration<double>(chrono::steady_clock::now() - r0).count() << "s - "
                 << (ok2 ? "reservoir OK" : "still invalid") << " [vs source: " << trueJunctions(seqm) << "/"
                 << (seqm.size() - 1) << " true junctions]\n";
            if (ok2) { result_ = mr.order; matchSeq_ = seqm; resultGap_ = gap; resultRepairMoves_ = mv; foundByMatch_ = true; return true; }
        } else if (ok) { result_ = mr.order; matchSeq_ = seqm; foundByMatch_ = true; return true; }
    }
    if (mr.succ.empty()) return false;
    dfs_.setMatchHint(mr.succ);
    dfs_.clearDeadline();
    cout << "Search with the matching as ranking hint:\n";
    search(lockAttempt);
    dfs_.clearMatchHint();
    if (found_) foundByHint_ = true;   // a DFS result: normal repair policy, own label
    return found_;
}

void Reconstructor::keepDeepest() {
    vector<int> p = chunksOf(supernodes_, dfs_.deepest());
    if (p.size() > bestPartial_.size()) {
        bestPartial_ = std::move(p);
        bestPartialSupers_ = dfs_.deepest().size();
        bestPartialOf_ = supernodes_.size();
    }
}

// The final repair, on a DFS result (the matching's ordering was repaired where it was found).
void Reconstructor::finalRepair(vector<int>& seq) {
    if (foundByMatch_ || !cfg_.repair.on || !scorer_.pcmOn()) return;
    const auto t0 = chrono::steady_clock::now();
    const int moves = repair_.run(seq, &resultGap_);
    resultRepairMoves_ += moves;
    cout << "Block-move repair: " << moves << " move(s), " << fixed << setprecision(2)
         << chrono::duration<double>(chrono::steady_clock::now() - t0).count() << "s\n";
}

bool Reconstructor::run(Reconstruction& out) {
    const bool matchOn = cfg_.match.on && scorer_.pcmOn();
    for (size_t lockAttempt = 0; lockAttempt < lockTaus_.size() && !found_; ++lockAttempt) {
        RunReport::set("lock_attempts", static_cast<long long>(lockAttempt + 1));
        const double tau = lockTaus_[lockAttempt];
        adj_ = graph_.adj;
        if (lockAttempt > 0) keepDeepest();
        dfs_.clearDeepest();
        dfs_.clearDeadline();   // each lock attempt gets its own search budget
        if (lockTaus_.size() > 1)
            cout << "\n--- lock attempt " << lockAttempt + 1 << "/" << lockTaus_.size() << " (tau " << tau << ") ---\n";
        if (tau > 0) lockEdges(tau);
        if (cfg_.chain && scorer_.pcmOn()) {
            const auto joined = chainMerge(chunks_, metas_, adj_, 0, cfg_.lock.taus, scorer_.pcmLeft(),
                                           cfg_.threads, origIndexOf_);
            EdgeLock::applyLocks(adj_, joined);
            if (cfg_.ac3) propagateArcConsistency(adj_, 0);
        }
        if (!collapse()) return false;
        const Start st = primeStart(tau);
        if (st == Start::Fatal) return false;
        if (st == Start::Abandon) continue;
        chooseBeamSchedule();

        // DFS first; the global matching when it fails (matching first measured slightly worse).
        search(lockAttempt);
        if (!found_ && matchOn && tryMatch(lockAttempt)) found_ = true;
    }

    out = Reconstruction();
    if (!found_) {
        keepDeepest();
        out.budgetExhausted = dfs_.aborted();
        out.partial = bestPartial_;
        out.partialSupers = bestPartialSupers_;
        out.partialOf = bestPartialOf_;
        return true;
    }
    cout << "Reconstruction found\n";
    vector<int> seq;
    if (foundByMatch_ && !matchSeq_.empty()) seq = matchSeq_;
    else seq = chunksOf(supernodes_, result_);
    finalRepair(seq);
    out.found = true;
    out.seq = std::move(seq);
    out.source = foundByMatch_ ? Reconstruction::Source::Matching
               : foundByHint_ ? Reconstruction::Source::DfsMatchRanked : Reconstruction::Source::Dfs;
    out.repairMoves = resultRepairMoves_;
    out.gap = resultGap_;
    return true;
}

} // namespace

bool reconstruct(const Problem& p, const ChunkGraph& graph, JunctionScorer& scorer, const Config& cfg,
                 Reconstruction& out) {
    Reconstructor r(p, graph, scorer, cfg);
    return r.run(out);
}
