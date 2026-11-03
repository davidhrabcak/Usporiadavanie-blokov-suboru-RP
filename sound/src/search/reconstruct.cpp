#include "search/reconstruct.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <string>
#include <unordered_set>
#include "graph/adjacency.hpp"
#include "graph/supernodes.hpp"
#include "mp3/reservoir.hpp"
#include "run_report.hpp"
#include "scoring/edge_lock.hpp"
#include "search/dfs.hpp"
#include "search/global_match.hpp"
#include "search/repair.hpp"

using namespace std;

namespace {

DfsSearch::Options dfsOptions(const Config& cfg) {
    DfsSearch::Options o;
    o.rank = cfg.rank;
    o.bestOf = cfg.search.bestOf;   // OFF by default - see DfsSearch
    o.maxNodes = cfg.search.maxNodes;
    o.progressEvery = cfg.search.progressEvery;
    return o;
}

class Reconstructor {
public:
    Reconstructor(const Problem& p, const ChunkGraph& graph, JunctionScorer& scorer, const Config& cfg)
        : chunks_(p.chunks), metas_(p.metas), origIndexOf_(p.origIndexOf), truth_(p.truth), graph_(graph),
          scorer_(scorer), cfg_(cfg), isLayerIII_(p.profile.isLayerIII()),
          sideInfo_(sideInfoSize(p.profile.versionID, p.profile.isMono())),
          maxReservoir_(maxReservoirBytes(p.profile.versionID)),
          dfs_(p.chunks, p.metas, scorer, p.profile, dfsOptions(cfg)),
          repair_(p.chunks, p.metas, scorer, graph.adjFull, p.profile, cfg.repair, &p.origIndexOf) {
        lockTaus_ = lockSchedule();
        acOk_ = cfg.ac3 != Switch::Off && (truth_.breaks == 0 || cfg.ac3 == Switch::On);
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
    void iterateMatching(MatchResult& mr);
    void keepDeepest();
    void finalRepair(vector<int>& seq);
    int trueJunctions(const vector<int>& seq) const;

    const vector<vector<uint8_t>>& chunks_;
    const vector<ChunkMeta>& metas_;
    const vector<int>& origIndexOf_;
    const TrueOrderCheck truth_;
    const ChunkGraph& graph_;
    JunctionScorer& scorer_;
    const Config& cfg_;
    const bool isLayerIII_;
    const int sideInfo_;
    const int maxReservoir_;
    DfsSearch dfs_;
    OrderRepair repair_;

    vector<double> lockTaus_;
    bool acOk_ = false;   // AC-3 after each lock round
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
    size_t foundAttempt_ = 0;      // which lock attempt produced the ordering (repair policy)
    bool foundByMatch_ = false;    // ordering came from the global matching (already repaired)
    bool foundByHint_ = false;     // ordering came from the DFS rerun with the matching as ranking hint
    bool slotOracle_ = false;
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

// Confident-edge locking (PLAN_full_pcm_decode.md step 5): force edges whose full-PCM junction
// flux beats every competitor - on both the successor and predecessor side - by a factor >= tau
// (median over the predecessor contexts), iterated with AC-3. RP_LOCK_TAU sets the schedule
// (comma list, e.g. "1.5" for a single attempt); RP_LOCK=0 disables. Same soundness gate as
// AC-3: with a real edge missing from the graph (pre-shuffle breaks) the predecessor-side
// margin can be won unopposed by a wrong edge. Without locking: one attempt, tau 0.
vector<double> Reconstructor::lockSchedule() const {
    const vector<vector<int>>& adj = graph_.adj;
    bool lockOn = cfg_.lock.mode != Switch::Off && isLayerIII_ &&
                  (truth_.breaks == 0 || cfg_.lock.mode == Switch::On);
    if (!lockOn && cfg_.lock.mode != Switch::Off && isLayerIII_)
        cout << "Edge locking: skipped (pre-shuffle found breaks; RP_LOCK=1 forces)\n";
    if (lockOn) {
        // Cost cap: one round is ~sum_a min(|preds(a)|, 8) * |succ(a)| window decodes
        // (~0.3-0.8 ms each, chunk-size independent since the window is byte-capped), spread
        // over RP_THREADS threads, and later rounds/attempts mostly hit the memo. The default
        // cap is a fixed 640k decodes (~25 s for the first round on 16 threads, a few minutes on
        // one) so that WHETHER locking runs - and so the result - doesn't depend on the core
        // count. RP_LOCK=1 forces past it; RP_LOCK_MAX_DECODES changes it.
        long estimate = 0;
        vector<int> indegTmp(chunks_.size(), 0);
        for (const auto& v : adj) for (int b : v) ++indegTmp[b];
        for (size_t x = 0; x < adj.size(); ++x)
            estimate += (long)std::min(indegTmp[x], 8) * (long)adj[x].size();
        const long cap = cfg_.lock.maxDecodes;
        if (estimate > cap && cfg_.lock.mode != Switch::On) {
            cout << "Edge locking: skipped (~" << estimate << " decodes/round > " << cap
                 << "; RP_LOCK=1 forces, RP_LOCK_MAX_DECODES raises the cap)\n";
            lockOn = false;
        }
    }
    if (!lockOn) return {0.0};
    return cfg_.lock.taus;
}

// Locks on adj_ at `tau`, re-locking on the pruned graph until nothing new locks.
void Reconstructor::lockEdges(const double tau) {
    const auto t0 = chrono::steady_clock::now();
    long totalLocks = 0, totalTrue = 0, totalDec = 0;
    for (int round = 1; round <= cfg_.lock.rounds; ++round) {
        long dec = 0;
        const auto locks = EdgeLock::findConfidentEdges(chunks_, metas_, adj_, 0, tau, 8,
                                                        EdgeLock::Mode::Median, &dec,
                                                        &lockMemo_, cfg_.threads, warmPtr_);
        totalDec += dec;
        if (locks.empty()) break;
        const long removed = EdgeLock::applyLocks(adj_, locks);
        int lockTrue = 0; // diagnostic only: checked against the source order
        for (const auto& l : locks) if (origIndexOf_[l.b] == origIndexOf_[l.a] + 1) ++lockTrue;
        totalLocks += (long)locks.size();
        totalTrue += lockTrue;
        long acRemoved = 0;
        if (acOk_) acRemoved = propagateArcConsistency(adj_, static_cast<int>(chunks_.size()), 0);
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
    const int n = static_cast<int>(chunks_.size());
    auto indeg  = computeInDegree(adj_, n);
    auto outdeg = computeOutDegree(adj_, n);

    int forced = 0;
    long edges = 0;
    for (int i = 0; i < n; ++i) {
        if (outdeg[i] == 1) ++forced;
        edges += static_cast<long>(adj_[i].size());
    }
    RunReport::set("edges", static_cast<long long>(edges));
    cout << "\nAdjacency graph: " << edges << " edges, "
         << forced << "/" << chunks_.size()
         << " chunks have exactly one valid successor\n";

    superAdj_.clear();
    supernodes_ = buildSupernodes(adj_, indeg, outdeg, n, 0, superAdj_);
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
        cout << "Pinned start supernode fails the reservoir check (a wrong lock merged into it)"
             << " - abandoning this lock attempt\n";
        dfs_.setDeepest({startSuper_});
        return Start::Abandon;
    }
    RunReport::set("error", "reservoir check failed on the pinned starting chunk");
    cerr << "Reservoir check failed on the pinned starting chunk - "
            "mainDataBegin metadata is likely wrong for chunk 0\n";
    return Start::Fatal;
}

// Beam schedule. RP_BEAM=K forces a single fixed-width attempt. Otherwise, for graphs small
// enough that an exhaustive DFS is known to terminate we just run it unlimited; for larger ones
// we iteratively broaden (near-greedy first, widening on dead-end) so a heuristic-guided answer
// comes back even when the exhaustive search never would. RP_BROADEN=0 / =1 overrides the size
// heuristic.
void Reconstructor::chooseBeamSchedule() {
    broaden_ = false;
    if (cfg_.search.beam) {
        beamSchedule_ = { *cfg_.search.beam };
    } else {
        broaden_ = cfg_.search.broaden ? *cfg_.search.broaden : supernodes_.size() > 120;
        beamSchedule_ = broaden_ ? vector<int>{1, 2, 3, 4, 6, 9, 0} : vector<int>{0};
    }
}

void Reconstructor::search(const size_t lockAttempt) {
    // Wall-clock budget for the whole search. Needed whenever the search is not a plain
    // first-valid exhaustive DFS: broadening's wider attempts and best-of both keep going
    // past the first complete ordering and would otherwise not terminate on large graphs.
    // RP_TIME_BUDGET (seconds) overrides; RP_MAX_NODES (node cap) also bounds it.
    if (dfs_.maxNodes() == 0 && (broaden_ || dfs_.bestOf())) {
        dfs_.setDeadline(cfg_.search.timeBudget ? *cfg_.search.timeBudget : broaden_ ? 45.0 : 30.0);
    } else if (cfg_.search.timeBudget) {
        dfs_.setDeadline(*cfg_.search.timeBudget);
    } else if (dfs_.maxNodes() == 0 && lockTaus_.size() > 1) {
        // With a lock schedule, no attempt may run an unbounded exhaustive DFS: locking can
        // shrink a large graph below the broadening threshold (test7_short at tau 1.25:
        // 108 supernodes, ran >500 s), which would drop rp's "always terminates" guarantee.
        // 30 s per attempt with a fallback left, 45 s for the last one.
        dfs_.setDeadline((lockAttempt + 1 < lockTaus_.size()) ? 30.0 : 45.0);
    }
    if (dfs_.runSchedule(supernodes_, superAdj_, primed_, beamSchedule_, result_)) found_ = true;
}

// Iterated matching (RP_MATCH_ITERS rounds): the assignment ignores the bit reservoir, so
// forbid the supernode arc at (or, for a break inside a supernode, just before) each reservoir
// break and re-solve; keep the result with the fewest breaks, then the lowest cost. Each round
// reuses the memoised window fluxes (~0.02-0.1 s).
void Reconstructor::iterateMatching(MatchResult& mr) {
    const int matchIters = cfg_.match.iters;
    if (!(isLayerIII_ && matchIters > 0 && !mr.order.empty())) return;
    std::unordered_set<uint64_t> forb;
    auto breaksOf = [&](const MatchResult& r, vector<int>* where) {
        const vector<int> sq = chunksOf(supernodes_, r.order);
        return reservoirBreaks(metas_, chunks_, sq, sideInfo_, maxReservoir_, where);
    };
    const long dec0 = mr.decodes;   // later rounds hit the memo
    int bestBreaks = breaksOf(mr, nullptr);
    const int firstBreaks = bestBreaks;
    int round = 0;
    MatchResult cur = mr;
    for (; round < matchIters && bestBreaks > 0; ++round) {
        vector<int> where;
        breaksOf(cur, &where);
        if (where.empty()) break;
        vector<int> superAt;   // order position of the supernode holding each chunk position
        for (int k = 0; k < (int)cur.order.size(); ++k)
            for (size_t j = 0; j < supernodes_[cur.order[k]].chunks.size(); ++j) superAt.push_back(k);
        size_t before = forb.size();
        for (int k : where) {
            const int sp = superAt[k];
            if (sp == 0) continue;   // inside the pinned start supernode
            forb.insert(((uint64_t)(unsigned)cur.order[sp - 1] << 32) | (unsigned)cur.order[sp]);
        }
        if (forb.size() == before) break;
        MatchResult nx = globalMatch(chunks_, supernodes_, superAdj_, startSuper_, adj_, metas_, lockMemo_,
                                     cfg_.threads, warmPtr_, cfg_.match.maxDecodes, &forb);
        if (nx.order.empty()) break;   // over-constrained: keep the best so far
        const int b = breaksOf(nx, nullptr);
        if (b < bestBreaks || (b == bestBreaks && nx.cost < mr.cost)) { mr = nx; bestBreaks = b; }
        cur = std::move(nx);
    }
    mr.decodes = dec0;
    cout << "Iterated matching: " << round << " round(s), " << forb.size() << " arcs forbidden, reservoir breaks "
         << firstBreaks << " -> " << bestBreaks << "\n";
}

// Global successor matching. Its ordering is used if it passes the reservoir check (after the
// repair); otherwise the DFS is rerun (fresh budget) with each matched successor ranked first.
bool Reconstructor::tryMatch(const size_t lockAttempt) {
    const auto t0 = chrono::steady_clock::now();
    MatchResult mr = globalMatch(chunks_, supernodes_, superAdj_, startSuper_, adj_, metas_, lockMemo_,
                                 cfg_.threads, warmPtr_, cfg_.match.maxDecodes);
    iterateMatching(mr);
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
        if (cfg_.repair.mode != Switch::Off) {
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

// Repair policy. Before the minimum-gain rule, an ordering found by the FIRST (strictest) lock
// attempt was left alone: on one first-attempt success (cbr32_10s, 1 KB) the repair moved the
// exact order to a lower-flux wrong one. With the reservoir arc filter the strictest attempt now
// often finds an order at once, and some are a block off (test7_short: 606/609, fixed by a
// 3.4-gain move). So the repair now always runs with the RP_REPAIR_MIN_GAIN threshold; with
// RP_REPAIR_MIN_GAIN=0 the old skip applies. RP_REPAIR=1 forces the repair, =0 disables it.
void Reconstructor::finalRepair(vector<int>& seq) {
    const bool repairForced = cfg_.repair.mode == Switch::On;
    const bool repairOff    = cfg_.repair.mode == Switch::Off;
    const bool firstOfSchedule = lockTaus_.size() > 1 && foundAttempt_ == 0 && !foundByMatch_ && cfg_.repair.minGain <= 0;
    if (foundByMatch_ && !repairForced)
        cout << "Block-move repair: skipped (the matching's ordering was already repaired)\n";
    else if (scorer_.pcmOn() && !repairOff && firstOfSchedule && !repairForced)
        cout << "Block-move repair: skipped (found by the strictest lock attempt; RP_REPAIR=1 forces)\n";
    else if (scorer_.pcmOn() && !repairOff) {
        const auto t0 = chrono::steady_clock::now();
        const int moves = repair_.run(seq, &resultGap_);
        resultRepairMoves_ += moves;
        cout << "Block-move repair: " << moves << " move(s), " << fixed << setprecision(2)
             << chrono::duration<double>(chrono::steady_clock::now() - t0).count() << "s\n";
    }
}

bool Reconstructor::run(Reconstruction& out) {
    // RP_SLOT_ORACLE=1 (diagnostic, PLAN_headerless_chunks.md stage B): skip the search and take
    // the true order of the chunks with a header, so the slot assignment is measured alone.
    if (cfg_.slotOracle) {
        matchSeq_.assign(chunks_.size(), 0);
        for (size_t ci = 0; ci < chunks_.size(); ++ci) matchSeq_[origIndexOf_[ci]] = static_cast<int>(ci);
        found_ = foundByMatch_ = slotOracle_ = true;
        cout << "Slot oracle: true order of the chunks with a header (search skipped)\n";
    }
    for (size_t lockAttempt = 0; lockAttempt < lockTaus_.size() && !found_; ++lockAttempt) {
        foundAttempt_ = lockAttempt;
        RunReport::set("lock_attempts", static_cast<long long>(lockAttempt + 1));
        const double tau = lockTaus_[lockAttempt];
        adj_ = graph_.adj;
        if (lockAttempt > 0) keepDeepest();
        dfs_.clearDeepest();
        dfs_.clearDeadline();   // each lock attempt gets its own search budget
        if (lockTaus_.size() > 1)
            cout << "\n--- lock attempt " << lockAttempt + 1 << "/" << lockTaus_.size() << " (tau " << tau << ") ---\n";
        if (tau > 0) lockEdges(tau);
        if (!collapse()) return false;
        const Start st = primeStart(tau);
        if (st == Start::Fatal) return false;
        if (st == Start::Abandon) continue;
        chooseBeamSchedule();

        // Global successor matching (RP_MATCH): 1 (default) = when this attempt's DFS fails,
        // 2 = before the DFS, 0 = off.
        int matchMode = cfg_.match.mode;
        if (!scorer_.pcmOn()) matchMode = 0;
        if (matchMode == 2 && tryMatch(lockAttempt)) { found_ = true; continue; }
        search(lockAttempt);
        if (!found_ && matchMode == 1 && tryMatch(lockAttempt)) found_ = true;
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
    cout << "Reconstruction found" << (dfs_.bestOf() ? (" (score " + std::to_string(dfs_.bestScore()) + ")") : "") << "\n";
    vector<int> seq;
    if (foundByMatch_ && !matchSeq_.empty()) seq = matchSeq_;
    else seq = chunksOf(supernodes_, result_);
    finalRepair(seq);
    out.found = true;
    out.seq = std::move(seq);
    out.source = slotOracle_ ? Reconstruction::Source::SlotOracle
               : foundByMatch_ ? Reconstruction::Source::Matching
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
