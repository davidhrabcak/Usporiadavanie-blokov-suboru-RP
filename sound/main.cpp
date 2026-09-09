#include <random>
#include <algorithm>
#include <csetjmp>
#include <chrono>
#include <string>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <vector>
#include <fstream>
#include <cstdlib>
#include <climits>
#include "frame_scanner.hpp"
#include "chunk_meta.hpp"
#include "adjacency.hpp"

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

// Per-chunk Layer III granule loudness at each end, for the continuity ranking heuristic.
// .first  = global_gain of granule 0 of the chunk's first complete frame
// .second = global_gain of the last granule of the chunk's last complete frame
// Either is -1 when unavailable (no complete frame / side info didn't fit). See the
// sideinfo_probe measurements in PLAN_sideinfo_mainbits.md Step 1: |Δ global_gain| across a
// chunk junction ranks the true successor #1 in ~60-74% of branch points on VBR fixtures
// (vs ~30% for the graph's own candidate order), so sorting DFS candidates by it explores
// the likely-correct edge first. Ranking only - never rejects an edge.
struct BoundaryGain { int first = -1; int last = -1; };

// Junction cost of putting a supernode whose first chunk is `b` right after a supernode whose
// last chunk is `a`: |Δ global_gain| across that boundary. When either side's loudness is
// unknown, a fixed penalty above the 255 max real delta - so an ordering that resolves more
// junctions scores better, unknowns sort last in the ranking, and the running sum stays
// finite (needed for the best-of branch-and-bound). Used for both the DFS exploration order
// and the total-ordering score.
static constexpr int UNKNOWN_JUNCTION_COST = 384;
static int junctionCost(const vector<BoundaryGain>& bg, const int a, const int b) {
    if (bg[a].last < 0 || bg[b].first < 0) return UNKNOWN_JUNCTION_COST;
    return std::abs(bg[a].last - bg[b].first);
}

// DFS instrumentation / bounding. `counter` = number of supernode placements tried.
// RP_MAX_NODES caps it (0 = unlimited) so a bounded A/B run stops cleanly instead of being
// SIGTERM'd (which loses buffered stdout); RP_PROGRESS overrides how often a progress line
// is printed. g_aborted unwinds the whole recursion once the cap is hit.
static uint64_t counter        = 0;
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

    // Try structurally-legal successors in order of granule-loudness continuity (smallest
    // junctionCost first). Pure exploration ordering - every candidate is still tried
    // (subject to the beam cap), and the reservoir check below is unchanged.
    vector<int> candidates(superAdj[cur].begin(), superAdj[cur].end());
    if (rankByContinuity && candidates.size() > 1) {
        stable_sort(candidates.begin(), candidates.end(), [&](const int x, const int y) {
            return junctionCost(boundaryGain, curTailChunk, supernodes[x].chunks.front())
                 < junctionCost(boundaryGain, curTailChunk, supernodes[y].chunks.front());
        });
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
        // Wall-clock stop, sampled sparsely (steady_clock::now is a syscall).
        if (g_haveDeadline && (counter & 0x3FFFF) == 0 && chrono::steady_clock::now() >= g_deadline)
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


    vector<uint8_t> buf(CHUNK_SIZE);
    while (file.read(reinterpret_cast<char*>(buf.data()), CHUNK_SIZE) || file.gcount() > 0) {
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
    {
        StreamProfile prof0{};
        if (deriveProfile(chunks[0], prof0)) {
            if (cbr) { prof0.isCBR = true; prof0.bitrateKbps = cbrKbps; }
            vector<ChunkMeta> origMetas;
            origMetas.reserve(chunks.size());
            for (int i = 0; i < static_cast<int>(chunks.size()); ++i)
                origMetas.push_back(computeChunkMeta(i, chunks[i], prof0));

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
        metas.push_back(computeChunkMeta(i, chunks[i], profile));
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
    vector<BoundaryGain> boundaryGain(chunks.size());
    if (profile.isLayerIII()) {
        const bool mpeg1 = profile.versionID == 0b11;
        const bool mono  = profile.isMono();
        for (size_t i = 0; i < metas.size(); ++i) {
            if (!metas[i].valid || metas[i].frames.empty()) continue;
            const FrameSideInfo f0 = parseFrameSideInfoAt(chunks[i], metas[i].frames.front().offset, mpeg1, mono);
            const FrameSideInfo f1 = parseFrameSideInfoAt(chunks[i], metas[i].frames.back().offset,  mpeg1, mono);
            if (f0.valid) boundaryGain[i].first = f0.gr[0][0].globalGain;
            if (f1.valid) boundaryGain[i].last  = f1.gr[f1.numGranules - 1][0].globalGain;
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
    vector<vector<int>> superAdj;
    vector<Supernode> supernodes = buildSupernodes(
        adj, indeg, outdeg, static_cast<int>(chunks.size()), 0, superAdj);

    cout << "Supernodes after chain collapse: " << supernodes.size() << "\n";

    int startSuper = -1;
    for (int si = 0; si < static_cast<int>(supernodes.size()); ++si)
        if (supernodes[si].chunks[0] == 0) { startSuper = si; break; }

    if (startSuper == -1) {
        cerr << "Could not find supernode for chunk 0\n";
        return 1;
    }

    // Reservoir parameters for this stream, computed once up front and threaded through
    // every DFS call rather than recomputed per edge.
    const bool isLayerIII   = profile.isLayerIII();
    const int  sideInfo     = sideInfoSize(profile.versionID, profile.isMono());
    const int  maxReservoir = (profile.versionID == 0b11) ? 511 : 255;

    // Primed starting state: chunk 0's supernode placed, reservoir advanced through its
    // own frames. Copied (cheap) per beam-schedule attempt below.
    DfsState primed;
    primed.used.assign(supernodes.size(), false);
    primed.order.push_back(startSuper);
    primed.used[startSuper] = true;
    primed.reservoirLevel = 0;
    if (isLayerIII) {
        const ChunkMeta* prev = nullptr; // chunk 0 is always first placed - no predecessor
        for (int ci : supernodes[startSuper].chunks) {
            if (!advanceReservoirChunk(primed.reservoirLevel, prev, metas[ci], chunks[ci], sideInfo, maxReservoir)) {
                cerr << "Reservoir check failed on the pinned starting chunk - "
                        "mainDataBegin metadata is likely wrong for chunk 0\n";
                return 1;
            }
            prev = &metas[ci];
        }
    }

    if (const char* s = std::getenv("RP_MAX_NODES")) g_maxNodes = std::strtoull(s, nullptr, 10);
    if (const char* s = std::getenv("RP_PROGRESS")) {
        const uint64_t v = std::strtoull(s, nullptr, 10);
        if (v) g_progressEvery = v;
    }
    if (const char* s = std::getenv("RP_BESTOF")) g_bestOf = (s[0] != '0'); // OFF by default - see g_bestOf

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
    }

    cout << "Starting search over " << supernodes.size() << " supernodes"
         << " (continuity ranking " << (rankByContinuity ? "ON" : "OFF")
         << ", " << (g_bestOf ? "best-of" : "first-valid") << ")\n";

    g_bestScore = LLONG_MAX;
    g_bestOrder.clear();
    vector<int> result;
    bool found = false;
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

    if (found) {
        cout << "Reconstruction found" << (g_bestOf ? (" (score " + std::to_string(g_bestScore) + ")") : "") << "\n";
        ofstream out("output.mp3", ios::binary | ios::trunc);
        if (!out) { cerr << "Cannot open output.mp3\n"; return 1; }

        vector<int> reconOrig; // original index of each chunk, in reconstructed order
        reconOrig.reserve(chunks.size());
        size_t total = 0;
        for (int si : result)
            for (int ci : supernodes[si].chunks) {
                out.write(reinterpret_cast<const char*>(chunks[ci].data()),
                          static_cast<long>(chunks[ci].size()));
                total += chunks[ci].size();
                reconOrig.push_back(origIndexOf[ci]);
            }
        cout << "Written " << total << " bytes to output.mp3\n";

        // C2: score the reconstruction against the true (pre-shuffle) order.
        int exactPos = 0;
        for (size_t i = 0; i < reconOrig.size(); ++i)
            if (reconOrig[i] == static_cast<int>(i)) ++exactPos;
        const bool exactMatch = (reconOrig.size() == chunks.size()) &&
                                std::is_sorted(reconOrig.begin(), reconOrig.end());
        cout << "Reconstruction vs source: " << exactPos << "/" << reconOrig.size()
             << " chunks in original position ("
             << fixed << setprecision(1) << (100.0 * exactPos / std::max<size_t>(1, reconOrig.size())) << "%)"
             << (exactMatch ? "  --  EXACT (output.mp3 is byte-identical to the source stream)" : "")
             << "\n";
    } else {
        cout << "Failed to reconstruct (no complete ordering found"
             << (g_aborted ? " within the search budget" : "") << ").\n";
    }

    return 0;
}
