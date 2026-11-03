#include <random>
#include <algorithm>
#include <csetjmp>
#include <chrono>
#include <array>
#include <queue>
#include <string>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <climits>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <memory>
#include "mp3/frame_scanner.hpp"
#include "mp3/chunk_meta.hpp"
#include "graph/adjacency.hpp"
#include "graph/chunk_graph.hpp"
#include "graph/supernodes.hpp"
#include "mp3/main_data_decoder.hpp"
#include "scoring/pcm_junction.hpp"
#include "scoring/edge_lock.hpp"
#include "scoring/junction_scorer.hpp"
#include "search/dfs.hpp"
#include "search/global_match.hpp"
#include "search/repair.hpp"
#include "run_report.hpp"
#include "graph/slot_fill.hpp"
#include "config.hpp"
#include "mp3/reservoir.hpp"

extern char** environ;

using namespace std;

vector<vector<uint8_t>> chunks; // moved for debugging

int main(int argc, char* argv[]) {
    const Config cfg = Config::fromEnv(argc, argv);
    const string& filename = cfg.input;

    // RP_REPORT (PLAN_test_automation.md): starts the clock; written on any return from main.
    RunReport::set("file", filename);
    RunReport::set("status", "error");
    {
        string env = "{";
        for (char** e = environ; e && *e; ++e) {
            const string kv = *e;
            if (kv.compare(0, 3, "RP_") != 0 || kv.compare(0, 10, "RP_REPORT=") == 0) continue;
            const size_t eq = kv.find('=');
            if (env.size() > 1) env += ", ";
            env += RunReport::quote(kv.substr(0, eq)) + ": " + RunReport::quote(eq == string::npos ? "" : kv.substr(eq + 1));
        }
        RunReport::setRaw("env", env + "}");
    }

    Mp3FrameScanner scanner(filename);
    if (scanner.getFrameCount() == 0) {
        cerr << "No frames found\n";
        RunReport::set("error", "no frames found");
        return 1;
    }

    const long chunkSize = cfg.chunkSize;
    if (chunkSize != Config::kDefaultChunkSize) cout << "Chunk size: " << chunkSize << " bytes\n";
    chunks = scanner.chunksFromFirstFrame(chunkSize);

    // Constant-bitrate detection (A3a): if every frame shares one bitrate, enable the
    // frame-header bitrate filter in StreamProfile::matches so false-positive syncs at other
    // bitrates are rejected. Order-independent (detectCBRUnordered): the first chunk is unknown.
    bool cbr = false;
    int  cbrKbps = 0;
    {
        StreamProfile tmp{};
        if (voteProfile(chunks, tmp)) cbr = detectCBRUnordered(chunks, tmp, cbrKbps);
    }
    if (!cfg.cbrFilter) cbr = false; // A/B toggle for A3a
    if (cbr)
        cout << "Detected constant bitrate: " << cbrKbps
             << " kbps - frame-header bitrate filter enabled\n";

    // --- Pre-shuffle sanity check ---
    TrueOrderCheck truth;   // gates AC-3, the reservoir arc filter and edge locking
    // Frames longer than the chunk (PLAN_headerless_chunks.md): the headerless chunks ("pool")
    // are taken out before the shuffle. The search orders the rest; a demander's edge a => b
    // stands for a, slot, b, and the pool fills the slots at output. RP_GAP=0 disables.
    vector<vector<uint8_t>> pool;   // headerless chunks (shuffled below like the rest)
    vector<int> poolFull;           // their index in the full source stream
    vector<int> keptFull;           // full-stream index of each remaining chunk (true order); empty = no pool
    int splitTailsPre = 0;          // split-tail chunks promoted on the true order (promoteSplitTails)
    {
        // The pool is an input to the search, so nothing here may use the true first chunk:
        // the vote profile (differs from chunk 0's at most in channelIdx, which matches()
        // ignores - musiclib) and, as promoteSplitTails' `proto`, the header of any chunk that
        // can be the start (only its constant bytes 0-1 and byte 3 are read).
        StreamProfile prof0{};
        vector<StartCandidate> protoCands;
        if (voteProfile(chunks, prof0)) protoCands = findStartCandidates(chunks, prof0);
        if (!protoCands.empty()) {
            if (cbr) { prof0.isCBR = true; prof0.bitrateKbps = cbrKbps; }
            const uint8_t* proto = chunks[protoCands[0].index].data();
            vector<ChunkMeta> origMetas;
            origMetas.reserve(chunks.size());
            for (int i = 0; i < static_cast<int>(chunks.size()); ++i)
                origMetas.push_back(computeChunkMeta(i, chunks[i], prof0, static_cast<long>(chunks[i].size()) < chunkSize));

            if (cfg.gap) {
                const SplitTails st = promoteSplitTails(origMetas, chunks, chunkSize, prof0, proto);
                splitTailsPre = st.promoted[1] + st.promoted[2] + st.promoted[3];
                if (splitTailsPre > 0 || st.expected[1] + st.expected[2] + st.expected[3] > 0) {
                    cout << "Split-tail chunks (frame end + 1-3 header bytes): promoted / expected by t:";
                    for (int t = 1; t <= 3; ++t) cout << " " << t << ":" << st.promoted[t] << "/" << st.expected[t];
                    cout << "\n";
                    RunReport::set("split_tails", splitTailsPre);
                    RunReport::set("split_tails_expected", st.expected[1] + st.expected[2] + st.expected[3]);
                }
                vector<int> keep;
                for (int i = 0; i < static_cast<int>(chunks.size()); ++i)
                    (isHeaderless(origMetas[i], chunks[i].size(), chunkSize) ? poolFull : keep).push_back(i);
                if (!poolFull.empty()) {
                    vector<vector<uint8_t>> kept;
                    vector<ChunkMeta> keptMetas;
                    for (int i : poolFull) pool.push_back(chunks[i]);
                    for (int i : keep) {
                        kept.push_back(std::move(chunks[i]));
                        keptMetas.push_back(origMetas[i]);
                        keptMetas.back().chunkIndex = static_cast<int>(keptMetas.size()) - 1;
                    }
                    chunks = std::move(kept);
                    origMetas = std::move(keptMetas);
                    keptFull = std::move(keep);
                }
            }

            const StructuralGraph origGraph = buildStructuralGraph(origMetas, pool, chunkSize);
            const GapSlots& gaps0 = origGraph.gaps;
            if (!pool.empty()) {
                cout << "Headerless chunks (frames > chunk): " << pool.size() << " taken out; demanders "
                     << gaps0.demanders() << " (" << gaps0.case1 << " whole header, " << gaps0.case2
                     << " split header" << (gaps0.ambiguous ? ", " + std::to_string(gaps0.ambiguous) + " ambiguous left out" : string())
                     << ") - count check " << ((int)pool.size() == gaps0.demanders() ? "OK" : "MISMATCH") << "\n";
                RunReport::set("pool", pool.size());
                RunReport::set("demanders", gaps0.demanders());
                RunReport::set("demanders_ambiguous", gaps0.ambiguous);
                RunReport::set("count_check", (int)pool.size() == gaps0.demanders());
            }
            int broken = 0;
            for (int i = 0; i + 1 < static_cast<int>(origMetas.size()); ++i) {
                const bool follows = origMetas[i].gapAfter ? canFollowGap(origMetas[i + 1], gaps0.offsets[i])
                                                           : canFollow(origMetas[i], origMetas[i + 1]);
                if (!follows) {
                    cout << "  BREAK at " << i << "->" << i+1
                         << ": tailOvfl=" << origMetas[i].tailOverflow
                         << " tailPartialLen=" << origMetas[i].tailPartialLen
                         << " headOff=" << origMetas[i+1].headOffset << "\n";
                    ++broken;
                }
            }
            truth.breaks = broken;
            RunReport::set("pre_breaks", broken);
            if (broken == 0)
                cout << "Pre-shuffle check PASSED: all " << chunks.size()
                     << " consecutive pairs compatible\n";
            else
                cout << "Pre-shuffle check FAILED: " << broken << " breaks\n";

            // Validate the part2_3_length-based reservoir model against the true,
            // unshuffled order before trusting it inside the DFS - the same technique
            // that caught the earlier failed mainDataBegin-as-consumption attempt
            // (see advanceReservoirFrame). A correct model must never reject a real
            // chunk sequence.
            if (prof0.isLayerIII()) {
                vector<int> trueOrder(origMetas.size()), breakAt;
                for (size_t i = 0; i < trueOrder.size(); ++i) trueOrder[i] = static_cast<int>(i);
                const int reservoirBreaks = ::reservoirBreaks(origMetas, chunks, trueOrder,
                    sideInfoSize(prof0.versionID, prof0.isMono()), maxReservoirBytes(prof0.versionID), &breakAt);
                for (int i : breakAt) cout << "  RESERVOIR BREAK at chunk " << i << "\n";
                truth.reservoirBreaks = reservoirBreaks;
                RunReport::set("resv_breaks", reservoirBreaks);
                if (reservoirBreaks == 0)
                    cout << "Reservoir check PASSED on true order: all "
                         << chunks.size() << " chunks consistent\n";
                else
                    cout << "Reservoir check FAILED on true order: "
                         << reservoirBreaks << " breaks\n";
            }
        }
    }
    // --- End sanity check ---

    // Shuffle seed. Deterministic by default (C1) so a given input always reconstructs the
    // same way and results are reproducible; RP_SEED=<n> pins a specific one, RP_SEED=random
    // draws from the clock.
    const unsigned seed = cfg.seed;

    // origIndexOf[i] = original (pre-shuffle) index of the chunk now at position i. Shuffled
    // with the same seed/engine as `chunks`, so it stays in lock-step and lets us score the
    // reconstruction against the true order at the end (C2).
    vector<int> origIndexOf(chunks.size());
    for (size_t i = 0; i < origIndexOf.size(); ++i) origIndexOf[i] = static_cast<int>(i);
    shuffle(chunks.begin() + 1, chunks.end(), default_random_engine(seed));
    shuffle(origIndexOf.begin() + 1, origIndexOf.end(), default_random_engine(seed));
    shuffle(pool.begin(), pool.end(), default_random_engine(seed + 1));
    shuffle(poolFull.begin(), poolFull.end(), default_random_engine(seed + 1));

    // The first chunk is not known either: move it to a random position too. Done as one swap
    // after the shuffle above, so a correctly found start restores exactly the layout the
    // pipeline had when chunk 0 was pinned. RP_START=pinned keeps chunk 0 in place (A/B).
    const bool startPinned = cfg.startPinned;
    if (!startPinned && chunks.size() > 1) {
        std::uniform_int_distribution<size_t> pick(0, chunks.size() - 1);
        std::mt19937 rngStart(seed + 2);   // not minstd: its first draw from a small seed is ~0
        const size_t k = pick(rngStart);
        std::swap(chunks[0], chunks[k]);
        std::swap(origIndexOf[0], origIndexOf[k]);
    }

    // Find the first chunk from content alone (findStartCandidates): a header at offset 0 and,
    // for Layer III, main_data_begin == 0. Several candidates: prefer a Xing/Info/VBRI frame,
    // then the lowest index (arbitrary; reported). The start is then swapped to index 0, and
    // everything below treats index 0 as the start, as before.
    if (!startPinned) {
        StreamProfile vote{};
        if (!voteProfile(chunks, vote)) {
            cerr << "Could not derive stream profile from any chunk\n";
            RunReport::set("error", "could not derive stream profile");
            return 1;
        }
        const vector<StartCandidate> cands = findStartCandidates(chunks, vote);
        int nXing = 0;
        for (const auto& c : cands) nXing += c.xing;
        if (cands.empty()) {
            cerr << "No chunk can be the first (no header at offset 0 with main_data_begin == 0)\n";
            RunReport::set("error", "no start candidate");
            return 1;
        }
        int start = cands[0].index;
        for (const auto& c : cands) if (c.xing) { start = c.index; break; }
        int trueCands = 0;
        for (const auto& c : cands) trueCands += origIndexOf[c.index] == 0;
        cout << "Start chunk: " << cands.size() << " candidate(s)"
             << (nXing ? " (" + std::to_string(nXing) + " with a Xing/Info/VBRI frame)" : string())
             << ", chose index " << start
             << (cands.size() > 1 && !(nXing == 1) ? " - AMBIGUOUS" : "")
             << " [vs source: " << (origIndexOf[start] == 0 ? "correct" : "WRONG")
             << (trueCands ? "" : ", true start not among the candidates") << "]\n";
        RunReport::set("start_candidates", cands.size());
        RunReport::set("start_xing", nXing);
        RunReport::set("start_correct", origIndexOf[start] == 0);
        std::swap(chunks[0], chunks[start]);
        std::swap(origIndexOf[0], origIndexOf[start]);
    }

    cout << "Total chunks: " << chunks.size() << "\n";
    RunReport::set("chunk_size", static_cast<long long>(chunkSize));
    RunReport::set("seed", static_cast<long long>(seed));
    RunReport::set("chunks", chunks.size());
    cout << "Shuffle seed: " << seed << (cfg.randomSeed ? " (random)" : " (fixed default; RP_SEED to override)") << "\n";

    // Derive stream profile from chunk 0 (the start found above, or pinned with RP_START=pinned)
    StreamProfile profile{};
    if (!deriveProfile(chunks[0], profile)) {
        cerr << "Could not derive stream profile from first chunk\n";
        RunReport::set("error", "could not derive stream profile");
        return 1;
    }
    if (cbr) { profile.isCBR = true; profile.bitrateKbps = cbrKbps; }
    RunReport::setRaw("profile", string("{\"mpeg\": ") + RunReport::quote(profile.versionID == 0b11 ? "1" : profile.versionID == 0b10 ? "2" : "2.5")
        + ", \"layer\": " + std::to_string(profile.layerID == 0b01 ? 3 : profile.layerID == 0b10 ? 2 : 1)
        + ", \"sample_rate\": " + std::to_string(profile.sampleRate)
        + ", \"cbr_kbps\": " + (profile.isCBR ? std::to_string(profile.bitrateKbps) : string("null"))
        + ", \"crc\": " + (profile.hasCrc ? "true" : "false")
        + ", \"mono\": " + (profile.isMono() ? "true" : "false") + "}");
    cout << "Stream profile: versionID=" << static_cast<int>(profile.versionID)
         << " layerID=" << static_cast<int>(profile.layerID)
         << " sampleRate=" << profile.sampleRate
         << " mono=" << profile.isMono()
         << (profile.isCBR ? (" cbr=" + std::to_string(profile.bitrateKbps) + "kbps") : string(" vbr"))
         << (profile.hasCrc ? " crc" : "") << "\n";

    // Compute per-chunk metadata
    vector<ChunkMeta> metas;
    metas.reserve(chunks.size());
    int invalidCount = 0;
    for (int i = 0; i < static_cast<int>(chunks.size()); ++i) {
        metas.push_back(computeChunkMeta(i, chunks[i], profile, chunks[i].size() < chunks[0].size()));
        if (!metas.back().valid) {
            cerr << "  Chunk " << i << ": INVALID (no consistent frame run found)\n";
            ++invalidCount;
        }
    }
    if (invalidCount > 0)
        cerr << invalidCount << " invalid chunks — reconstruction may fail\n";

    // Junction scoring for the ranking, the repair and the matching. Constructed here: it
    // references `chunks` and `metas`, which don't move from now on.
    JunctionScorer scorer(chunks, metas, profile, chunkSize, cfg);
    // Continuity ranking of DFS candidates (see dfs()). Off only if RP_RANK=0 - kept as a switch
    // for the pinned-seed A/B in PLAN_sideinfo_mainbits.md Step 1.
    const bool rankByContinuity = cfg.rank;

    // Print full metadata table
    cout << "\nChunk metadata:\n";
    cout << "  idx  headOff  tailOvfl  frames\n";
    for (int i = 0; i < static_cast<int>(metas.size()); ++i) {
        cout << "  " << i
             << "  " << metas[i].headOffset
             << "  " << metas[i].tailOverflow
             << "  " << metas[i].frames.size()
             << (metas[i].valid ? "" : "  INVALID") << "\n";
    }

    ChunkGraph graph = buildGraph(metas, chunks, pool, profile, chunkSize, splitTailsPre, truth, cfg, origIndexOf);
    auto& adj = graph.adj;
    const auto& adjFull = graph.adjFull;

    // Reservoir parameters for this stream, computed once up front and threaded through
    // every DFS call rather than recomputed per edge.
    const bool isLayerIII   = profile.isLayerIII();
    const int  sideInfo     = sideInfoSize(profile.versionID, profile.isMono());
    const int  maxReservoir = maxReservoirBytes(profile.versionID);

    DfsSearch::Options dfsOpt;
    dfsOpt.rank = rankByContinuity;
    dfsOpt.bestOf = cfg.search.bestOf;   // OFF by default - see DfsSearch
    dfsOpt.maxNodes = cfg.search.maxNodes;
    dfsOpt.progressEvery = cfg.search.progressEvery;
    DfsSearch dfs(chunks, metas, scorer, profile, dfsOpt);
    OrderRepair repair(chunks, metas, scorer, adjFull, profile, cfg.repair, &origIndexOf);

    scorer.calibrateSpectral();
    scorer.calibratePcm(adj);

    // Confident-edge locking (PLAN_full_pcm_decode.md step 5): force edges whose full-PCM
    // junction flux beats every competitor - on both the successor and predecessor side - by
    // a factor >= tau (median over the predecessor contexts), iterated with AC-3. A wrong lock
    // makes the true order unreachable for the DFS, so thresholds are tried strictest first:
    // if no complete ordering is found at one tau, the whole lock + search is retried at the
    // next (lower) one. A complete-but-wrong ordering from a too-low tau is then the repair
    // pass's job (it works on the FULL graph, so it can undo a wrong lock). RP_LOCK_TAU sets
    // the schedule (comma list, e.g. "1.5" for a single attempt); RP_LOCK=0 disables.
    // Same soundness gate as AC-3: with a real edge missing from the graph (pre-shuffle
    // breaks) the predecessor-side margin can be won unopposed by a wrong edge.
    bool lockOn = cfg.lock.mode != Switch::Off && profile.isLayerIII() &&
                  (truth.breaks == 0 || cfg.lock.mode == Switch::On);
    if (!lockOn && cfg.lock.mode != Switch::Off && profile.isLayerIII())
        cout << "Edge locking: skipped (pre-shuffle found breaks; RP_LOCK=1 forces)\n";
    if (lockOn) {
        // Cost cap: one round is ~sum_a min(|preds(a)|, 8) * |succ(a)| window decodes
        // (~0.3-0.8 ms each, chunk-size independent since the window is byte-capped), spread
        // over RP_THREADS threads, and later rounds/attempts mostly hit the memo. The default
        // cap is a fixed 640k decodes (~25 s for the first round on 16 threads, a few minutes on
        // one) so that WHETHER locking runs - and so the result - doesn't depend on the core
        // count. RP_LOCK=1 forces past it; RP_LOCK_MAX_DECODES changes it.
        long estimate = 0;
        vector<int> indegTmp(chunks.size(), 0);
        for (const auto& v : adj) for (int b : v) ++indegTmp[b];
        for (size_t x = 0; x < adj.size(); ++x)
            estimate += (long)std::min(indegTmp[x], 8) * (long)adj[x].size();
        const long cap = cfg.lock.maxDecodes;
        if (estimate > cap && cfg.lock.mode != Switch::On) {
            cout << "Edge locking: skipped (~" << estimate << " decodes/round > " << cap
                 << "; RP_LOCK=1 forces, RP_LOCK_MAX_DECODES raises the cap)\n";
            lockOn = false;
        }
    }
    vector<double> lockTaus = cfg.lock.taus;
    if (!lockOn) lockTaus = {0.0};  // single attempt, no locking
    const int lockRounds = cfg.lock.rounds;  // re-lock on the pruned graph until nothing new locks
    const bool acOk = cfg.ac3 != Switch::Off && (truth.breaks == 0 || cfg.ac3 == Switch::On);

    // Window fluxes are shared across lock rounds and tau attempts (a window's flux depends only
    // on its three chunks), and scored on RP_THREADS threads (default: all cores).
    EdgeLock::FluxMemo lockMemo;
    const int lockThreads = cfg.threads;

    const auto adjBase = adj;
    // Lock/matching windows are [p, a, b], one chunk less on the left than the DFS's; past 3
    // they get pcmLeft - 3 warm-up chunks before p (EdgeLock::buildWarmChains).
    const EdgeLock::WarmChains warm = EdgeLock::buildWarmChains(adjBase, scorer.pcmLeft() - 3);
    const EdgeLock::WarmChains* warmPtr = warm.empty() ? nullptr : &warm;
    vector<vector<int>> superAdj;
    vector<Supernode> supernodes;
    vector<int> result;
    bool found = false;
    size_t foundAttempt = 0;   // which lock attempt produced the ordering (repair policy below)
    bool foundByMatch = false;  // ordering came from the global matching (already repaired)
    bool foundByHint = false;   // ordering came from the DFS rerun with the matching as ranking hint
    vector<int> matchSeq;       // its chunk sequence (possibly already repaired)
    double resultGap = std::numeric_limits<double>::quiet_NaN();   // repair's stability margin
    int resultRepairMoves = 0;   // repair moves applied to the reported order (matching or DFS repair)
    // Deepest partial path over ALL lock attempts, in chunks (each attempt has its own
    // supernode numbering): a late attempt that dead-ends at once mustn't hide an earlier
    // attempt's long partial path in the failure report.
    vector<int> bestPartial;   // chunk indices
    size_t bestPartialSupers = 0, bestPartialOf = 0;
    auto keepDeepest = [&]() {
        vector<int> p = chunksOf(supernodes, dfs.deepest());
        if (p.size() > bestPartial.size()) {
            bestPartial = std::move(p);
            bestPartialSupers = dfs.deepest().size();
            bestPartialOf = supernodes.size();
        }
    };
    // RP_SLOT_ORACLE=1 (diagnostic, PLAN_headerless_chunks.md stage B): skip the search and take
    // the true order of the chunks with a header, so the slot assignment is measured alone.
    bool slotOracle = false;
    if (cfg.slotOracle) {
        matchSeq.assign(chunks.size(), 0);
        for (size_t ci = 0; ci < chunks.size(); ++ci) matchSeq[origIndexOf[ci]] = static_cast<int>(ci);
        found = foundByMatch = slotOracle = true;
        cout << "Slot oracle: true order of the chunks with a header (search skipped)\n";
    }
    for (size_t lockAttempt = 0; lockAttempt < lockTaus.size() && !found; ++lockAttempt) {
        foundAttempt = lockAttempt;
        RunReport::set("lock_attempts", static_cast<long long>(lockAttempt + 1));
        const double tau = lockTaus[lockAttempt];
        adj = adjBase;
        if (lockAttempt > 0) keepDeepest();
        dfs.clearDeepest();
        dfs.clearDeadline();   // each lock attempt gets its own search budget
        if (lockTaus.size() > 1)
            cout << "\n--- lock attempt " << lockAttempt + 1 << "/" << lockTaus.size() << " (tau " << tau << ") ---\n";
        if (tau > 0) {
            const auto t0 = chrono::steady_clock::now();
            long totalLocks = 0, totalTrue = 0, totalDec = 0;
            for (int round = 1; round <= lockRounds; ++round) {
                long dec = 0;
                const auto locks = EdgeLock::findConfidentEdges(chunks, metas, adj, 0, tau, 8,
                                                                EdgeLock::Mode::Median, &dec,
                                                                &lockMemo, lockThreads, warmPtr);
                totalDec += dec;
                if (locks.empty()) break;
                const long removed = EdgeLock::applyLocks(adj, locks);
                int lockTrue = 0; // diagnostic only: checked against the source order
                for (const auto& l : locks) if (origIndexOf[l.b] == origIndexOf[l.a] + 1) ++lockTrue;
                totalLocks += (long)locks.size();
                totalTrue += lockTrue;
                long acRemoved = 0;
                if (acOk) acRemoved = propagateArcConsistency(adj, static_cast<int>(chunks.size()), 0);
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

        auto indeg  = computeInDegree(adj, static_cast<int>(chunks.size()));
        auto outdeg = computeOutDegree(adj, static_cast<int>(chunks.size()));

        // Report graph stats
        int forced = 0;
        long edges = 0;
        for (int i = 0; i < static_cast<int>(chunks.size()); ++i) {
            if (outdeg[i] == 1) ++forced;
            edges += static_cast<long>(adj[i].size());
        }
        RunReport::set("edges", static_cast<long long>(edges));
        cout << "\nAdjacency graph: " << edges << " edges, "
             << forced << "/" << chunks.size()
             << " chunks have exactly one valid successor\n";

        // Find which supernode contains chunk 0
        superAdj.clear();
        supernodes = buildSupernodes(
            adj, indeg, outdeg, static_cast<int>(chunks.size()), 0, superAdj);

        cout << "Supernodes after chain collapse: " << supernodes.size() << "\n";
        RunReport::set("supernodes", supernodes.size());

        int startSuper = -1;
        for (int si = 0; si < static_cast<int>(supernodes.size()); ++si)
            if (supernodes[si].chunks[0] == 0) { startSuper = si; break; }

        if (startSuper == -1) {
            cerr << "Could not find supernode for chunk 0\n";
            RunReport::set("error", "no supernode for chunk 0");
            return 1;
        }

        // Primed starting state: chunk 0's supernode placed, reservoir advanced through its
        // own frames. Copied (cheap) per beam-schedule attempt below.
        DfsState primed;
        primed.used.assign(supernodes.size(), false);
        primed.order.push_back(startSuper);
        primed.used[startSuper] = true;
        primed.reservoirLevel = 0;
        if (isLayerIII) {
            const ChunkMeta* prev = nullptr; // chunk 0 is always first placed - no predecessor
            bool startOk = true;
            for (int ci : supernodes[startSuper].chunks) {
                if (!advanceReservoirChunk(primed.reservoirLevel, prev, metas[ci], chunks[ci], sideInfo, maxReservoir)) {
                    startOk = false;
                    break;
                }
                prev = &metas[ci];
            }
            if (!startOk && tau > 0) {
                // A wrong lock chained an impossible successor onto chunk 0's supernode: this
                // attempt can't succeed, the next (from the unlocked graph) still can.
                cout << "Pinned start supernode fails the reservoir check (a wrong lock merged into it)"
                     << " - abandoning this lock attempt\n";
                dfs.setDeepest({startSuper});
                continue;
            }
            if (!startOk) {
                RunReport::set("error", "reservoir check failed on the pinned starting chunk");
                cerr << "Reservoir check failed on the pinned starting chunk - "
                        "mainDataBegin metadata is likely wrong for chunk 0\n";
                return 1;
            }
        }

        // Beam schedule. RP_BEAM=K forces a single fixed-width attempt. Otherwise, for graphs
        // small enough that an exhaustive DFS is known to terminate we just run it unlimited;
        // for larger ones we iteratively broaden (near-greedy first, widening on dead-end) so a
        // heuristic-guided answer comes back even when the exhaustive search never would.
        // RP_BROADEN=0 / =1 overrides the size heuristic.
        vector<int> beamSchedule;
        bool broaden = false;
        if (cfg.search.beam) {
            beamSchedule = { *cfg.search.beam };
        } else {
            broaden = cfg.search.broaden ? *cfg.search.broaden : supernodes.size() > 120;
            beamSchedule = broaden ? vector<int>{1, 2, 3, 4, 6, 9, 0} : vector<int>{0};
        }
        auto search = [&]() {
            // Wall-clock budget for the whole search. Needed whenever the search is not a plain
            // first-valid exhaustive DFS: broadening's wider attempts and best-of both keep going
            // past the first complete ordering and would otherwise not terminate on large graphs.
            // RP_TIME_BUDGET (seconds) overrides; RP_MAX_NODES (node cap) also bounds it.
            if (dfs.maxNodes() == 0 && (broaden || dfs.bestOf())) {
                dfs.setDeadline(cfg.search.timeBudget ? *cfg.search.timeBudget : broaden ? 45.0 : 30.0);
            } else if (cfg.search.timeBudget) {
                dfs.setDeadline(*cfg.search.timeBudget);
            } else if (dfs.maxNodes() == 0 && lockTaus.size() > 1) {
                // With a lock schedule, no attempt may run an unbounded exhaustive DFS: locking can
                // shrink a large graph below the broadening threshold (test7_short at tau 1.25:
                // 108 supernodes, ran >500 s), which would drop rp's "always terminates" guarantee.
                // 30 s per attempt with a fallback left, 45 s for the last one.
                dfs.setDeadline((lockAttempt + 1 < lockTaus.size()) ? 30.0 : 45.0);
            }
            if (dfs.runSchedule(supernodes, superAdj, primed, beamSchedule, result)) found = true;
        };

        // Global successor matching (RP_MATCH): 1 (default) = when this attempt's DFS fails,
        // 2 = before the DFS, 0 = off. Its ordering is used if it passes the reservoir check;
        // otherwise the DFS is rerun (fresh budget) with each matched successor ranked first.
        auto tryMatch = [&]() -> bool {
            const auto t0 = chrono::steady_clock::now();
            MatchResult mr = globalMatch(chunks, supernodes, superAdj, startSuper, adj, metas, lockMemo, cfg.threads, warmPtr,
                                         cfg.match.maxDecodes);
            // Iterated matching (RP_MATCH_ITERS rounds): the assignment ignores the bit
            // reservoir, so forbid the supernode arc at (or, for a break inside a supernode, just
            // before) each reservoir break and re-solve; keep the result with the fewest breaks,
            // then the lowest cost. Each round reuses the memoised window fluxes (~0.02-0.1 s).
            const int matchIters = cfg.match.iters;
            if (isLayerIII && matchIters > 0 && !mr.order.empty()) {
                std::unordered_set<uint64_t> forb;
                auto breaksOf = [&](const MatchResult& r, vector<int>* where) {
                    const vector<int> sq = chunksOf(supernodes, r.order);
                    return reservoirBreaks(metas, chunks, sq, sideInfo, maxReservoir, where);
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
                        for (size_t j = 0; j < supernodes[cur.order[k]].chunks.size(); ++j) superAt.push_back(k);
                    size_t before = forb.size();
                    for (int k : where) {
                        const int sp = superAt[k];
                        if (sp == 0) continue;   // inside the pinned start supernode
                        forb.insert(((uint64_t)(unsigned)cur.order[sp - 1] << 32) | (unsigned)cur.order[sp]);
                    }
                    if (forb.size() == before) break;
                    MatchResult nx = globalMatch(chunks, supernodes, superAdj, startSuper, adj, metas, lockMemo, cfg.threads, warmPtr,
                                                     cfg.match.maxDecodes, &forb);
                    if (nx.order.empty()) break;   // over-constrained: keep the best so far
                    const int b = breaksOf(nx, nullptr);
                    if (b < bestBreaks || (b == bestBreaks && nx.cost < mr.cost)) { mr = nx; bestBreaks = b; }
                    cur = std::move(nx);
                }
                mr.decodes = dec0;
                cout << "Iterated matching: " << round << " round(s), " << forb.size() << " arcs forbidden, reservoir breaks "
                     << firstBreaks << " -> " << bestBreaks << "\n";
            }
            const double secs = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
            cout << "Global matching: " << supernodes.size() << " supernodes, " << mr.decodes << " decodes ("
                 << mr.contexts << " ctx), "
                 << mr.cycles << " cycle(s), " << mr.spliced << " spliced, cost " << fixed << setprecision(3)
                 << mr.cost << ", " << setprecision(2) << secs << "s";
            RunReport::set("matching", mr.order.empty() ? mr.failure : string("ok"));
            if (mr.order.empty()) { cout << " - " << mr.failure << "\n"; }
            else {
                vector<int> seqm = chunksOf(supernodes, mr.order);
                int tj = 0;
                for (size_t k = 1; k < seqm.size(); ++k) if (origIndexOf[seqm[k]] == origIndexOf[seqm[k - 1]] + 1) ++tj;
                const bool ok = !isLayerIII || reservoirValid(metas, chunks, seqm, sideInfo, maxReservoir);
                cout << (ok ? " - reservoir OK" : " - reservoir check FAILED")
                     << " [vs source: " << tj << "/" << (seqm.size() - 1) << " true junctions]\n";
                // Repair (moves on the full structural graph). If the order is reservoir-invalid
                // - usual, it is nearly right but a few wrong junctions break the reservoir -
                // the cost is led by the number of reservoir breaks.
                if (cfg.repair.mode != Switch::Off) {
                    const auto r0 = chrono::steady_clock::now();
                    double gap = std::numeric_limits<double>::quiet_NaN();
                    const int mv = repair.run(seqm, &gap);
                    const bool ok2 = reservoirValid(metas, chunks, seqm, sideInfo, maxReservoir);
                    int tj2 = 0;
                    for (size_t k = 1; k < seqm.size(); ++k) if (origIndexOf[seqm[k]] == origIndexOf[seqm[k - 1]] + 1) ++tj2;
 (void)ok;
                    cout << "Matching repair: " << mv << " move(s), " << fixed << setprecision(2)
                         << chrono::duration<double>(chrono::steady_clock::now() - r0).count() << "s - "
                         << (ok2 ? "reservoir OK" : "still invalid") << " [vs source: " << tj2 << "/"
                         << (seqm.size() - 1) << " true junctions]\n";
                    if (ok2) { result = mr.order; matchSeq = seqm; resultGap = gap; resultRepairMoves = mv; foundByMatch = true; return true; }
                } else if (ok) { result = mr.order; matchSeq = seqm; foundByMatch = true; return true; }
            }
            if (mr.succ.empty()) return false;
            dfs.setMatchHint(mr.succ);
            dfs.clearDeadline();
            cout << "Search with the matching as ranking hint:\n";
            search();
            dfs.clearMatchHint();
            if (found) foundByHint = true;   // a DFS result: normal repair policy, own label
            return found;
        };
        int matchMode = cfg.match.mode;
        if (!scorer.pcmOn()) matchMode = 0;
        if (matchMode == 2 && tryMatch()) { found = true; continue; }
        search();
        if (!found && matchMode == 1 && tryMatch()) found = true;
    } // lock attempts

    if (found) {
        cout << "Reconstruction found" << (dfs.bestOf() ? (" (score " + std::to_string(dfs.bestScore()) + ")") : "") << "\n";
        ofstream out("output.mp3", ios::binary | ios::trunc);
        if (!out) { cerr << "Cannot open output.mp3\n"; RunReport::set("error", "cannot open output.mp3"); return 1; }

        vector<int> seq; // reconstructed chunk order
        if (foundByMatch && !matchSeq.empty()) seq = matchSeq;
        else seq = chunksOf(supernodes, result);

        // Repair policy. Before the minimum-gain rule, an ordering found by the FIRST (strictest)
        // lock attempt was left alone: on one first-attempt success (cbr32_10s, 1 KB) the
        // repair moved the exact order to a lower-flux wrong one. With the reservoir arc filter
        // the strictest attempt now often finds an order at once, and some are a block off
        // (test7_short: 606/609, fixed by a 3.4-gain move). So the repair now always runs with
        // the RP_REPAIR_MIN_GAIN threshold; with RP_REPAIR_MIN_GAIN=0 the old skip applies.
        // RP_REPAIR=1 forces the repair, =0 disables it.
        const bool repairForced = cfg.repair.mode == Switch::On;
        const bool repairOff    = cfg.repair.mode == Switch::Off;
        const bool firstOfSchedule = lockTaus.size() > 1 && foundAttempt == 0 && !foundByMatch && cfg.repair.minGain <= 0;
        if (foundByMatch && !repairForced)
            cout << "Block-move repair: skipped (the matching's ordering was already repaired)\n";
        else if (scorer.pcmOn() && !repairOff && firstOfSchedule && !repairForced)
            cout << "Block-move repair: skipped (found by the strictest lock attempt; RP_REPAIR=1 forces)\n";
        else if (scorer.pcmOn() && !repairOff) {
            const auto t0 = chrono::steady_clock::now();
            const int moves = repair.run(seq, &resultGap);
            resultRepairMoves += moves;
            cout << "Block-move repair: " << moves << " move(s), " << fixed << setprecision(2)
                 << chrono::duration<double>(chrono::steady_clock::now() - t0).count() << "s\n";
        }

        vector<int> reconOrig; // original index of each chunk, in reconstructed order
        reconOrig.reserve(chunks.size());
        vector<int> reconFull; // full-stream index of each written chunk, slots included
        size_t total = 0;
        size_t nextPool = 0;
        int slots = 0, slotsTrue = 0;
        // Stage B (PLAN_headerless_chunks.md): assign the pool to the slots after the demanders
        // by PCM flux (SlotFill). RP_SLOT_FILL=0: pool order (stage A behaviour).
        vector<int> slotPool;   // per slot, in order: pool index, -1 none
        vector<char> poolUsed(pool.size(), 0);
        if (!pool.empty() && cfg.slotFill) {
            vector<SlotFill::Slot> sl;
            for (size_t k = 0; k + 1 < seq.size(); ++k) {
                if (!metas[seq[k]].gapAfter) continue;
                SlotFill::Slot s;
                size_t from = k;   // left side: back to pcmLeft chunks, not across another slot
                while (from > 0 && k - from + 1 < static_cast<size_t>(scorer.pcmLeft()) && !metas[seq[from - 1]].gapAfter) --from;
                s.left.assign(seq.begin() + from, seq.begin() + k + 1);
                s.b = seq[k + 1];
                if (k + 2 < seq.size() && !metas[s.b].gapAfter) s.after = seq[k + 2];
                sl.push_back(s);
            }
            SlotFill::Stats st;
            slotPool = SlotFill::assign(chunks, metas, sl, pool, chunkSize, lockThreads, &st);
            cout << "Slot assignment: " << sl.size() << " slots x " << pool.size() << " headerless chunks, "
                 << st.decodes << " decodes, " << st.excluded << " pairs excluded by header/CRC, "
                 << st.unmeasured << " unmeasured, " << st.forced << " forced, " << fixed << setprecision(2)
                 << st.seconds << "s\n";
            RunReport::set("slot_decodes", static_cast<long long>(st.decodes));
            RunReport::set("slots_forced", st.forced);
        }
        size_t slotIdx = 0;
        auto writeChunk = [&](const vector<uint8_t>& c) {
            out.write(reinterpret_cast<const char*>(c.data()), static_cast<long>(c.size()));
            total += c.size();
        };
        for (size_t k = 0; k < seq.size(); ++k) {
            const int ci = seq[k];
            writeChunk(chunks[ci]);
            reconOrig.push_back(origIndexOf[ci]);
            reconFull.push_back(keptFull.empty() ? origIndexOf[ci] : keptFull[origIndexOf[ci]]);
            // A demander is followed by a slot. Stage A fills slots in pool order; the
            // assignment by flux is stage B (PLAN_headerless_chunks.md).
            if (metas[ci].gapAfter && k + 1 < seq.size()) {
                ++slots;
                int pi = -1;
                if (!slotPool.empty()) pi = slotIdx < slotPool.size() ? slotPool[slotIdx] : -1;
                else {   // pool order
                    while (nextPool < pool.size() && poolUsed[nextPool]) ++nextPool;
                    if (nextPool < pool.size()) pi = static_cast<int>(nextPool);
                }
                ++slotIdx;
                if (pi >= 0) {
                    poolUsed[pi] = 1;
                    writeChunk(pool[pi]);
                    reconFull.push_back(poolFull[pi]);
                    if (poolFull[pi] == reconFull[reconFull.size() - 2] + 1) ++slotsTrue;
                }
            }
        }
        for (size_t pi = 0; pi < pool.size(); ++pi) {   // pool chunks no slot took: keep the bytes
            if (poolUsed[pi]) continue;
            writeChunk(pool[pi]);
            reconFull.push_back(poolFull[pi]);
        }
        cout << "Written " << total << " bytes to output.mp3\n";
        if (!pool.empty())
            cout << "Slots: " << slots << " for " << pool.size() << " headerless chunks, " << slotsTrue
                 << " filled with the true chunk" << (slotPool.empty() ? " (pool order)" : "") << "\n";
        if (!pool.empty()) { RunReport::set("slots", slots); RunReport::set("slots_true", slotsTrue); }
        // Provenance label (a line of its own - regression.sh/parse.py grep the lines around it).
        // A global-matching ordering is the min-cost assignment patched + repaired: reservoir-
        // valid, but nothing proves it is the original order, and on some files a wrong order is
        // cheaper than the true one (Queen 4 KB, cbr192_10s 2 KB). The gap is the summed-flux
        // margin to the nearest valid ordering one repair move away (small = ambiguous).
        // "+ N repair move(s)": the order was changed by flux-driven repair moves after it was
        // found. Neither label means "verified": in the 2026-10-18 benchmark 5 wrong outputs
        // came from the DFS (2 of them repaired, 3 wrong as found) and 11 from the matching.
        cout << "Result provenance: " << (slotOracle ? "slot oracle (true order of the chunks with a header)"
                                          : foundByMatch ? "best-effort (global matching + repair)"
                                          : foundByHint ? "DFS search (matching-ranked)" : "DFS search");
        if (resultRepairMoves > 0) cout << " + " << resultRepairMoves << " repair move(s)";
        if (!std::isnan(resultGap)) {
            if (std::isfinite(resultGap)) cout << ", stability gap " << fixed << setprecision(3) << resultGap;
            else cout << ", stability gap inf";
        }
        cout << "\n";

        // C2: score the reconstruction against the true (pre-shuffle) order.
        int exactPos = 0;
        for (size_t i = 0; i < reconOrig.size(); ++i)
            if (reconOrig[i] == static_cast<int>(i)) ++exactPos;
        int trueJ = 0; // chunk-level junctions whose successor is the true next chunk
        for (size_t i = 1; i < reconOrig.size(); ++i) if (reconOrig[i] == reconOrig[i - 1] + 1) ++trueJ;
        // With headerless chunks taken out, the lines below score the order of the chunks the
        // search placed; the output is only exact when the slots got the true chunks as well.
        const bool exactMatch = (reconOrig.size() == chunks.size()) &&
                                std::is_sorted(reconOrig.begin(), reconOrig.end()) &&
                                (pool.empty() || (reconFull.size() == chunks.size() + pool.size() &&
                                                  std::is_sorted(reconFull.begin(), reconFull.end())));
        if (!pool.empty()) {
            int fullPos = 0;
            for (size_t i = 0; i < reconFull.size(); ++i) if (reconFull[i] == static_cast<int>(i)) ++fullPos;
            cout << "Full stream (slots included): " << fullPos << "/" << reconFull.size()
                 << " chunks in original position\n";
        }
        cout << "Reconstruction vs source: " << exactPos << "/" << reconOrig.size()
             << " chunks in original position ("
             << fixed << setprecision(1) << (100.0 * exactPos / std::max<size_t>(1, reconOrig.size())) << "%)"
             << (exactMatch ? "  --  EXACT (output.mp3 is byte-identical to the source stream)" : "")
             << "\n";
        RunReport::set("status", "complete");
        RunReport::set("exact", exactMatch);
        RunReport::set("exact_pct", 100.0 * exactPos / std::max<size_t>(1, reconOrig.size()));
        RunReport::set("true_junctions", trueJ);
        RunReport::set("junctions", reconOrig.empty() ? size_t(0) : reconOrig.size() - 1);
        RunReport::set("provenance", slotOracle ? "slot_oracle" : foundByMatch ? "matching" : foundByHint ? "dfs_matching_ranked" : "dfs");
        RunReport::set("repair_moves", resultRepairMoves);
        cout << "Reconstruction junctions: " << trueJ << "/" << (reconOrig.empty() ? 0 : reconOrig.size() - 1)
             << " are true successor pairs\n";
        // Diagnostic (RP_SCORE_ORDER=1): summed PCM flux of the found ordering vs the source
        // order, each junction scored in its own exact (PP, P, cur) context. Tells whether a
        // wrong-but-valid result is something a flux-minimising repair could fix.
        if (cfg.scoreOrder && !exactMatch) {
            vector<int> found = seq, truth(chunks.size());
            for (size_t ci = 0; ci < chunks.size(); ++ci) truth[origIndexOf[ci]] = static_cast<int>(ci);
            for (size_t k = 1; k < found.size(); ++k)
                if (origIndexOf[found[k]] != origIndexOf[found[k - 1]] + 1)
                    cout << "  wrong junction at position " << k << ": orig " << origIndexOf[found[k - 1]]
                         << " -> " << origIndexOf[found[k]] << "\n";
            const FluxSum fs = scorer.summedFlux(found), ts = scorer.summedFlux(truth);
            const int uf = fs.unmeasured, ut = ts.unmeasured;
            const double sf = fs.sum, st = ts.sum;
            cout << "Summed log1p(flux): found " << fixed << setprecision(3) << sf << " (" << uf
                 << " unmeasured), source " << st << " (" << ut << " unmeasured) -> "
                 << (st < sf ? "source is cheaper" : "found is cheaper or equal") << "\n";
        }
    } else {
        cout << "Failed to reconstruct (no complete ordering found"
             << (dfs.aborted() ? " within the search budget" : "") << ").\n";
        // How right was the deepest partial path? Chunk-level junctions whose successor is the
        // true next chunk, and where the first wrong one is.
        keepDeepest();
        vector<int> path;
        for (int ci : bestPartial) path.push_back(origIndexOf[ci]);
        int good = 0, firstBad = -1;
        for (size_t k = 1; k < path.size(); ++k) {
            if (path[k] == path[k - 1] + 1) ++good;
            else if (firstBad < 0) firstBad = static_cast<int>(k);
        }
        RunReport::set("status", "no_ordering");
        RunReport::set("budget_exhausted", dfs.aborted());
        RunReport::set("partial_chunks", path.size());
        RunReport::set("partial_true_junctions", good);
        cout << "Deepest partial path: " << bestPartialSupers << "/" << bestPartialOf << " supernodes, "
             << path.size() << " chunks; true junctions " << good << "/" << (path.empty() ? 0 : path.size() - 1)
             << "; correct prefix = " << (firstBad < 0 ? (int)path.size() : firstBad) << " chunks\n";
    }

    return 0;
}
