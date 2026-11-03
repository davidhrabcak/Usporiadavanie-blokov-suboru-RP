#include "pipeline/output.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include "graph/slot_fill.hpp"
#include "run_report.hpp"

using namespace std;

namespace {

// Stage B (PLAN_headerless_chunks.md): assign the pool to the slots after the demanders by PCM
// flux (SlotFill). Returns the pool index per slot, in order (-1 none).
vector<int> assignSlots(const Problem& p, const JunctionScorer& scorer, const vector<int>& seq, const int threads) {
    const vector<ChunkMeta>& metas = p.metas;
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
    vector<int> slotPool = SlotFill::assign(p.chunks, metas, sl, p.pool, p.chunkSize, threads, &st);
    cout << "Slot assignment: " << sl.size() << " slots x " << p.pool.size() << " headerless chunks, "
         << st.decodes << " decodes, " << st.excluded << " pairs excluded by header/CRC, "
         << st.unmeasured << " unmeasured, " << st.forced << " forced, " << fixed << setprecision(2)
         << st.seconds << "s\n";
    RunReport::set("slot_decodes", static_cast<long long>(st.decodes));
    RunReport::set("slots_forced", st.forced);
    return slotPool;
}

// Provenance label (a line of its own - regression.sh/parse.py grep the lines around it).
// A global-matching ordering is the min-cost assignment patched + repaired: reservoir-valid, but
// nothing proves it is the original order, and on some files a wrong order is cheaper than the
// true one (Queen 4 KB, cbr192_10s 2 KB). The gap is the summed-flux margin to the nearest valid
// ordering one repair move away (small = ambiguous). "+ N repair move(s)": the order was changed
// by flux-driven repair moves after it was found. Neither label means "verified": in the
// 2026-10-18 benchmark 5 wrong outputs came from the DFS (2 of them repaired, 3 wrong as found)
// and 11 from the matching.
void printProvenance(const Reconstruction& r) {
    using Source = Reconstruction::Source;
    cout << "Result provenance: " << (r.source == Source::SlotOracle ? "slot oracle (true order of the chunks with a header)"
                                      : r.source == Source::Matching ? "best-effort (global matching + repair)"
                                      : r.source == Source::DfsMatchRanked ? "DFS search (matching-ranked)" : "DFS search");
    if (r.repairMoves > 0) cout << " + " << r.repairMoves << " repair move(s)";
    if (!std::isnan(r.gap)) {
        if (std::isfinite(r.gap)) cout << ", stability gap " << fixed << setprecision(3) << r.gap;
        else cout << ", stability gap inf";
    }
    cout << "\n";
}

const char* provenanceKey(const Reconstruction::Source s) {
    using Source = Reconstruction::Source;
    return s == Source::SlotOracle ? "slot_oracle" : s == Source::Matching ? "matching"
         : s == Source::DfsMatchRanked ? "dfs_matching_ranked" : "dfs";
}

// Diagnostic (RP_SCORE_ORDER=1): summed PCM flux of the found ordering vs the source order, each
// junction scored in its own exact (PP, P, cur) context. Tells whether a wrong-but-valid result
// is something a flux-minimising repair could fix.
void scoreOrder(const Problem& p, JunctionScorer& scorer, const vector<int>& seq) {
    const vector<int>& origIndexOf = p.origIndexOf;
    vector<int> found = seq, truth(p.chunks.size());
    for (size_t ci = 0; ci < p.chunks.size(); ++ci) truth[origIndexOf[ci]] = static_cast<int>(ci);
    for (size_t k = 1; k < found.size(); ++k)
        if (origIndexOf[found[k]] != origIndexOf[found[k - 1]] + 1)
            cout << "  wrong junction at position " << k << ": orig " << origIndexOf[found[k - 1]]
                 << " -> " << origIndexOf[found[k]] << "\n";
    const FluxSum fs = scorer.summedFlux(found), ts = scorer.summedFlux(truth);
    cout << "Summed log1p(flux): found " << fixed << setprecision(3) << fs.sum << " (" << fs.unmeasured
         << " unmeasured), source " << ts.sum << " (" << ts.unmeasured << " unmeasured) -> "
         << (ts.sum < fs.sum ? "source is cheaper" : "found is cheaper or equal") << "\n";
}

} // namespace

int writeOutput(const Problem& p, JunctionScorer& scorer, const Reconstruction& r, const Config& cfg) {
    const vector<vector<uint8_t>>& chunks = p.chunks;
    const vector<ChunkMeta>& metas = p.metas;
    const vector<int>& origIndexOf = p.origIndexOf;
    const vector<vector<uint8_t>>& pool = p.pool;
    const vector<int>& seq = r.seq;

    ofstream out("output.mp3", ios::binary | ios::trunc);
    if (!out) { cerr << "Cannot open output.mp3\n"; RunReport::set("error", "cannot open output.mp3"); return 1; }

    vector<int> reconOrig; // original index of each chunk, in reconstructed order
    reconOrig.reserve(chunks.size());
    vector<int> reconFull; // full-stream index of each written chunk, slots included
    size_t total = 0;
    size_t nextPool = 0;
    int slots = 0, slotsTrue = 0;
    vector<int> slotPool;   // per slot, in order: pool index, -1 none (empty: pool order, RP_SLOT_FILL=0)
    vector<char> poolUsed(pool.size(), 0);
    if (!pool.empty() && cfg.slotFill) slotPool = assignSlots(p, scorer, seq, cfg.threads);
    size_t slotIdx = 0;
    auto writeChunk = [&](const vector<uint8_t>& c) {
        out.write(reinterpret_cast<const char*>(c.data()), static_cast<long>(c.size()));
        total += c.size();
    };
    for (size_t k = 0; k < seq.size(); ++k) {
        const int ci = seq[k];
        writeChunk(chunks[ci]);
        reconOrig.push_back(origIndexOf[ci]);
        reconFull.push_back(p.keptFull.empty() ? origIndexOf[ci] : p.keptFull[origIndexOf[ci]]);
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
                reconFull.push_back(p.poolFull[pi]);
                if (p.poolFull[pi] == reconFull[reconFull.size() - 2] + 1) ++slotsTrue;
            }
        }
    }
    for (size_t pi = 0; pi < pool.size(); ++pi) {   // pool chunks no slot took: keep the bytes
        if (poolUsed[pi]) continue;
        writeChunk(pool[pi]);
        reconFull.push_back(p.poolFull[pi]);
    }
    cout << "Written " << total << " bytes to output.mp3\n";
    if (!pool.empty())
        cout << "Slots: " << slots << " for " << pool.size() << " headerless chunks, " << slotsTrue
             << " filled with the true chunk" << (slotPool.empty() ? " (pool order)" : "") << "\n";
    if (!pool.empty()) { RunReport::set("slots", slots); RunReport::set("slots_true", slotsTrue); }
    printProvenance(r);

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
    RunReport::set("provenance", provenanceKey(r.source));
    RunReport::set("repair_moves", r.repairMoves);
    cout << "Reconstruction junctions: " << trueJ << "/" << (reconOrig.empty() ? 0 : reconOrig.size() - 1)
         << " are true successor pairs\n";
    if (cfg.scoreOrder && !exactMatch) scoreOrder(p, scorer, seq);
    return 0;
}

void reportFailure(const Problem& p, const Reconstruction& r) {
    cout << "Failed to reconstruct (no complete ordering found"
         << (r.budgetExhausted ? " within the search budget" : "") << ").\n";
    // How right was the deepest partial path? Chunk-level junctions whose successor is the
    // true next chunk, and where the first wrong one is.
    vector<int> path;
    for (int ci : r.partial) path.push_back(p.origIndexOf[ci]);
    int good = 0, firstBad = -1;
    for (size_t k = 1; k < path.size(); ++k) {
        if (path[k] == path[k - 1] + 1) ++good;
        else if (firstBad < 0) firstBad = static_cast<int>(k);
    }
    RunReport::set("status", "no_ordering");
    RunReport::set("budget_exhausted", r.budgetExhausted);
    RunReport::set("partial_chunks", path.size());
    RunReport::set("partial_true_junctions", good);
    cout << "Deepest partial path: " << r.partialSupers << "/" << r.partialOf << " supernodes, "
         << path.size() << " chunks; true junctions " << good << "/" << (path.empty() ? 0 : path.size() - 1)
         << "; correct prefix = " << (firstBad < 0 ? (int)path.size() : firstBad) << " chunks\n";
}
