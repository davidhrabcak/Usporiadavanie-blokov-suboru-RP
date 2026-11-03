#ifndef RP_REPAIR_HPP
#define RP_REPAIR_HPP

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include "config.hpp"
#include "mp3/chunk_meta.hpp"
#include "scoring/junction_scorer.hpp"

// Block-move repair (PLAN_full_pcm_decode.md step 6). The DFS returns the FIRST valid ordering;
// when locking/ranking took one wrong turn it can be complete-but-wrong with a whole block of
// the song in the wrong place (A-C-B instead of A-B-C). Summed PCM flux points at the true order
// (2-opt test: 97-100% of swaps), so: repeatedly move a contiguous block of chunks to another
// position when (1) all new junctions are structural edges of the FULL graph (before
// AC-3/locking - so a wrong lock can be undone), (2) the whole ordering still passes the
// bit-reservoir check, and (3) the cost strictly decreases. Chunk 0 stays first.
// Best-improvement per pass until no move helps.
//
// Two move types, each enumerated only where every new junction is a structural edge:
//   block move: take seq[i..j] out and reinsert it after position k (3 new junctions);
//   block swap: exchange two disjoint blocks seq[i1..j1] and seq[i2..j2] (4 new junctions) -
//               needed for the A-X-Y-B -> A-B-Y-X shape, where every single block move on the
//               way is structurally invalid.
//
// Lexicographic cost: (reservoir breaks, junctions the PCM window can't measure, summed
// log1p(flux) over the rest). Breaks lead only when the order starts out invalid (from the
// global matching): it is repaired towards validity first. Unmeasurable before the sum, so a
// move can't "win" by turning a high-flux junction into an unscorable one (seen on test7_short
// when unmeasurable junctions used a neutral value). On a reservoir-valid order a flux-only
// move must lower the sum by at least RP_REPAIR_MIN_GAIN: small gains are noise (a 0.037 move
// turned a true order into a cheaper wrong one, cbr192_10s 2 KB; the move that fixed
// test7_short gained 3.4).
class OrderRepair {
public:
    // `origIndexOf` (diagnostic, may be null): source position of each chunk, for the
    // "[vs source: ...]" log lines.
    OrderRepair(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
                JunctionScorer& scorer, const std::vector<std::vector<int>>& adjFull,
                const StreamProfile& profile, const Config::Repair& opt, const std::vector<int>* origIndexOf);

    // Repairs `seq` in place and returns the number of moves applied. `gap` (optional) gets the
    // stability margin of the final order: how much the summed flux would rise under the
    // cheapest valid move the last (full-scan) pass rejected - NaN when no full scan completed.
    // A small gap means a nearby, almost equally cheap ordering exists (the wrong-output risk).
    int run(std::vector<int>& seq, double* gap = nullptr);

private:
    struct Cost {
        int breaks = 0;      // reservoir-check failures (only tracked when repairing an invalid order)
        int unmeasured = 0;
        double sum = 0;
        bool operator<(const Cost& o) const {
            if (breaks != o.breaks) return breaks < o.breaks;
            return unmeasured != o.unmeasured ? unmeasured < o.unmeasured : sum < o.sum - 1e-9;
        }
    };
    // A candidate order: a list of [from, to] segments of the current order.
    using Seg = std::pair<int, int>;
    struct Move { Cost cost; std::vector<Seg> segs; std::string what; };

    // Per-junction flux, reservoir levels and their prefix sums for the current order. A
    // candidate is then evaluated incrementally: a junction's flux depends on its
    // (pcmLeft + 1)-chunk window, so only the first pcmLeft positions of each segment need a
    // pcmRaw lookup (the rest come from the prefix sums), and the reservoir state
    // re-synchronises with the current order's within a frame or two, after which the
    // segment's remaining breaks come from prefix sums too.
    void prepare();
    int evalBreaks(const std::vector<Seg>& segs) const;
    void evalFlux(const std::vector<Seg>& segs, Cost& c);
    std::vector<int> build(const std::vector<Seg>& segs) const;
    // Suspect filter: a move can only fix the ordering by cutting a wrong junction, and wrong
    // splices are usually high-flux (pcm_probe: true median ~1.05, wrong ~3). Marks the K worst
    // junctions (plus, for an invalid order, the junctions just before each reservoir break).
    std::vector<char> suspectJunctions() const;
    // Evaluates one candidate; keeps it in `moves` if it beats the current cost.
    void consider(std::vector<Seg>&& segs, std::string&& what, std::vector<Move>& moves, long& candidates);
    // Every structurally valid block move / block swap that cuts a junction marked in `bad`.
    void scanMoves(const std::vector<int>& pos, const std::vector<char>& bad,
                   std::vector<Move>& moves, long& candidates);
    bool isEdge(int a, int b) const;
    int trueJunctions(const std::vector<int>& seq) const;

    const std::vector<std::vector<uint8_t>>& chunks_;
    const std::vector<ChunkMeta>& metas_;
    JunctionScorer& scorer_;
    const std::vector<std::vector<int>>& adjFull_;
    const bool isLayerIII_;
    const int sideInfo_;
    const int maxReservoir_;
    const Config::Repair opt_;
    const std::vector<int>* origIndexOf_;
    std::unordered_set<uint64_t> edge_;
    std::vector<std::vector<int>> preds_;

    // state of one run()
    std::vector<int>* seq_ = nullptr;
    int n_ = 0;
    Cost cur_;
    bool trackBreaks_ = false;
    std::chrono::steady_clock::time_point deadline_;
    bool outOfTime_ = false;
    double nearest_ = 0;          // cheapest rejected move (sum delta)
    std::string nearestWhat_;
    std::vector<double> jv_;      // junction value at k (k >= 1), -1 = unmeasured
    std::vector<double> PS_;      // prefix sums of measured jv
    std::vector<int> PU_;         // prefix counts of unmeasured
    std::vector<int> lvlAfter_;   // reservoir level after chunk k (break -> reset to full)
    std::vector<int> PB_;         // prefix reservoir-break counts
};

#endif
