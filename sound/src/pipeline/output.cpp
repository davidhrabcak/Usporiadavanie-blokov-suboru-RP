#include "pipeline/output.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include "run_report.hpp"

using namespace std;

namespace {

// Provenance label. A global-matching ordering is the min-cost assignment patched + repaired:
// reservoir-valid, but nothing proves it is the original order, and on some files a wrong order
// is cheaper than the true one. The gap is the summed-flux margin to the nearest valid ordering
// one repair move away (small = ambiguous). "+ N repair move(s)": the order was changed by
// flux-driven repair moves after it was found. Neither label means "verified": wrong outputs
// came from both the DFS and the matching.
void printProvenance(const Reconstruction& r) {
    using Source = Reconstruction::Source;
    cout << "Result provenance: " << (r.source == Source::Matching ? "best-effort (global matching + repair)"
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
    return s == Source::Matching ? "matching" : s == Source::DfsMatchRanked ? "dfs_matching_ranked" : "dfs";
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
    const vector<int>& seq = r.seq;

    ofstream out("output.mp3", ios::binary | ios::trunc);
    if (!out) { cerr << "Cannot open output.mp3\n"; RunReport::set("error", "cannot open output.mp3"); return 1; }
    size_t total = 0;
    vector<int> reconOrig; // source index of each chunk, in reconstructed order
    reconOrig.reserve(seq.size());
    for (const int ci : seq) {
        out.write(reinterpret_cast<const char*>(p.chunks[ci].data()), static_cast<long>(p.chunks[ci].size()));
        total += p.chunks[ci].size();
        reconOrig.push_back(p.origIndexOf[ci]);
    }
    cout << "Written " << total << " bytes to output.mp3\n";
    printProvenance(r);

    // Score the reconstruction against the source order.
    int exactPos = 0, trueJ = 0;
    for (size_t i = 0; i < reconOrig.size(); ++i) {
        if (reconOrig[i] == static_cast<int>(i)) ++exactPos;
        if (i > 0 && reconOrig[i] == reconOrig[i - 1] + 1) ++trueJ;
    }
    const bool exactMatch = reconOrig.size() == p.chunks.size() && exactPos == static_cast<int>(reconOrig.size());
    cout << "Reconstruction vs source: " << exactPos << "/" << reconOrig.size()
         << " chunks in original position ("
         << fixed << setprecision(1) << (100.0 * exactPos / std::max<size_t>(1, reconOrig.size())) << "%)"
         << (exactMatch ? "  --  EXACT (output.mp3 is byte-identical to the source stream)" : "")
         << "\n";
    cout << "Reconstruction junctions: " << trueJ << "/" << (reconOrig.empty() ? 0 : reconOrig.size() - 1)
         << " are true successor pairs\n";
    // The report carries the order itself; test/corpus.py derives every metric from it.
    RunReport::set("status", "complete");
    RunReport::set("n", p.chunks.size());
    RunReport::set("order", reconOrig);
    RunReport::set("budget_exhausted", r.budgetExhausted);
    RunReport::set("provenance", provenanceKey(r.source));
    RunReport::set("repair_moves", r.repairMoves);
    if (cfg.scoreOrder && !exactMatch) scoreOrder(p, scorer, seq);
    return 0;
}

void reportFailure(const Problem& p, const Reconstruction& r) {
    cout << "Failed to reconstruct (no complete ordering found"
         << (r.budgetExhausted ? " within the search budget" : "") << ").\n";
    // How right was the deepest partial path? Chunk-level junctions whose successor is the
    // true next chunk, and where the first wrong one is.
    vector<int> path;   // source index of each chunk
    for (int ci : r.partial) path.push_back(p.origIndexOf[ci]);
    int good = 0, firstBad = -1;
    for (size_t k = 1; k < path.size(); ++k) {
        if (path[k] == path[k - 1] + 1) ++good;
        else if (firstBad < 0) firstBad = static_cast<int>(k);
    }
    RunReport::set("status", "no_ordering");
    RunReport::set("budget_exhausted", r.budgetExhausted);
    RunReport::set("n", p.chunks.size());
    RunReport::set("order", path);   // the deepest partial path
    cout << "Deepest partial path: " << r.partialSupers << "/" << r.partialOf << " supernodes, "
         << path.size() << " chunks; true junctions " << good << "/" << (path.empty() ? 0 : path.size() - 1)
         << "; correct prefix = " << (firstBad < 0 ? (int)path.size() : firstBad) << " chunks\n";
}
