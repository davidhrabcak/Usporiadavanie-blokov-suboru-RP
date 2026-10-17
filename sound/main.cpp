#include <random>
#include <algorithm>
#include <csetjmp>
#include <chrono>
#include <array>
#include <queue>
#include <thread>
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
#include "frame_scanner.hpp"
#include "chunk_meta.hpp"
#include "adjacency.hpp"
#include "main_data_decoder.hpp"
#include "pcm_junction.hpp"
#include "edge_lock.hpp"

#define CHUNK_SIZE 1024
#define COUNTER_FREQUENCY 9999999
#define FILENAME "test/test7_shorter.mp3"

using namespace std;

// --------------------------------------------------------------------------
// Bit-reservoir validator (Layer III only)
// --------------------------------------------------------------------------

vector<vector<uint8_t>> chunks; // moved for debugging

/**
 * Advances the Layer III bit-reservoir level through a single frame and checks that
 * the frame's mainDataBegin is actually satisfiable by what has accumulated so far.
 *
 * `level` is tracked in BITS, not bytes (see below for why), even though callers treat it
 * as an opaque running total.
 *
 * level_after = clamp(f.mainDataBegin*8 + producedBits - f.part23Bits, 0, maxReservoir*8).
 * Cross-checked against minimp3's L3_restore_reservoir/L3_save_reservoir: the "history"
 * contribution to level_after is `f.mainDataBegin`, NOT `level_before` - a frame only ever
 * reaches back exactly as far as its own mainDataBegin declares, so any reservoir surplus
 * beyond that is discarded, not carried forward. The separate check `f.mainDataBegin >
 * level_before` still applies: mainDataBegin(N) can never exceed what was genuinely
 * available after frame N-1.
 *
 * part2_3_length/producedBits are kept in BITS throughout, only compared against
 * mainDataBegin (byte-granular) at the byte level - rounding part2_3_length up to bytes
 * per frame would add ~7 bits of phantom consumption per frame, compounding into false
 * rejections over a whole file.
 *
 * @param level  Current reservoir fill level in BITS; updated in place.
 * @param f      The frame to advance through.
 * @param sideInfo Side-information size in bytes for this stream (see sideInfoSize).
 * @param maxReservoir Reservoir capacity in bytes (511 for MPEG1, 255 for MPEG2/2.5).
 * @return False if `f.mainDataBegin` reaches further back than `level` allows, or if this
 *         frame's actual consumption (`part23Bits`) exceeds what became available.
 */
static bool advanceReservoirFrame(int& level, const FrameSlice& f, const int sideInfo, const int maxReservoir) {
    if (f.mainDataBegin > level / 8) return false; // mainDataBegin is byte-granular

    int producedBits = (f.length - 4 - sideInfo) * 8;
    if (producedBits < 0) producedBits = 0;

    int newLevel = f.mainDataBegin * 8 + producedBits - f.part23Bits;
    if (newLevel < 0) return false;
    const int maxBits = maxReservoir * 8;
    if (newLevel > maxBits) newLevel = maxBits;
    level = newLevel;
    return true;
}

/**
 * Advances the reservoir through every frame of a single chunk, in order. Used to
 * incrementally validate/extend the reservoir as each supernode is placed during the DFS,
 * rather than only checking the fully-assembled order at the end (see dfs()).
 *
 * `meta.frames` only holds *complete* frames (fully contained in this chunk) - a frame that
 * straddles a chunk boundary (routine, since chunk_size=1024 rarely divides evenly into
 * frame lengths of ~100-800 bytes) contributes no FrameSlice to *either* chunk on its own.
 *
 * When `prevMeta` is a Case-1 split (`prevMeta->tailPartialLen > 0`), the split frame's own
 * side info is decoded exactly from `prevMeta`'s tail bytes + `metaBytes`'s head
 * (`decodeSplitFrameSideInfo`, chunk_meta.cpp) and validated/consumed via `advanceReservoirFrame`
 * like any other frame - possible here because the DFS already knows both chunks of the
 * candidate edge being tried. If the exact decode isn't possible (not enough bytes to complete
 * the side-info block), this falls back to crediting `meta.headOffset` raw payload bytes without
 * validating the split frame's own mainDataBegin/part2_3_length.
 *
 * @param level     Current reservoir fill level in BITS; updated in place, even on failure
 *                  (the caller must restore it from a saved copy rather than trust it after false).
 * @param prevMeta  Metadata for the chunk immediately preceding `meta` in the candidate order,
 *                  or nullptr if `meta` is the first chunk placed (chunk 0).
 * @param meta      Metadata for the chunk being appended to the candidate order.
 * @param metaBytes Raw bytes of `meta`'s own chunk (needed to decode `prevMeta`'s split-frame
 *                  side info, which may extend past `prevMeta`'s own bytes).
 * @param sideInfo  Side-information size in bytes for this stream.
 * @param maxReservoir Reservoir capacity in bytes.
 * @return False as soon as any complete frame (or the exactly-decoded split frame) is invalid;
 *         true otherwise.
 */
static bool advanceReservoirChunk(int& level,
                                   const ChunkMeta* prevMeta,
                                   const ChunkMeta& meta,
                                   const vector<uint8_t>& metaBytes,
                                   const int sideInfo,
                                   const int maxReservoir) {
    const int maxBits = maxReservoir * 8;
    bool creditedExactly = false;

    if (prevMeta != nullptr && prevMeta->tailPartialLen > 0) {
        const SideInfoResult side = decodeSplitFrameSideInfo(*prevMeta, metaBytes);
        if (side.valid) {
            const FrameSlice splitFrame{-1, prevMeta->tailPartialLen, 0, side.mainDataBegin, side.part23Bits};
            if (!advanceReservoirFrame(level, splitFrame, sideInfo, maxReservoir)) return false;
            creditedExactly = true;
        }
    }

    if (!creditedExactly && meta.headOffset > 0) {
        level += meta.headOffset * 8;
        if (level > maxBits) level = maxBits;
    }

    for (const FrameSlice& f : meta.frames) {
        if (!advanceReservoirFrame(level, f, sideInfo, maxReservoir)) return false;
    }

    return true;
}

// --------------------------------------------------------------------------
// Greedy forced-chain collapse (Phase 3)
// --------------------------------------------------------------------------

/** A supernode is a maximal forced chain: every interior node has out-degree 1
 * and its successor has in-degree 1.
 */
struct Supernode {
    vector<int> chunks;   // chunk indices in order
};

/**
 * Build supernodes and the adjacency list over them.
 * @param adj Adjacency list of the input graph.
 * @param indeg List of degrees of only incoming edges for each vertex.
 * @param outdeg List of degrees of only outgoing edges for each vertex.
 * @param n Number of vertices in the graph.
 * @param start The index of the first chunk (pinned, never shuffled).
 * @param superAdj Output adjacency list.
 * @return List of created supernodes.
 */
vector<Supernode> buildSupernodes(
        const vector<vector<int>>& adj,
        const vector<int>& indeg,
        const vector<int>& outdeg,
        const int n,
        const int start,
        vector<vector<int>>& superAdj) {

    vector<int> chunkToSuper(n, -1);
    vector<Supernode> supernodes;

    // Find chain heads: nodes that start a forced chain.
    // A chain head is a node where we cannot extend a forced chain backwards,
    // i.e. indeg != 1 or its predecessor has outdeg != 1.
    // We detect them by: node is `start`, or indeg[node] != 1, or the unique
    // predecessor has outdeg != 1.
    vector<bool> isHead(n, false);
    isHead[start] = true;
    for (int i = 0; i < n; ++i) {
        if (indeg[i] == 0) { isHead[i] = true; continue; }
        if (indeg[i] > 1)  { isHead[i] = true; continue; }
        // indeg[i] == 1: find predecessor
        bool predOk = false;
        for (int p = 0; p < n; ++p) {
            for (int s : adj[p]) {
                if (s == i && outdeg[p] == 1) { predOk = true; break; }
            }
            if (predOk) break;
        }
        if (!predOk) isHead[i] = true;
    }

    // Grow a chain from each head
    for (int i = 0; i < n; ++i) {
        if (!isHead[i]) continue;
        Supernode sn;
        int cur = i;
        while (true) {
            if (chunkToSuper[cur] != -1) break; // already part of another supernode
            chunkToSuper[cur] = static_cast<int>(supernodes.size());
            sn.chunks.push_back(cur);
            if (outdeg[cur] == 1) {
                const int next = adj[cur][0];
                if (indeg[next] == 1) {
                    cur = next;
                    continue;
                }
            }
            break;
        }
        supernodes.push_back(sn);
    }

    // Any chunk not yet assigned (cycle-only nodes, shouldn't happen in practice)
    for (int i = 0; i < n; ++i) {
        if (chunkToSuper[i] == -1) {
            chunkToSuper[i] = static_cast<int>(supernodes.size());
            supernodes.push_back({{i}});
        }
    }

    const int sn = static_cast<int>(supernodes.size());
    superAdj.assign(sn, {});

    for (int si = 0; si < sn; ++si) {
        const int tail = supernodes[si].chunks.back();
        for (const int nb : adj[tail]) {
            int sj = chunkToSuper[nb];
            if (sj != si)
                superAdj[si].push_back(sj);
        }
    }

    return supernodes;
}

// --------------------------------------------------------------------------
// DFS over supernodes (Phase 5)
// --------------------------------------------------------------------------

struct DfsState {
    vector<int> order;    // supernode indices placed so far
    vector<bool> used;
    int reservoirLevel = 0;   // bit-reservoir fill level after everything placed so far
    long long score  = 0;     // summed junctionCost over the junctions between placed supernodes
};

// Per-chunk Layer III side-info features at each end, for the continuity ranking heuristic.
// *First = granule 0 of the chunk's first complete frame; *Last = last granule of the last
// complete frame. -1 when unavailable (no complete frame / side info didn't fit).
// See PLAN_sideinfo_mainbits.md Step 1 + A2: `global_gain` (granule loudness) is the dominant
// junction-continuity signal - |Δ global_gain| ranks the true successor #1 in ~60-74% of VBR
// branch points (vs ~30% for the graph's own order). A2: adding a small |Δ big_values|
// (spectral-occupancy) term robustly nudges rank-1 up ~1-3pp on every fixture. Ranking only -
// never rejects an edge.
struct BoundaryGain {
    int ggFirst = -1, ggLast = -1;   // global_gain
    int bvFirst = -1, bvLast = -1;   // big_values
};

// Junction cost of putting a supernode whose first chunk is `b` right after one whose last
// chunk is `a`: |Δ global_gain| + |Δ big_values|/BV_DIV across that boundary (A2 - the /32
// matches the z-scored 1.0*gg + 0.15*bv the probe found robust). When global_gain is unknown
// on either side, a fixed penalty above the 255 max real delta - so an ordering that resolves
// more junctions scores better, unknowns sort last, and the running sum stays finite (needed
// for the best-of branch-and-bound). Used for both DFS exploration order and the best-of score.
static constexpr int UNKNOWN_JUNCTION_COST = 384;
static constexpr int BV_DIV = 32;
static int junctionCost(const vector<BoundaryGain>& bg, const int a, const int b) {
    if (bg[a].ggLast < 0 || bg[b].ggFirst < 0) return UNKNOWN_JUNCTION_COST;
    int cost = std::abs(bg[a].ggLast - bg[b].ggFirst);
    if (bg[a].bvLast >= 0 && bg[b].bvFirst >= 0)
        cost += std::abs(bg[a].bvLast - bg[b].bvFirst) / BV_DIV;
    return cost;
}

// --- Milestone 2: spectral-continuity term added to the candidate ranking (not to the
// best-of score / branch-and-bound). Decodes the trailing granule of `cur`'s last frame and
// the leading granule of a candidate's first frame - cross-chunk main-data assembly, so the
// trailing spectrum needs `cur`'s predecessor and the leading one needs `cur` itself.
// Memoised by (predecessor, chunk); distinct keys ~= edge count. RP_SPEC=0 disables.
// The probe (sideinfo_probe Measurement D) found z(global_gain) + 0.5*z(spectralL2) a strong
// net win; here the raw 12-band-energy L2 distance is scaled by g_specScale, auto-calibrated
// per file at startup so a median junction contributes a gentle ~3 (RP_SPEC_SCALE overrides).
static bool   g_specRank   = true;
static int    g_sampleRate = 44100;
static double g_specScale  = 10.0;
static std::unordered_map<long long, Mp3MainData::GranuleSpectrum> g_specHead, g_specTail;

static const Mp3MainData::GranuleSpectrum* memoSpec(bool head, const vector<ChunkMeta>& metas,
                                                    int prev, int chunk) {
    if (chunk < 0 || !metas[chunk].valid || metas[chunk].frames.empty()) return nullptr;
    auto& memo = head ? g_specHead : g_specTail;
    const long long key = ((long long)(prev + 1) << 20) | (unsigned)chunk;
    auto it = memo.find(key);
    if (it == memo.end()) {
        static const vector<uint8_t> empty;
        const vector<uint8_t>& pv = (prev >= 0) ? chunks[prev] : empty;
        const ChunkMeta* pm = (prev >= 0) ? &metas[prev] : nullptr;
        const int off = head ? metas[chunk].frames.front().offset : metas[chunk].frames.back().offset;
        const Mp3MainData::FrameSpectra fsp =
            Mp3MainData::decodeChunkFrame(pm, pv, chunks[chunk], metas[chunk], off, g_sampleRate);
        const bool mpeg1 = metas[chunk].profile.versionID == 0b11;
        Mp3MainData::GranuleSpectrum gs; // gs.valid == false by default
        if (fsp.valid) gs = head ? fsp.gr[0][0] : fsp.gr[mpeg1 ? 1 : 0][0];
        it = memo.emplace(key, gs).first;
    }
    return it->second.valid ? &it->second : nullptr;
}

static double spectralJunctionCost(const vector<ChunkMeta>& metas, int curPrev, int curTail, int candHead) {
    const Mp3MainData::GranuleSpectrum* A = memoSpec(false, metas, curPrev, curTail);
    const Mp3MainData::GranuleSpectrum* B = memoSpec(true, metas, curTail, candHead);
    if (!A || !B) return 0.0; // no spectrum on one side -> spectral term contributes nothing
    double s = 0;
    for (int q = 0; q < Mp3MainData::NBANDS; ++q) {
        const double d = A->bandEnergy[q] - B->bandEnergy[q];
        s += d * d;
    }
    return g_specScale * std::sqrt(s);
}

// --- Full PCM decode junction term (PLAN_full_pcm_decode.md). For a candidate edge, decode
// the last (up to) 3 chunks of the current path followed by the candidate's first chunk all
// the way to PCM with minimp3 (fresh decoder, the first 2 chunks are warm-up - with them the
// window decode is sample-exact vs a full-file decode), and measure the spectral-flux spike
// at the junction relative to the flux inside the already-placed audio. A wrong successor
// produces a hybrid straddling frame + a jump to another moment of the song. pcm_probe:
// ranks the true successor #1 in 47-92% of branch points (Milestone 2: 11-51%).
// Memoised by the (PP, P, cur, cand) chunk tuple. Ranking only - never rejects an edge
// (not a safe filter: some true junctions do spike). RP_PCM=0 disables; RP_PCM_W weights it
// relative to the existing (global_gain + spectral) cost, see the calibration in main().
static vector<int> g_matchSucc;   // global matching: successor per supernode (DFS hint), -1 none
static bool   g_pcmRank    = true;
static double g_pcmScale   = 1.0;  // set by calibration
static double g_pcmNeutral = 0.0;  // raw value used when a junction can't be measured
static std::unordered_map<uint64_t, float> g_pcmMemo;

// Raw log1p(flux) for path tail `left[0..nLeft)` (oldest first) followed by `cand`;
// -1 when the window can't be measured (too little audio, silent pre side).
static double pcmRaw(const vector<ChunkMeta>& metas, const int* left, int nLeft, int cand) {
    while (nLeft > 1 && !metas[left[0]].valid) { ++left; --nLeft; }
    uint64_t key = (uint64_t)(unsigned)cand;
    for (int k = 0; k < 3; ++k)
        key = (key << 16) | (k < nLeft ? (uint64_t)(unsigned)left[k] : 0xFFFFu);
    auto it = g_pcmMemo.find(key);
    if (it != g_pcmMemo.end()) return it->second;
    vector<const vector<uint8_t>*> segs;
    for (int k = 0; k < nLeft; ++k) segs.push_back(&chunks[left[k]]);
    segs.push_back(&chunks[cand]);
    const PcmJunction::Metrics m = PcmJunction::scoreJunction(segs, nLeft, metas[left[0]].headOffset);
    const float v = (m.valid && !m.silent) ? (float)std::log1p(m.flux) : -1.0f;
    g_pcmMemo.emplace(key, v);
    return v;
}

static double pcmJunctionCost(const vector<ChunkMeta>& metas, const int* left, int nLeft, int cand) {
    const double v = pcmRaw(metas, left, nLeft, cand);
    return g_pcmScale * (v < 0 ? g_pcmNeutral : v);
}

// --- Block-move repair (PLAN_full_pcm_decode.md step 6). The DFS returns the FIRST valid
// ordering; when locking/ranking took one wrong turn it can be complete-but-wrong with a whole
// block of the song in the wrong place (A-C-B instead of A-B-C). Summed PCM flux points at the
// true order (2-opt test: 97-100% of swaps), so: repeatedly move a contiguous block of chunks
// to another position when (1) all 3 new junctions are structural edges of the FULL graph
// (before AC-3/locking - so a wrong lock can be undone), (2) the whole ordering still passes
// the bit-reservoir check, and (3) the summed log1p(flux) strictly decreases. Chunk 0 stays
// pinned first. Best-improvement per pass until no move helps. RP_REPAIR=0 disables.
// Lexicographic cost: (number of junctions the PCM window can't measure, summed log1p(flux)
// over the rest). Unmeasurable first, so a move can't "win" by turning a high-flux junction
// into an unscorable one (seen on test7_short when unmeasurable junctions used a neutral value).
struct OrderCost {
    int breaks = 0;      // reservoir-check failures (only tracked when repairing an invalid order)
    int unmeasured = 0;
    double sum = 0;
    bool operator<(const OrderCost& o) const {
        if (breaks != o.breaks) return breaks < o.breaks;
        return unmeasured != o.unmeasured ? unmeasured < o.unmeasured : sum < o.sum - 1e-9;
    }
};
static OrderCost summedFlux(const vector<ChunkMeta>& metas, const vector<int>& seq) {
    OrderCost c;
    for (size_t k = 1; k < seq.size(); ++k) {
        int left[3], nl = 0;
        for (int j = (int)k - 3; j < (int)k; ++j) if (j >= 0) left[nl++] = seq[j];
        const double v = pcmRaw(metas, left, nl, seq[k]);
        if (v < 0) ++c.unmeasured; else c.sum += v;
    }
    return c;
}

static bool reservoirValid(const vector<ChunkMeta>& metas, const vector<int>& seq,
                           const int sideInfo, const int maxReservoir) {
    int level = 0;
    const ChunkMeta* prev = nullptr;
    for (int ci : seq) {
        if (!advanceReservoirChunk(level, prev, metas[ci], chunks[ci], sideInfo, maxReservoir)) return false;
        prev = &metas[ci];
    }
    return true;
}

// Number of chunks at which the reservoir check fails, continuing after each failure with a
// full reservoir (so later failures are counted independently). `where` gets their positions.
static int reservoirBreaks(const vector<ChunkMeta>& metas, const vector<int>& seq,
                           const int sideInfo, const int maxReservoir, vector<int>* where = nullptr) {
    int level = 0, breaks = 0;
    const ChunkMeta* prev = nullptr;
    for (size_t k = 0; k < seq.size(); ++k) {
        const int ci = seq[k];
        if (!advanceReservoirChunk(level, prev, metas[ci], chunks[ci], sideInfo, maxReservoir)) {
            ++breaks;
            if (where) where->push_back((int)k);
            level = maxReservoir * 8;
        }
        prev = &metas[ci];
    }
    return breaks;
}

// Two move types, each enumerated only where every new junction is a structural edge:
//   block move: take seq[i..j] out and reinsert it after position k (3 new junctions);
//   block swap: exchange two disjoint blocks seq[i1..j1] and seq[i2..j2] (4 new junctions) -
//               needed for the A-X-Y-B -> A-B-Y-X shape, where every single block move on the
//               way is structurally invalid.
// Returns the number of moves applied; `seq` is updated in place. `gap` (optional) gets the
// stability margin of the final order: how much the summed flux would rise under the cheapest
// valid move the last (full-scan) pass rejected - NaN when no full scan completed. A small gap
// means a nearby, almost equally cheap ordering exists (the wrong-output risk).
static int repairOrdering(vector<int>& seq, const vector<ChunkMeta>& metas,
                          const vector<vector<int>>& adjFull, const bool isLayerIII,
                          const int sideInfo, const int maxReservoir, const int maxPasses,
                          double* gap = nullptr) {
    const int n = static_cast<int>(seq.size());
    std::unordered_set<uint64_t> edge;
    vector<vector<int>> preds(adjFull.size());
    for (size_t a = 0; a < adjFull.size(); ++a)
        for (int b : adjFull[a]) { edge.insert(((uint64_t)a << 32) | (unsigned)b); preds[b].push_back((int)a); }
    auto isEdge = [&](int a, int b) { return edge.count(((uint64_t)(unsigned)a << 32) | (unsigned)b) > 0; };

    // Suspect filter: a move can only fix the ordering by cutting a wrong junction, and wrong
    // splices are usually high-flux (pcm_probe: true median ~1.05, wrong ~3). Phase 0 of each
    // pass only evaluates moves that cut one of the K worst junctions (plus, for an invalid
    // order, the junctions just before each reservoir break); phase 1 scans every move if phase
    // 0 found nothing. RP_REPAIR_SUSPECTS=K overrides (0 = no filter); RP_REPAIR_BUDGET (s,
    // default 60) bounds the whole repair - when spent, the best move found so far is applied.
    int suspects = std::max(16, n / 20);
    if (const char* v = std::getenv("RP_REPAIR_SUSPECTS")) suspects = atoi(v);
    double budget = 60;
    if (const char* v = std::getenv("RP_REPAIR_BUDGET")) budget = atof(v);
    const auto deadline = chrono::steady_clock::now() +
        chrono::duration_cast<chrono::steady_clock::duration>(chrono::duration<double>(budget));
    bool outOfTime = false;
    if (gap) *gap = std::numeric_limits<double>::quiet_NaN();
    double nearest = std::numeric_limits<double>::infinity();   // cheapest rejected move (sum delta)
    string nearestWhat;

    // A candidate is a list of [from, to] segments of the current `seq`. It is evaluated
    // incrementally: a junction's flux depends on its 4-chunk window, so only the first 3
    // positions of each segment need a pcmRaw lookup (the rest come from prefix sums of the
    // current order), and the reservoir state re-synchronises with the current order's within
    // a frame or two, after which the segment's remaining breaks come from prefix sums too.
    using Seg = pair<int, int>;
    vector<double> jv(n, 0);       // junction value at k (k >= 1), -1 = unmeasured
    vector<double> PS(n + 1, 0);   // prefix sums of measured jv
    vector<int> PU(n + 1, 0);      // prefix counts of unmeasured
    vector<int> lvlAfter(n, 0);    // reservoir level after chunk k (break -> reset to full)
    vector<int> PB(n + 1, 0);      // prefix reservoir-break counts
    auto prepare = [&]() {
        for (int k = 0; k < n; ++k) {
            double v = 0;
            if (k >= 1) {
                int left[3], nl = 0;
                for (int j = k - 3; j < k; ++j) if (j >= 0) left[nl++] = seq[j];
                v = pcmRaw(metas, left, nl, seq[k]);
            }
            jv[k] = v;
            PS[k + 1] = PS[k] + ((k >= 1 && v >= 0) ? v : 0);
            PU[k + 1] = PU[k] + ((k >= 1 && v < 0) ? 1 : 0);
        }
        int level = 0;
        const ChunkMeta* prev = nullptr;
        for (int k = 0; k < n; ++k) {
            bool br = false;
            if (isLayerIII && !advanceReservoirChunk(level, prev, metas[seq[k]], chunks[seq[k]], sideInfo, maxReservoir)) {
                br = true;
                level = maxReservoir * 8;
            }
            lvlAfter[k] = level;
            PB[k + 1] = PB[k] + (br ? 1 : 0);
            prev = &metas[seq[k]];
        }
    };
    auto evalBreaks = [&](const vector<Seg>& segs) {
        if (!isLayerIII) return 0;
        int level = 0, breaks = 0;
        const ChunkMeta* prev = nullptr;
        for (const Seg& sg : segs) {
            for (int k = sg.first; k <= sg.second; ++k) {
                if (k > sg.first && level == lvlAfter[k - 1]) {   // in sync with the current order
                    breaks += PB[sg.second + 1] - PB[k];
                    level = lvlAfter[sg.second];
                    break;
                }
                if (!advanceReservoirChunk(level, prev, metas[seq[k]], chunks[seq[k]], sideInfo, maxReservoir)) {
                    ++breaks;
                    level = maxReservoir * 8;
                }
                prev = &metas[seq[k]];
            }
            prev = &metas[seq[sg.second]];
        }
        return breaks;
    };
    auto evalFlux = [&](const vector<Seg>& segs, OrderCost& c) {
        int g = 0;              // position in the candidate
        int last[3], nl = 0;    // last (up to) 3 chunks placed, oldest first
        for (const Seg& sg : segs) {
            for (int k = sg.first; k <= sg.second; ++k, ++g) {
                if (g >= 1) {
                    if (k - sg.first >= 3 || sg.first == 0) {   // same window as the current order
                        c.sum += PS[sg.second + 1] - PS[k];
                        c.unmeasured += PU[sg.second + 1] - PU[k];
                        g += sg.second - k;
                        k = sg.second;
                        nl = 0;
                        for (int j = std::max(sg.first, sg.second - 2); j <= sg.second; ++j) last[nl++] = seq[j];
                        if (nl < 3 && sg.second - sg.first < 2) {
                            // segment shorter than 3 and fully skipped can't happen here
                        }
                        goto nextSeg;
                    }
                    const double v = pcmRaw(metas, last, nl, seq[k]);
                    if (v < 0) ++c.unmeasured; else c.sum += v;
                }
                if (nl < 3) last[nl++] = seq[k];
                else { last[0] = last[1]; last[1] = last[2]; last[2] = seq[k]; }
            }
            nextSeg:;
        }
    };
    auto build = [&](const vector<Seg>& segs) {
        vector<int> t; t.reserve(n);
        for (const Seg& sg : segs) t.insert(t.end(), seq.begin() + sg.first, seq.begin() + sg.second + 1);
        return t;
    };

    prepare();
    OrderCost cur;
    cur.sum = PS[n]; cur.unmeasured = PU[n];
    // An invalid starting order (from the global matching) is repaired towards validity first:
    // the cost then leads with the number of reservoir breaks.
    const bool trackBreaks = isLayerIII && PB[n] > 0;
    if (trackBreaks) cur.breaks = PB[n];
    int applied = 0;
    for (int pass = 0; pass < maxPasses && !outOfTime; ++pass) {
        if (pass > 0) prepare();
        vector<int> pos(chunks.size(), -1);
        for (int k = 0; k < n; ++k) pos[seq[k]] = k;
        vector<char> bad(n + 1, suspects <= 0 ? 1 : 0);   // bad[k]: junction seq[k-1] -> seq[k]
        if (suspects > 0) {
            vector<pair<double, int>> js;
            for (int k = 1; k < n; ++k) js.push_back({jv[k] < 0 ? 1e9 : jv[k], k});
            std::sort(js.begin(), js.end(), [](const pair<double, int>& x, const pair<double, int>& y) { return x.first > y.first; });
            for (int q = 0; q < (int)js.size() && q < suspects; ++q) bad[js[q].second] = 1;
        }
        if (cur.breaks > 0)
            for (int k = 0; k < n; ++k)
                if (PB[k + 1] > PB[k]) for (int d = 0; d <= 8; ++d) if (k - d >= 1) bad[k - d] = 1;
        auto cut = [&](int k) { return k < n && bad[k]; };
        struct Move { OrderCost cost; vector<Seg> segs; string what; };
        vector<Move> moves;
        long candidates = 0;
        auto consider = [&](vector<Seg>&& segs, string&& what) {
            if (outOfTime) return;
            if ((candidates & 0xFF) == 0 && chrono::steady_clock::now() >= deadline) { outOfTime = true; return; }
            ++candidates;
            OrderCost c;
            c.breaks = evalBreaks(segs);
            // valid order: stay valid; invalid order: a move must reduce the breaks
            if (cur.breaks > 0 ? c.breaks >= cur.breaks : c.breaks > 0) return;
            if (!trackBreaks) c.breaks = 0;
            evalFlux(segs, c);
            if (c < cur) moves.push_back({c, std::move(segs), std::move(what)});
            else if (c.breaks == cur.breaks && c.unmeasured == cur.unmeasured && c.sum - cur.sum < nearest) {
                nearest = c.sum - cur.sum;
                nearestWhat = what;
            }
        };
        auto push = [](vector<Seg>& v, int a, int b) { if (a <= b) v.push_back({a, b}); };

        bool moved = false;
        for (int phase = suspects > 0 ? 0 : 1; phase < 2 && !moved && !outOfTime; ++phase) {
            if (phase == 1) std::fill(bad.begin(), bad.end(), 1);
            moves.clear();
            candidates = 0;
            nearest = std::numeric_limits<double>::infinity();

            // block moves: take seq[i..j] out and reinsert it after position k
            for (int i = 1; i < n && !outOfTime; ++i)
                for (int p : preds[seq[i]]) {
                    const int k = pos[p];
                    if (k < 0 || k == i - 1) continue;
                    const bool outerCut = cut(i) || cut(k + 1);
                    for (int j = i; j < n && !outOfTime; ++j) {
                        if (k >= i && k <= j) break;
                        if (!outerCut && !cut(j + 1)) continue;
                        if (j + 1 < n && !isEdge(seq[i - 1], seq[j + 1])) continue;
                        if (k + 1 < n && !isEdge(seq[j], seq[k + 1])) continue;
                        vector<Seg> sg;
                        if (k < i) { push(sg, 0, k); push(sg, i, j); push(sg, k + 1, i - 1); push(sg, j + 1, n - 1); }
                        else       { push(sg, 0, i - 1); push(sg, j + 1, k); push(sg, i, j); push(sg, k + 1, n - 1); }
                        consider(std::move(sg), "move [" + std::to_string(i) + ".." + std::to_string(j) +
                                                "] after " + std::to_string(k));
                    }
                }
            // block swaps: seq[i1..j1] <-> seq[i2..j2] with a non-empty middle
            for (int i1 = 1; i1 < n && !outOfTime; ++i1)
                for (int s2 : adjFull[seq[i1 - 1]]) {
                    const int i2 = pos[s2];
                    if (i2 < 0 || i2 <= i1 + 1) continue;
                    if (!isEdge(seq[i2 - 1], seq[i1])) continue;
                    const bool outerCut = cut(i1) || cut(i2);
                    for (int j1 = i1; j1 + 1 < i2 && !outOfTime; ++j1)
                        for (int p : preds[seq[j1 + 1]]) {
                            const int j2 = pos[p];
                            if (j2 < i2) continue;
                            if (!outerCut && !cut(j1 + 1) && !cut(j2 + 1)) continue;
                            if (j2 + 1 < n && !isEdge(seq[j1], seq[j2 + 1])) continue;
                            vector<Seg> sg;
                            push(sg, 0, i1 - 1); push(sg, i2, j2); push(sg, j1 + 1, i2 - 1);
                            push(sg, i1, j1); push(sg, j2 + 1, n - 1);
                            consider(std::move(sg), "swap [" + std::to_string(i1) + ".." + std::to_string(j1) +
                                                    "] <-> [" + std::to_string(i2) + ".." + std::to_string(j2) + "]");
                        }
                }

            if (moves.empty()) continue;
            const Move& m = *std::min_element(moves.begin(), moves.end(),
                                              [](const Move& x, const Move& y) { return x.cost < y.cost; });
            cout << "  repair pass " << pass + 1 << (phase == 0 ? " (suspects)" : " (full scan)") << ": " << m.what
                 << ", cost (" << (trackBreaks ? "resv breaks, " : "") << "unmeasured, summed flux) ("
                 << (trackBreaks ? std::to_string(cur.breaks) + ", " : string()) << cur.unmeasured << ", "
                 << fixed << setprecision(3) << cur.sum << ") -> ("
                 << (trackBreaks ? std::to_string(m.cost.breaks) + ", " : string()) << m.cost.unmeasured << ", "
                 << m.cost.sum << ") (" << candidates << " valid moves scanned, " << moves.size() << " improving)\n";
            seq = build(m.segs);
            cur = m.cost;
            ++applied;
            moved = true;
        }
        if (!moved) {
            cout << "  repair pass " << pass + 1 << ": no improving valid move";
            if (!outOfTime) {
                if (gap) *gap = nearest;
                if (std::isfinite(nearest))
                    cout << " (nearest valid alternative: +" << fixed << setprecision(3) << nearest
                         << " summed flux, " << nearestWhat << ")";
                else cout << " (no other valid ordering within one move)";
            }
            cout << "\n";
            break;
        }
        if (outOfTime) cout << "  repair: time budget (" << budget << " s) spent - stopping\n";
    }
    return applied;
}

// DFS instrumentation / bounding. `counter` = number of supernode placements tried.
// RP_MAX_NODES caps it (0 = unlimited) so a bounded A/B run stops cleanly instead of being
// SIGTERM'd (which loses buffered stdout); RP_PROGRESS overrides how often a progress line
// is printed. g_aborted unwinds the whole recursion once the cap is hit.
static uint64_t counter        = 0;
static vector<int> g_deepestOrder;   // deepest partial ordering reached (diagnostic)
static uint64_t g_maxNodes     = 0;
static uint64_t g_progressEvery = COUNTER_FREQUENCY;
static bool     g_aborted      = false;
static chrono::steady_clock::time_point g_dfsStart;
static chrono::steady_clock::time_point g_deadline;   // wall-clock stop for the whole search
static bool     g_haveDeadline = false;

// Best-of search (A1, RP_BESTOF=1): instead of returning the first complete valid ordering,
// keep exploring (bounded by the deadline) and remember the lowest-scoring one, score =
// summed junctionCost over every supernode boundary; branch-and-bound prunes any prefix whose
// partial score already meets/exceeds the best complete score.
//
// OFF by default: measured worse than first-valid. The summed-junctionCost objective is
// misaligned with truth - its global minimum is a "smoother than reality" reorder (sample-3s:
// 25% of chunks in place vs 76-100% for first-valid), because a real stream's true order
// genuinely contains large global_gain steps at transients/section edges. global_gain is a
// good *local rank* signal, not a good *global objective*. Kept behind the flag for
// experimenting with a better objective (reservoir margin, or a multi-feature score verified
// to have the true order as its argmin). See CLAUDE.md item 7.
static bool          g_bestOf     = false;
static long long     g_bestScore  = LLONG_MAX;
static vector<int>   g_bestOrder;

// RP_BEAM: at each DFS node, recurse into at most this many reservoir-valid candidates,
// taken in continuity-rank order (0 = unlimited = exhaustive). A small beam turns the DFS
// into near-greedy descent - it reaches full depth fast when the global_gain heuristic is
// right, but can dead-end with no complete ordering when the heuristic is wrong at some node,
// so it is normally driven by the iterative-broadening loop in main().
static int g_beamK = 0;

static double dfsElapsedSec() {
    return chrono::duration<double>(chrono::steady_clock::now() - g_dfsStart).count();
}

/**
 * Exhaustive DFS over the supernode graph, starting from the supernode containing chunk 0.
 * The Layer III bit-reservoir check now runs incrementally as each candidate supernode is
 * tried (see advanceReservoirChunk), instead of only once the whole order is complete - a
 * candidate that would break the reservoir chain is pruned immediately rather than explored
 * to the end, per CLAUDE.md TODO "Incremental validateReservoir inside the DFS".
 * @param state The current state of the search. state.reservoirLevel must already reflect
 *              every chunk in state.order.
 * @param supernodes A list of supernode structures containing grouped MP3 chunks.
 * @param superAdj The adjacency list representing valid transitions between supernodes.
 * @param metas Metadata for all chunks.
 * @param sideInfo Side-information size in bytes for this stream (0 / unused for non-Layer-III).
 * @param maxReservoir Reservoir capacity in bytes (0 / unused for non-Layer-III).
 * @param isLayerIII Whether the reservoir check applies at all to this stream.
 * @param result If found, the valid supernode sequence is filled into result.
 * @return True if a valid, decodable sequence of all supernodes exists.
 */
bool dfs(DfsState& state,
         const vector<Supernode>& supernodes,
         const vector<vector<int>>& superAdj,
         const vector<ChunkMeta>& metas,
         const vector<BoundaryGain>& boundaryGain,
         const bool rankByContinuity,
         const int sideInfo,
         const int maxReservoir,
         const bool isLayerIII,
         vector<int>& result) {

    if (g_aborted) return false;
    if (state.order.size() > g_deepestOrder.size()) g_deepestOrder = state.order;

    if (state.order.size() == supernodes.size()) {
        // Reservoir validity has already been established incrementally for every chunk
        // placed so far - nothing left to verify.
        if (!g_bestOf) {
            result = state.order;
            return true;
        }
        if (state.score < g_bestScore) {
            g_bestScore = state.score;
            g_bestOrder = state.order;
            cout << "  [ordering found: score=" << state.score
                 << " nodes=" << counter
                 << " elapsed=" << fixed << setprecision(1) << dfsElapsedSec() << "s]\n" << flush;
        }
        return g_bestScore == 0; // score 0 = perfect continuity, cannot be beaten - stop early
    }

    const int cur = state.order.back();
    const int curTailChunk = supernodes[cur].chunks.back();

    // Chunk immediately preceding curTailChunk in the current path (for the trailing spectrum's
    // cross-chunk main-data assembly). -1 when cur is the pinned start.
    int curPrevChunk = -1;
    if (supernodes[cur].chunks.size() >= 2)
        curPrevChunk = supernodes[cur].chunks[supernodes[cur].chunks.size() - 2];
    else if (state.order.size() >= 2)
        curPrevChunk = supernodes[state.order[state.order.size() - 2]].chunks.back();

    // Try structurally-legal successors in order of junction continuity (smallest cost first).
    // Pure exploration ordering - every candidate is still tried (subject to the beam cap),
    // and the reservoir check below is unchanged.
    vector<int> candidates(superAdj[cur].begin(), superAdj[cur].end());
    if (rankByContinuity && candidates.size() > 1) {
        const bool useSpec = g_specRank && isLayerIII;
        const bool usePcm  = g_pcmRank;
        // Last (up to) 3 chunks of the path, oldest first - the PCM window's left side.
        int left[3], nLeft = 0;
        if (usePcm) {
            int tmp[3];
            for (int oi = (int)state.order.size() - 1; oi >= 0 && nLeft < 3; --oi) {
                const auto& ch = supernodes[state.order[oi]].chunks;
                for (int k = (int)ch.size() - 1; k >= 0 && nLeft < 3; --k) tmp[nLeft++] = ch[k];
            }
            for (int k = 0; k < nLeft; ++k) left[k] = tmp[nLeft - 1 - k];
        }
        // Each candidate's cost computed once (the PCM term is a full decode per edge).
        vector<pair<double, int>> keyed;
        keyed.reserve(candidates.size());
        for (int x : candidates) {
            const int head = supernodes[x].chunks.front();
            double c = junctionCost(boundaryGain, curTailChunk, head);
            if (useSpec) c += spectralJunctionCost(metas, curPrevChunk, curTailChunk, head);
            if (usePcm && !state.used[x]) c += pcmJunctionCost(metas, left, nLeft, head);
            if (!g_matchSucc.empty() && g_matchSucc[cur] == x) c -= 1e9;   // global matching hint
            keyed.push_back({c, x});
        }
        stable_sort(keyed.begin(), keyed.end(),
                    [](const pair<double, int>& a, const pair<double, int>& b) { return a.first < b.first; });
        for (size_t k = 0; k < keyed.size(); ++k) candidates[k] = keyed[k].second;
    }

    int recursed = 0; // reservoir-valid candidates descended into so far, for the RP_BEAM cap
    for (int si : candidates) {
        if (state.used[si]) continue;

        const int jc = junctionCost(boundaryGain, curTailChunk, supernodes[si].chunks.front());
        // Branch-and-bound: this prefix can only get more expensive, so if it already ties or
        // beats the best complete ordering there is no point descending. Candidates are sorted
        // ascending by jc when ranking is on, so every later one is >= this: break, not continue.
        if (g_bestOf && state.score + jc >= g_bestScore) {
            if (rankByContinuity) break;
            continue;
        }

        const int savedLevel = state.reservoirLevel;
        bool reservoirOk = true;
        if (isLayerIII) {
            int prevCi = curTailChunk; // last chunk of `cur`'s supernode directly precedes si's first chunk
            for (int ci : supernodes[si].chunks) {
                if (!advanceReservoirChunk(state.reservoirLevel, &metas[prevCi], metas[ci], chunks[ci], sideInfo, maxReservoir)) {
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

        if (++counter % g_progressEvery == 0) {
            cout << "  [nodes=" << counter << " depth=" << state.order.size()
                 << "/" << supernodes.size() << " elapsed="
                 << fixed << setprecision(1) << dfsElapsedSec() << "s]\n" << flush;
        }
        if (g_maxNodes && counter >= g_maxNodes) g_aborted = true;
        // Wall-clock stop. Sampled every 64 placements: with the PCM term a single placement can
        // cost tens of ms (a decode per candidate), so the old every-2^18 sampling overshot a
        // 45 s budget by minutes on dense graphs. steady_clock::now is a vDSO call (~20 ns).
        if (g_haveDeadline && (counter & 0x3F) == 0 && chrono::steady_clock::now() >= g_deadline)
            g_aborted = true;

        const bool hit = !g_aborted &&
            dfs(state, supernodes, superAdj, metas, boundaryGain, rankByContinuity, sideInfo, maxReservoir, isLayerIII, result);

        state.order.pop_back();
        state.used[si] = false;
        state.reservoirLevel = savedLevel;
        state.score -= jc;

        if (hit) return true;              // first-valid mode, or best-of early-out (score 0)
        if (g_aborted) return false;
        if (g_beamK && ++recursed >= g_beamK) break;
    }
    return false;
}

// --- Global min-cost successor matching (method 1 of "global search on summed flux"). ---
// The DFS uses flux locally: one wrong early pick strands it. Instead, give EVERY supernode
// exactly one successor and one predecessor so that the summed edge cost (log1p of the median
// junction flux) is minimal - an assignment problem, solved exactly by successive shortest
// paths on the sparse bipartite graph. The start supernode has no predecessor; a virtual END
// takes the last one (only the final-fragment chunk's supernode if there is one). The result
// is a path from the start plus possibly some cycles; each cycle is cut at its best edge and
// spliced into the path where both new junctions are structural edges, at minimal added cost.
// The caller then checks the bit reservoir and runs the repair. Edge context: a supernode of
// >= 2 chunks gives its own second-to-last chunk as the window's warm-up chunk; a single-chunk
// one uses the median over (up to 8 sampled) structural predecessors, as in edge locking.

struct MatchResult {
    vector<int> order;            // supernode order (start first), empty on failure
    vector<int> succ;             // matched successor per supernode (-1 = END)
    int cycles = 0, spliced = 0;
    double cost = 0;
    long decodes = 0;
    int contexts = 8;             // predecessor contexts per single-chunk supernode (decode cap)
    string failure;
};

static MatchResult globalMatch(const vector<Supernode>& sn, const vector<vector<int>>& superAdj,
                               int startSuper, const vector<vector<int>>& chunkAdj,
                               const vector<ChunkMeta>& metas, EdgeLock::FluxMemo& memo, int threads) {
    MatchResult res;
    const int N = static_cast<int>(sn.size());
    // chunk-level predecessors in the current (locked) graph, for single-chunk contexts
    vector<vector<int>> preds(chunkAdj.size());
    for (size_t a = 0; a < chunkAdj.size(); ++a) for (int b : chunkAdj[a]) preds[b].push_back((int)a);
    auto contexts = [&](int s, int maxCtx) {
        const auto& ch = sn[s].chunks;
        vector<int> ctx;
        if (ch.size() >= 2) ctx.push_back(ch[ch.size() - 2]);
        else if (s == startSuper) ctx.push_back(-1);
        else {
            const auto& pr = preds[ch.back()];
            if ((int)pr.size() <= maxCtx) ctx = pr;
            else for (int k = 0; k < maxCtx; ++k) ctx.push_back(pr[(size_t)k * pr.size() / maxCtx]);
            if (ctx.empty()) ctx.push_back(-1);
        }
        return ctx;
    };
    // Decode cap: dense low-bitrate graphs (bohemoan 32 kbps at 1-2 KB: ~700 supernodes x ~140
    // successors x 8 contexts) took > 15 min just to score. Halve the number of predecessor
    // contexts per single-chunk supernode until the windows not yet in the memo fit under
    // RP_MATCH_MAX_DECODES; skip the matching if even one context doesn't fit. Count-based (not
    // time-based) so whether and how the matching runs doesn't depend on the core count.
    long cap = 100000;
    if (const char* v = std::getenv("RP_MATCH_MAX_DECODES")) cap = atol(v);
    vector<vector<int>> ctxOf(N);
    vector<std::array<int, 3>> windows;
    for (int maxCtx = 8; ; maxCtx /= 2) {
        windows.clear();
        for (int s = 0; s < N; ++s) {
            ctxOf[s] = contexts(s, maxCtx);
            const int a = sn[s].chunks.back();
            for (int t : superAdj[s]) {
                const int b = sn[t].chunks.front();
                for (int p : ctxOf[s]) if (p != b) windows.push_back({p, a, b});
            }
        }
        long todo = 0;
        { std::unordered_set<uint64_t> miss;
          for (const auto& w : windows) {
              const uint64_t k = EdgeLock::packKey((w[0] >= 0 && metas[w[0]].valid) ? w[0] : -1, w[1], w[2]);
              if (!memo.count(k)) miss.insert(k);
          }
          todo = (long)miss.size(); }
        res.contexts = maxCtx;
        if (todo <= cap) {
            cout << "Global matching: scoring " << todo << " new windows (<= " << maxCtx
                 << " contexts per edge) ...\n" << flush;
            break;
        }
        if (maxCtx == 1) {
            res.failure = "skipped (" + std::to_string(todo) + " window decodes > " + std::to_string(cap) +
                          " even with 1 context; RP_MATCH_MAX_DECODES raises the cap)";
            return res;
        }
    }
    res.decodes = EdgeLock::scoreWindows(chunks, metas, windows, memo, threads);

    // edge costs
    std::unordered_map<uint64_t, double> cost;
    vector<double> finite;
    auto ekey = [N](int s, int t) { return (uint64_t)s * (uint64_t)(N + 1) + (uint64_t)t; };
    for (int s = 0; s < N; ++s) {
        const int a = sn[s].chunks.back();
        for (int t : superAdj[s]) {
            const int b = sn[t].chunks.front();
            vector<double> v;
            for (int p : ctxOf[s]) {
                if (p == b) continue;
                const int pp = (p >= 0 && metas[p].valid) ? p : -1;
                auto it = memo.find(EdgeLock::packKey(pp, a, b));
                if (it != memo.end() && !std::isnan(it->second)) v.push_back(it->second);
            }
            double c = std::numeric_limits<double>::quiet_NaN();
            if (!v.empty()) {
                std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                c = std::log1p(v[v.size() / 2]);
                finite.push_back(c);
            }
            cost[ekey(s, t)] = c;
        }
    }
    double neutral = 1.0;
    if (!finite.empty()) {
        std::nth_element(finite.begin(), finite.begin() + finite.size() / 2, finite.end());
        neutral = finite[finite.size() / 2];
    }
    for (auto& kv : cost) if (std::isnan(kv.second)) kv.second = neutral;
    auto c = [&](int s, int t) -> double {   // t == -1: END
        if (t < 0) return neutral;
        auto it = cost.find(ekey(s, t));
        return it == cost.end() ? std::numeric_limits<double>::infinity() : it->second;
    };
    int fragSuper = -1;
    for (int s = 0; s < N; ++s) if (metas[sn[s].chunks.back()].finalFragment) fragSuper = s;
    auto mayEnd = [&](int s) { return fragSuper < 0 || s == fragSuper; };

    // Min-cost perfect matching, left = supernodes as predecessors, right = supernodes as
    // successors, with right slot `startSuper` standing for END. SSP + Dijkstra + potentials.
    const int S = 2 * N, T = 2 * N + 1, V = 2 * N + 2;
    struct E { int to, cap; long long w; };
    vector<E> e;
    vector<vector<int>> g(V);
    auto add = [&](int u, int v, long long w) {
        g[u].push_back((int)e.size()); e.push_back({v, 1, w});
        g[v].push_back((int)e.size()); e.push_back({u, 0, -w});
    };
    const double SCALE = 1e4;
    for (int s = 0; s < N; ++s) {
        add(S, s, 0);
        add(N + s, T, 0);
        for (int t : superAdj[s]) if (t != startSuper) add(s, N + t, std::llround(c(s, t) * SCALE));
        if (mayEnd(s)) add(s, N + startSuper, std::llround(neutral * SCALE));
    }
    const long long INF = std::numeric_limits<long long>::max() / 4;
    vector<long long> h(V, 0), dist(V);
    vector<int> pe(V);
    int flow = 0;
    while (flow < N) {
        std::fill(dist.begin(), dist.end(), INF);
        dist[S] = 0;
        std::priority_queue<pair<long long, int>, vector<pair<long long, int>>, std::greater<>> pq;
        pq.push({0, S});
        while (!pq.empty()) {
            const long long d = pq.top().first; const int u = pq.top().second; pq.pop();
            if (d > dist[u]) continue;
            for (int id : g[u]) {
                const E& ed = e[id];
                if (!ed.cap) continue;
                const long long nd = d + ed.w + h[u] - h[ed.to];
                if (nd < dist[ed.to]) { dist[ed.to] = nd; pe[ed.to] = id; pq.push({nd, ed.to}); }
            }
        }
        if (dist[T] >= INF) break;
        for (int v = 0; v < V; ++v) if (dist[v] < INF) h[v] += dist[v];
        for (int v = T; v != S; v = e[pe[v] ^ 1].to) { e[pe[v]].cap -= 1; e[pe[v] ^ 1].cap += 1; }
        ++flow;
    }
    if (flow < N) { res.failure = "no perfect successor matching"; return res; }
    res.succ.assign(N, -1);
    for (int s = 0; s < N; ++s)
        for (int id : g[s]) {
            const E& ed = e[id];
            if (ed.to >= N && ed.to < 2 * N && ed.cap == 0 && (id % 2 == 0)) {
                const int t = ed.to - N;
                res.succ[s] = (t == startSuper) ? -1 : t;
                res.cost += (t == startSuper) ? neutral : c(s, t);
            }
        }

    // path from the start, then cycles
    vector<char> seen(N, 0);
    vector<int> path;
    for (int s = startSuper; s >= 0 && !seen[s]; s = res.succ[s]) { seen[s] = 1; path.push_back(s); }
    vector<vector<int>> cyc;
    for (int s = 0; s < N; ++s) {
        if (seen[s]) continue;
        vector<int> cy;
        for (int x = s; !seen[x]; x = res.succ[x]) { seen[x] = 1; cy.push_back(x); }
        cyc.push_back(cy);
    }
    res.cycles = (int)cyc.size();
    std::unordered_set<uint64_t> isEdge;
    vector<vector<int>> spred(N);
    for (int s = 0; s < N; ++s) for (int t : superAdj[s]) { isEdge.insert(ekey(s, t)); spred[t].push_back(s); }
    auto edge = [&](int s, int t) { return t < 0 ? mayEnd(s) : isEdge.count(ekey(s, t)) > 0; };
    // splice cycles, cheapest first, until none is left (or none fits)
    while (!cyc.empty()) {
        vector<int> pos(N, -1);
        for (int i = 0; i < (int)path.size(); ++i) pos[path[i]] = i;
        double best = std::numeric_limits<double>::infinity();
        int bc = -1, bk = -1, bi = -1;
        for (int ci = 0; ci < (int)cyc.size(); ++ci) {
            const auto& cy = cyc[ci];
            const int m = (int)cy.size();
            for (int k = 0; k < m; ++k) {        // cut edge cy[k] -> cy[k+1]: chain cy[k+1] .. cy[k]
                const int u = cy[k], v = cy[(k + 1) % m];
                for (int x : spred[v]) {
                    const int i = pos[x];
                    if (i < 0) continue;
                    const int y = (i + 1 < (int)path.size()) ? path[i + 1] : -1;
                    if (!edge(u, y)) continue;
                    const double d = c(x, v) + c(u, y) - c(x, y) - c(u, v);
                    if (d < best) { best = d; bc = ci; bk = k; bi = i; }
                }
            }
        }
        if (bc < 0) break;
        const auto cy = cyc[bc];
        const int m = (int)cy.size();
        vector<int> chain;
        for (int j = 1; j <= m; ++j) chain.push_back(cy[(bk + j) % m]);
        path.insert(path.begin() + bi + 1, chain.begin(), chain.end());
        res.cost += best;
        cyc.erase(cyc.begin() + bc);
        ++res.spliced;
    }
    if (!cyc.empty()) { res.failure = std::to_string(cyc.size()) + " cycle(s) could not be spliced in"; return res; }
    res.order = path;
    return res;
}

int main(int argc, char* argv[]) {
    const string filename = (argc > 1) ? argv[1] : FILENAME;

    Mp3FrameScanner scanner(filename);
    if (scanner.getFrameCount() == 0) {
        cerr << "No frames found\n";
        return 1;
    }

    long offset = static_cast<long>(scanner.getFrames()[0].position);
    ifstream file(filename, ios::binary);
    file.seekg(offset, ios::beg);


    // RP_CHUNK_SIZE overrides the default chunk size (bytes). Must be >= the largest frame so a
    // frame never spans more than one chunk boundary.
    long chunkSize = CHUNK_SIZE;
    if (const char* s = std::getenv("RP_CHUNK_SIZE")) chunkSize = std::max(1024L, std::strtol(s, nullptr, 10));
    if (chunkSize != CHUNK_SIZE) cout << "Chunk size: " << chunkSize << " bytes\n";
    vector<uint8_t> buf(chunkSize);
    while (file.read(reinterpret_cast<char*>(buf.data()), chunkSize) || file.gcount() > 0) {
        size_t n = file.gcount();
        chunks.emplace_back(buf.begin(), buf.begin() + static_cast<long>(n));
    }

    // Constant-bitrate detection (A3a): walk the first few chunks (still in true order here)
    // and, if every frame shares one bitrate, enable the frame-header bitrate filter in
    // StreamProfile::matches so false-positive syncs at other bitrates are rejected.
    bool cbr = false;
    int  cbrKbps = 0;
    {
        StreamProfile tmp{};
        if (deriveProfile(chunks[0], tmp)) {
            vector<uint8_t> head;
            for (size_t i = 0; i < chunks.size() && i < 6; ++i)
                head.insert(head.end(), chunks[i].begin(), chunks[i].end());
            cbr = detectCBR(head, tmp, cbrKbps);
        }
    }
    if (const char* s = std::getenv("RP_CBR")) if (s[0] == '0') cbr = false; // A/B toggle for A3a
    if (cbr)
        cout << "Detected constant bitrate: " << cbrKbps
             << " kbps - frame-header bitrate filter enabled\n";

    // --- Pre-shuffle sanity check ---
    int preShuffleBreaks = -1; // -1 = not computed; >=0 = broken consecutive-pair count on true order
    {
        StreamProfile prof0{};
        if (deriveProfile(chunks[0], prof0)) {
            if (cbr) { prof0.isCBR = true; prof0.bitrateKbps = cbrKbps; }
            vector<ChunkMeta> origMetas;
            origMetas.reserve(chunks.size());
            for (int i = 0; i < static_cast<int>(chunks.size()); ++i)
                origMetas.push_back(computeChunkMeta(i, chunks[i], prof0, chunks[i].size() < chunks[0].size()));

            auto origAdj = buildAdjacency(origMetas);
            int broken = 0;
            for (int i = 0; i + 1 < static_cast<int>(origMetas.size()); ++i) {
                if (!canFollow(origMetas[i], origMetas[i+1])) {
                    cout << "  BREAK at " << i << "->" << i+1
                         << ": tailOvfl=" << origMetas[i].tailOverflow
                         << " tailPartialLen=" << origMetas[i].tailPartialLen
                         << " headOff=" << origMetas[i+1].headOffset << "\n";
                    ++broken;
                }
            }
            preShuffleBreaks = broken;
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
                const int sideInfo0     = sideInfoSize(prof0.versionID, prof0.isMono());
                const int maxReservoir0 = (prof0.versionID == 0b11) ? 511 : 255;
                int level = 0;
                int reservoirBreaks = 0;
                for (int i = 0; i < static_cast<int>(origMetas.size()); ++i) {
                    const ChunkMeta* prev = (i > 0) ? &origMetas[i - 1] : nullptr;
                    if (!advanceReservoirChunk(level, prev, origMetas[i], chunks[i], sideInfo0, maxReservoir0)) {
                        cout << "  RESERVOIR BREAK at chunk " << i << "\n";
                        ++reservoirBreaks;
                    }
                }
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
    unsigned seed = 1;
    bool randomSeed = false;
    if (const char* s = std::getenv("RP_SEED")) {
        if (std::string(s) == "random") randomSeed = true;
        else seed = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
    }
    if (randomSeed) seed = static_cast<unsigned>(chrono::system_clock::now().time_since_epoch().count());

    // origIndexOf[i] = original (pre-shuffle) index of the chunk now at position i. Shuffled
    // with the same seed/engine as `chunks`, so it stays in lock-step and lets us score the
    // reconstruction against the true order at the end (C2).
    vector<int> origIndexOf(chunks.size());
    for (size_t i = 0; i < origIndexOf.size(); ++i) origIndexOf[i] = static_cast<int>(i);
    shuffle(chunks.begin() + 1, chunks.end(), default_random_engine(seed));
    shuffle(origIndexOf.begin() + 1, origIndexOf.end(), default_random_engine(seed));

    cout << "Total chunks: " << chunks.size() << "\n";
    cout << "Shuffle seed: " << seed << (randomSeed ? " (random)" : " (fixed default; RP_SEED to override)") << "\n";

    // Derive stream profile from chunk 0 (pinned, never shuffled)
    StreamProfile profile{};
    if (!deriveProfile(chunks[0], profile)) {
        cerr << "Could not derive stream profile from first chunk\n";
        return 1;
    }
    if (cbr) { profile.isCBR = true; profile.bitrateKbps = cbrKbps; }
    cout << "Stream profile: versionID=" << static_cast<int>(profile.versionID)
         << " layerID=" << static_cast<int>(profile.layerID)
         << " sampleRate=" << profile.sampleRate
         << " mono=" << profile.isMono()
         << (profile.isCBR ? (" cbr=" + std::to_string(profile.bitrateKbps) + "kbps") : string(" vbr")) << "\n";

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

    // Boundary granule loudness for the DFS continuity heuristic (see BoundaryGain / dfs()).
    // Off only if RP_RANK=0 - kept as a switch for the pinned-seed A/B in
    // PLAN_sideinfo_mainbits.md Step 1.
    const bool rankByContinuity = [] {
        const char* v = std::getenv("RP_RANK");
        return !(v && v[0] == '0');
    }();
    // Milestone 2 spectral-continuity term in the ranking (see spectralJunctionCost). RP_SPEC=0
    // disables it for A/B.
    g_sampleRate = profile.sampleRate;
    if (const char* v = std::getenv("RP_SPEC")) g_specRank = (v[0] != '0');
    if (const char* v = std::getenv("RP_SPEC_SCALE")) g_specScale = atof(v);
    vector<BoundaryGain> boundaryGain(chunks.size());
    if (profile.isLayerIII()) {
        const bool mpeg1 = profile.versionID == 0b11;
        const bool mono  = profile.isMono();
        for (size_t i = 0; i < metas.size(); ++i) {
            if (!metas[i].valid || metas[i].frames.empty()) continue;
            const FrameSideInfo f0 = parseFrameSideInfoAt(chunks[i], metas[i].frames.front().offset, mpeg1, mono);
            const FrameSideInfo f1 = parseFrameSideInfoAt(chunks[i], metas[i].frames.back().offset,  mpeg1, mono);
            if (f0.valid) {
                boundaryGain[i].ggFirst = f0.gr[0][0].globalGain;
                boundaryGain[i].bvFirst = f0.gr[0][0].bigValues;
            }
            if (f1.valid) {
                boundaryGain[i].ggLast = f1.gr[f1.numGranules - 1][0].globalGain;
                boundaryGain[i].bvLast = f1.gr[f1.numGranules - 1][0].bigValues;
            }
        }
    }

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

    // Build adjacency graph
    auto adj    = buildAdjacency(metas);
    const auto adjFull = adj;   // before AC-3 / locking - the repair pass may undo either

    // Arc-consistency prune (A4) - shrinks the branch-point count before the search. It is
    // only sound when `canFollow` has no false negatives among the true consecutive pairs
    // (otherwise it can force a wrong edge where a real predecessor is missing from the
    // graph). Gate on the pre-shuffle diagnostic: skip when it found breaks. RP_AC3=0 forces
    // off, RP_AC3=1 forces on regardless of the gate.
    {
        const char* e = std::getenv("RP_AC3");
        const bool force = e && e[0] == '1';
        const bool off   = e && e[0] == '0';
        const bool safe  = preShuffleBreaks == 0;
        if (off) {
            // disabled
        } else if (!safe && !force) {
            cout << "Arc consistency: skipped (" << (preShuffleBreaks < 0 ? "no pre-shuffle diagnostic"
                 : "pre-shuffle found breaks - canFollow is lossy on this file") << "; RP_AC3=1 to force)\n";
        } else {
            const long acRemoved = propagateArcConsistency(adj, static_cast<int>(chunks.size()), 0);
            if (acRemoved < 0)
                cout << "Arc consistency: contradiction (canFollow is missing a real edge) - graph left as built\n";
            else
                cout << "Arc consistency: removed " << acRemoved << " edges\n";
        }
    }

    // Reservoir parameters for this stream, computed once up front and threaded through
    // every DFS call rather than recomputed per edge.
    const bool isLayerIII   = profile.isLayerIII();
    const int  sideInfo     = sideInfoSize(profile.versionID, profile.isMono());
    const int  maxReservoir = (profile.versionID == 0b11) ? 511 : 255;

    if (const char* s = std::getenv("RP_MAX_NODES")) g_maxNodes = std::strtoull(s, nullptr, 10);
    if (const char* s = std::getenv("RP_PROGRESS")) {
        const uint64_t v = std::strtoull(s, nullptr, 10);
        if (v) g_progressEvery = v;
    }
    if (const char* s = std::getenv("RP_BESTOF")) g_bestOf = (s[0] != '0'); // OFF by default - see g_bestOf

    // Auto-calibrate the spectral term: sample random (prev, a, b) triples, take the median
    // raw L2 distance, and scale so a median junction contributes SPEC_TARGET (~a gentle
    // nudge, well below a typical |Δglobal_gain|). Removes the magic constant and adapts per
    // file. RP_SPEC_SCALE overrides the result.
    if (g_specRank && isLayerIII && !std::getenv("RP_SPEC_SCALE")) {
        vector<int> validChunks;
        for (int i = 0; i < static_cast<int>(chunks.size()); ++i)
            if (metas[i].valid && !metas[i].frames.empty()) validChunks.push_back(i);
        if (validChunks.size() >= 8) {
            const double savedScale = g_specScale;
            g_specScale = 1.0; // raw L2
            std::mt19937 rc(12345);
            std::uniform_int_distribution<int> pc(0, static_cast<int>(validChunks.size()) - 1);
            vector<double> d;
            for (int t = 0; t < 3000 && d.size() < 800; ++t) {
                const int p = validChunks[pc(rc)], a = validChunks[pc(rc)], b = validChunks[pc(rc)];
                if (a == b || a == p) continue;
                const double v = spectralJunctionCost(metas, p, a, b);
                if (v > 0) d.push_back(v);
            }
            if (d.size() >= 30) {
                std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
                const double med = d[d.size() / 2];
                constexpr double SPEC_TARGET = 3.0;
                g_specScale = med > 1e-6 ? SPEC_TARGET / med : savedScale;
            } else {
                g_specScale = savedScale;
            }
            g_specHead.clear();
            g_specTail.clear();
            cout << "Spectral term: calibrated scale = " << fixed << setprecision(2) << g_specScale
                 << " (" << d.size() << " samples)\n";
        }
    }

    // Calibrate the PCM term against the existing ranking cost (z-score style): over a sample
    // of real structural edges a->b with a plausible 2-chunk path context (pp->p->a, drawn from
    // the graph's in-edges), scale log1p(flux) so its spread is RP_PCM_W times the spread of
    // junctionCost + spectral. Unmeasurable junctions get the median (neutral) value.
    if (const char* v = std::getenv("RP_PCM")) g_pcmRank = (v[0] != '0');
    if (g_pcmRank && rankByContinuity) {
        double weight = 2.0;
        if (const char* v = std::getenv("RP_PCM_W")) weight = atof(v);
        const int n = static_cast<int>(chunks.size());
        vector<vector<int>> preds(n);
        for (int a = 0; a < n; ++a) for (int b : adj[a]) preds[b].push_back(a);
        std::mt19937 rc(777);
        std::uniform_int_distribution<int> pa(0, n - 1);
        vector<double> pv, ov;
        for (int t = 0; t < 20000 && pv.size() < 600; ++t) {
            const int a = pa(rc);
            if (adj[a].size() < 2 || preds[a].empty()) continue;
            const int p = preds[a][rc() % preds[a].size()];
            if (p == a || preds[p].empty()) continue;
            const int pp = preds[p][rc() % preds[p].size()];
            if (pp == p || pp == a) continue;
            const int b = adj[a][rc() % adj[a].size()];
            if (b == a || b == p || b == pp) continue;
            const int left[3] = {pp, p, a};
            const double raw = pcmRaw(metas, left, 3, b);
            if (raw < 0) continue;
            double o = junctionCost(boundaryGain, a, b);
            if (g_specRank && isLayerIII) o += spectralJunctionCost(metas, p, a, b);
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
            g_pcmScale = sp > 1e-9 ? weight * std::max(so, 1.0) / sp : 0.0;
            vector<double> tmp = pv;
            std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
            g_pcmNeutral = tmp[tmp.size() / 2];
        } else {
            g_pcmScale = weight * 10.0;   // too few samples: just make it dominant-ish
            g_pcmNeutral = 0.7;
        }
        g_pcmMemo.clear();
        cout << "PCM term: weight " << weight << ", scale = " << fixed << setprecision(2) << g_pcmScale
             << ", neutral = " << g_pcmNeutral << " (" << pv.size() << " calibration edges)\n";
    } else {
        g_pcmRank = false;
    }

    // Confident-edge locking (PLAN_full_pcm_decode.md step 5): force edges whose full-PCM
    // junction flux beats every competitor - on both the successor and predecessor side - by
    // a factor >= tau (median over the predecessor contexts), iterated with AC-3. A wrong lock
    // makes the true order unreachable for the DFS, so thresholds are tried strictest first:
    // if no complete ordering is found at one tau, the whole lock + search is retried at the
    // next (lower) one. A complete-but-wrong ordering from a too-low tau is then the repair
    // pass's job (it works on the FULL graph, so it can undo a wrong lock). RP_LOCK_TAU sets
    // the schedule (comma list, e.g. "1.5" for a single attempt); RP_LOCK=0 disables.
    const char* lockEnv = std::getenv("RP_LOCK");
    // Same soundness gate as AC-3: with a real edge missing from the graph (pre-shuffle
    // breaks) the predecessor-side margin can be won unopposed by a wrong edge.
    bool lockOn = !(lockEnv && lockEnv[0] == '0') && profile.isLayerIII() &&
                  (preShuffleBreaks == 0 || (lockEnv && lockEnv[0] == '1'));
    if (!lockOn && !(lockEnv && lockEnv[0] == '0') && profile.isLayerIII())
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
        long cap = 640000;
        if (const char* c = std::getenv("RP_LOCK_MAX_DECODES")) cap = atol(c);
        if (estimate > cap && !(lockEnv && lockEnv[0] == '1')) {
            cout << "Edge locking: skipped (~" << estimate << " decodes/round > " << cap
                 << "; RP_LOCK=1 forces, RP_LOCK_MAX_DECODES raises the cap)\n";
            lockOn = false;
        }
    }
    vector<double> lockTaus = {1.5, 1.25, 1.1};
    if (const char* t = std::getenv("RP_LOCK_TAU")) {
        lockTaus.clear();
        string spec(t);
        size_t at = 0;
        while (at < spec.size()) {
            size_t comma = spec.find(',', at);
            if (comma == string::npos) comma = spec.size();
            const double v = atof(spec.substr(at, comma - at).c_str());
            if (v > 0) lockTaus.push_back(v);
            at = comma + 1;
        }
        if (lockTaus.empty()) lockTaus = {1.5};
    }
    if (!lockOn) lockTaus = {0.0};  // single attempt, no locking
    int lockRounds = 8;  // re-lock on the pruned graph until nothing new locks
    if (const char* r = std::getenv("RP_LOCK_ROUNDS")) lockRounds = std::max(1, atoi(r));
    const char* acEnv = std::getenv("RP_AC3");
    const bool acOk = !(acEnv && acEnv[0] == '0') && (preShuffleBreaks == 0 || (acEnv && acEnv[0] == '1'));

    // Window fluxes are shared across lock rounds and tau attempts (a window's flux depends only
    // on its three chunks), and scored on RP_THREADS threads (default: all cores).
    EdgeLock::FluxMemo lockMemo;
    int lockThreads = std::max(1u, std::thread::hardware_concurrency());
    if (const char* t = std::getenv("RP_THREADS")) lockThreads = std::max(1, atoi(t));

    const auto adjBase = adj;
    vector<vector<int>> superAdj;
    vector<Supernode> supernodes;
    vector<int> result;
    bool found = false;
    size_t foundAttempt = 0;   // which lock attempt produced the ordering (repair policy below)
    bool foundByMatch = false;  // ordering came from the global matching (repair always runs)
    vector<int> matchSeq;       // its chunk sequence (possibly already repaired)
    double resultGap = std::numeric_limits<double>::quiet_NaN();   // repair's stability margin
    // Deepest partial path over ALL lock attempts, in chunks (each attempt has its own
    // supernode numbering): a late attempt that dead-ends at once mustn't hide an earlier
    // attempt's long partial path in the failure report.
    vector<int> bestPartial;   // chunk indices
    size_t bestPartialSupers = 0, bestPartialOf = 0;
    auto keepDeepest = [&]() {
        vector<int> p;
        for (int si : g_deepestOrder) for (int ci : supernodes[si].chunks) p.push_back(ci);
        if (p.size() > bestPartial.size()) {
            bestPartial = std::move(p);
            bestPartialSupers = g_deepestOrder.size();
            bestPartialOf = supernodes.size();
        }
    };
    for (size_t lockAttempt = 0; lockAttempt < lockTaus.size() && !found; ++lockAttempt) {
        foundAttempt = lockAttempt;
        const double tau = lockTaus[lockAttempt];
        adj = adjBase;
        if (lockAttempt > 0) keepDeepest();
        g_deepestOrder.clear();
        g_haveDeadline = false;   // each lock attempt gets its own search budget
        if (lockTaus.size() > 1)
            cout << "\n--- lock attempt " << lockAttempt + 1 << "/" << lockTaus.size() << " (tau " << tau << ") ---\n";
        if (tau > 0) {
            const auto t0 = chrono::steady_clock::now();
            long totalLocks = 0, totalTrue = 0, totalDec = 0;
            for (int round = 1; round <= lockRounds; ++round) {
                long dec = 0;
                const auto locks = EdgeLock::findConfidentEdges(chunks, metas, adj, 0, tau, 8,
                                                                EdgeLock::Mode::Median, &dec,
                                                                &lockMemo, lockThreads);
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
        cout << "\nAdjacency graph: " << edges << " edges, "
             << forced << "/" << chunks.size()
             << " chunks have exactly one valid successor\n";

        // Find which supernode contains chunk 0
        superAdj.clear();
        supernodes = buildSupernodes(
            adj, indeg, outdeg, static_cast<int>(chunks.size()), 0, superAdj);

        cout << "Supernodes after chain collapse: " << supernodes.size() << "\n";

        int startSuper = -1;
        for (int si = 0; si < static_cast<int>(supernodes.size()); ++si)
            if (supernodes[si].chunks[0] == 0) { startSuper = si; break; }

        if (startSuper == -1) {
            cerr << "Could not find supernode for chunk 0\n";
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
                g_deepestOrder.assign(1, startSuper);
                continue;
            }
            if (!startOk) {
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
        if (const char* s = std::getenv("RP_BEAM")) {
            beamSchedule = { std::max(0, atoi(s)) };
        } else {
            broaden = supernodes.size() > 120;
            if (const char* b = std::getenv("RP_BROADEN")) broaden = (b[0] != '0');
            beamSchedule = broaden ? vector<int>{1, 2, 3, 4, 6, 9, 0} : vector<int>{0};
        }

        auto search = [&]() {
            // Wall-clock budget for the whole search. Needed whenever the search is not a plain
            // first-valid exhaustive DFS: broadening's wider attempts and best-of both keep going
            // past the first complete ordering and would otherwise not terminate on large graphs.
            // RP_TIME_BUDGET (seconds) overrides; RP_MAX_NODES (node cap) also bounds it.
            if (g_maxNodes == 0 && (broaden || g_bestOf)) {
                double budget = broaden ? 45.0 : 30.0;
                if (const char* t = std::getenv("RP_TIME_BUDGET")) budget = atof(t);
                g_deadline = chrono::steady_clock::now() +
                             chrono::duration_cast<chrono::steady_clock::duration>(chrono::duration<double>(budget));
                g_haveDeadline = true;
            } else if (const char* t = std::getenv("RP_TIME_BUDGET")) {
                g_deadline = chrono::steady_clock::now() +
                             chrono::duration_cast<chrono::steady_clock::duration>(chrono::duration<double>(atof(t)));
                g_haveDeadline = true;
            } else if (g_maxNodes == 0 && lockTaus.size() > 1) {
                // With a lock schedule, no attempt may run an unbounded exhaustive DFS: locking can
                // shrink a large graph below the broadening threshold (test7_short at tau 1.25:
                // 108 supernodes, ran >500 s), which would drop rp's "always terminates" guarantee.
                // 30 s per attempt with a fallback left, 45 s for the last one.
                const double budget = (lockAttempt + 1 < lockTaus.size()) ? 30.0 : 45.0;
                g_deadline = chrono::steady_clock::now() +
                             chrono::duration_cast<chrono::steady_clock::duration>(chrono::duration<double>(budget));
                g_haveDeadline = true;
            }

            cout << "Starting search over " << supernodes.size() << " supernodes"
                 << " (continuity ranking " << (rankByContinuity ? "ON" : "OFF")
                 << ", " << (g_bestOf ? "best-of" : "first-valid")
                 << (g_specRank && isLayerIII ? ", +spectral" : "")
                 << (g_pcmRank ? ", +pcm" : "") << ")\n";

            g_bestScore = LLONG_MAX;
            g_bestOrder.clear();
            for (size_t attempt = 0; attempt < beamSchedule.size(); ++attempt) {
                if (g_haveDeadline && chrono::steady_clock::now() >= g_deadline) {
                    cout << "  (time budget spent - skipping remaining beam widths)\n";
                    break;
                }
                g_beamK   = beamSchedule[attempt];
                counter   = 0;
                g_aborted = false;
                DfsState state = primed;
                vector<int> res;
                g_dfsStart = chrono::steady_clock::now();

                cout << "  beam=" << (g_beamK ? std::to_string(g_beamK) : string("unlimited")) << " ... " << flush;
                const bool hit = dfs(state, supernodes, superAdj, metas, boundaryGain, rankByContinuity,
                                     sideInfo, maxReservoir, isLayerIII, res);

                if (g_bestOf) {
                    found = !g_bestOrder.empty();
                    cout << counter << " nodes, " << fixed << setprecision(2) << dfsElapsedSec() << "s";
                    if (found) cout << ", best score=" << g_bestScore;
                    else       cout << (g_aborted ? " (stopped: budget)" : " (exhausted, no ordering)");
                    cout << "\n" << flush;
                    if (found && g_bestScore == 0) break;       // perfect continuity - cannot improve
                } else {
                    if (hit) { result = res; found = true; }
                    cout << counter << " nodes, " << fixed << setprecision(2) << dfsElapsedSec() << "s"
                         << (found ? " -> FOUND" : (g_aborted ? " (stopped: budget)" : " (exhausted, no ordering)"))
                         << "\n" << flush;
                    if (found) break;
                }
            }
            if (g_bestOf && found) result = g_bestOrder;
        };

        // Global successor matching (RP_MATCH): 1 (default) = when this attempt's DFS fails,
        // 2 = before the DFS, 0 = off. Its ordering is used if it passes the reservoir check;
        // otherwise the DFS is rerun (fresh budget) with each matched successor ranked first.
        auto tryMatch = [&]() -> bool {
            const auto t0 = chrono::steady_clock::now();
            MatchResult mr = globalMatch(supernodes, superAdj, startSuper, adj, metas, lockMemo, lockThreads);
            const double secs = chrono::duration<double>(chrono::steady_clock::now() - t0).count();
            cout << "Global matching: " << supernodes.size() << " supernodes, " << mr.decodes << " decodes ("
                 << mr.contexts << " ctx), "
                 << mr.cycles << " cycle(s), " << mr.spliced << " spliced, cost " << fixed << setprecision(3)
                 << mr.cost << ", " << setprecision(2) << secs << "s";
            if (mr.order.empty()) { cout << " - " << mr.failure << "\n"; }
            else {
                vector<int> seqm;
                for (int si : mr.order) for (int ci : supernodes[si].chunks) seqm.push_back(ci);
                int tj = 0;
                for (size_t k = 1; k < seqm.size(); ++k) if (origIndexOf[seqm[k]] == origIndexOf[seqm[k - 1]] + 1) ++tj;
                const bool ok = !isLayerIII || reservoirValid(metas, seqm, sideInfo, maxReservoir);
                cout << (ok ? " - reservoir OK" : " - reservoir check FAILED")
                     << " [vs source: " << tj << "/" << (seqm.size() - 1) << " true junctions]\n";
                // Repair (moves on the full structural graph). If the order is reservoir-invalid
                // - usual, it is nearly right but a few wrong junctions break the reservoir -
                // the cost is led by the number of reservoir breaks.
                if (!(std::getenv("RP_REPAIR") && std::getenv("RP_REPAIR")[0] == '0')) {
                    int passes = 50;
                    if (const char* v = std::getenv("RP_REPAIR_PASSES")) passes = std::max(1, atoi(v));
                    const auto r0 = chrono::steady_clock::now();
                    double gap = std::numeric_limits<double>::quiet_NaN();
                    const int mv = repairOrdering(seqm, metas, adjFull, isLayerIII, sideInfo, maxReservoir, passes, &gap);
                    const bool ok2 = reservoirValid(metas, seqm, sideInfo, maxReservoir);
                    int tj2 = 0;
                    for (size_t k = 1; k < seqm.size(); ++k) if (origIndexOf[seqm[k]] == origIndexOf[seqm[k - 1]] + 1) ++tj2;
 (void)ok;
                    cout << "Matching repair: " << mv << " move(s), " << fixed << setprecision(2)
                         << chrono::duration<double>(chrono::steady_clock::now() - r0).count() << "s - "
                         << (ok2 ? "reservoir OK" : "still invalid") << " [vs source: " << tj2 << "/"
                         << (seqm.size() - 1) << " true junctions]\n";
                    if (ok2) { result = mr.order; matchSeq = seqm; resultGap = gap; return true; }
                } else if (ok) { result = mr.order; matchSeq = seqm; return true; }
            }
            if (mr.succ.empty()) return false;
            g_matchSucc = mr.succ;
            g_haveDeadline = false;
            cout << "Search with the matching as ranking hint:\n";
            search();
            g_matchSucc.clear();
            return found;
        };
        int matchMode = 1;
        if (const char* m = std::getenv("RP_MATCH")) matchMode = atoi(m);
        if (!g_pcmRank) matchMode = 0;
        if (matchMode == 2 && tryMatch()) { found = true; foundByMatch = true; continue; }
        search();
        if (!found && matchMode == 1 && tryMatch()) { found = true; foundByMatch = true; }
    } // lock attempts

    if (found) {
        cout << "Reconstruction found" << (g_bestOf ? (" (score " + std::to_string(g_bestScore) + ")") : "") << "\n";
        ofstream out("output.mp3", ios::binary | ios::trunc);
        if (!out) { cerr << "Cannot open output.mp3\n"; return 1; }

        vector<int> seq; // reconstructed chunk order
        if (foundByMatch && !matchSeq.empty()) seq = matchSeq;
        else for (int si : result) for (int ci : supernodes[si].chunks) seq.push_back(ci);

        // Repair policy: with a tau schedule, an ordering found by the FIRST (strictest)
        // attempt is left alone. The benchmark (BENCHMARK_pcm_vs_main.md) found the repair
        // only helped orderings from a fallback tau (wrong locks / wrong block), and on one
        // first-attempt success (cbr32_10s, 1 KB) it moved the exact order to a lower-flux
        // wrong one - the true order is not always the flux minimum. RP_REPAIR=1 forces it,
        // =0 disables; a single-tau run keeps the old always-on behaviour.
        const char* rp = std::getenv("RP_REPAIR");
        const bool repairForced = rp && rp[0] == '1';
        const bool firstOfSchedule = lockTaus.size() > 1 && foundAttempt == 0 && !foundByMatch;
        if (foundByMatch && !repairForced)
            cout << "Block-move repair: skipped (the matching's ordering was already repaired)\n";
        else if (g_pcmRank && !(rp && rp[0] == '0') && firstOfSchedule && !repairForced)
            cout << "Block-move repair: skipped (found by the strictest lock attempt; RP_REPAIR=1 forces)\n";
        else if (g_pcmRank && !(rp && rp[0] == '0')) {
            int passes = 50;
            if (const char* v = std::getenv("RP_REPAIR_PASSES")) passes = std::max(1, atoi(v));
            const auto t0 = chrono::steady_clock::now();
            const int moves = repairOrdering(seq, metas, adjFull, isLayerIII, sideInfo, maxReservoir, passes, &resultGap);
            cout << "Block-move repair: " << moves << " move(s), " << fixed << setprecision(2)
                 << chrono::duration<double>(chrono::steady_clock::now() - t0).count() << "s\n";
        }

        vector<int> reconOrig; // original index of each chunk, in reconstructed order
        reconOrig.reserve(chunks.size());
        size_t total = 0;
        for (int ci : seq) {
            out.write(reinterpret_cast<const char*>(chunks[ci].data()),
                      static_cast<long>(chunks[ci].size()));
            total += chunks[ci].size();
            reconOrig.push_back(origIndexOf[ci]);
        }
        cout << "Written " << total << " bytes to output.mp3\n";
        // Provenance label (a line of its own - regression.sh/parse.py grep the lines around it).
        // A global-matching ordering is the min-cost assignment patched + repaired: reservoir-
        // valid, but nothing proves it is the original order, and on some files a wrong order is
        // cheaper than the true one (Queen 4 KB, cbr192_10s 2 KB). The gap is the summed-flux
        // margin to the nearest valid ordering one repair move away (small = ambiguous).
        cout << "Result provenance: " << (foundByMatch ? "best-effort (global matching + repair)" : "DFS search");
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
        const bool exactMatch = (reconOrig.size() == chunks.size()) &&
                                std::is_sorted(reconOrig.begin(), reconOrig.end());
        cout << "Reconstruction vs source: " << exactPos << "/" << reconOrig.size()
             << " chunks in original position ("
             << fixed << setprecision(1) << (100.0 * exactPos / std::max<size_t>(1, reconOrig.size())) << "%)"
             << (exactMatch ? "  --  EXACT (output.mp3 is byte-identical to the source stream)" : "")
             << "\n";
        cout << "Reconstruction junctions: " << trueJ << "/" << (reconOrig.empty() ? 0 : reconOrig.size() - 1)
             << " are true successor pairs\n";
        // Diagnostic (RP_SCORE_ORDER=1): summed PCM flux of the found ordering vs the source
        // order, each junction scored in its own exact (PP, P, cur) context. Tells whether a
        // wrong-but-valid result is something a flux-minimising repair could fix.
        if (std::getenv("RP_SCORE_ORDER") && !exactMatch) {
            vector<int> found = seq, truth(chunks.size());
            for (size_t ci = 0; ci < chunks.size(); ++ci) truth[origIndexOf[ci]] = static_cast<int>(ci);
            auto summed = [&](const vector<int>& ord, int& unmeasured) {
                double sum = 0; unmeasured = 0;
                for (size_t k = 1; k < ord.size(); ++k) {
                    int left[3], nl = 0;
                    for (int j = (int)k - 3; j < (int)k; ++j) if (j >= 0) left[nl++] = ord[j];
                    const double v = pcmRaw(metas, left, nl, ord[k]);
                    if (v < 0) ++unmeasured; else sum += v;
                }
                return sum;
            };
            for (size_t k = 1; k < found.size(); ++k)
                if (origIndexOf[found[k]] != origIndexOf[found[k - 1]] + 1)
                    cout << "  wrong junction at position " << k << ": orig " << origIndexOf[found[k - 1]]
                         << " -> " << origIndexOf[found[k]] << "\n";
            int uf = 0, ut = 0;
            const double sf = summed(found, uf), st = summed(truth, ut);
            cout << "Summed log1p(flux): found " << fixed << setprecision(3) << sf << " (" << uf
                 << " unmeasured), source " << st << " (" << ut << " unmeasured) -> "
                 << (st < sf ? "source is cheaper" : "found is cheaper or equal") << "\n";
        }
    } else {
        cout << "Failed to reconstruct (no complete ordering found"
             << (g_aborted ? " within the search budget" : "") << ").\n";
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
        cout << "Deepest partial path: " << bestPartialSupers << "/" << bestPartialOf << " supernodes, "
             << path.size() << " chunks; true junctions " << good << "/" << (path.empty() ? 0 : path.size() - 1)
             << "; correct prefix = " << (firstBad < 0 ? (int)path.size() : firstBad) << " chunks\n";
    }

    return 0;
}
