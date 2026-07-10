#include <random>
#include <algorithm>
#include <csetjmp>
#include <chrono>
#include <string>
#include <cstdint>
#include <iostream>
#include <vector>
#include <fstream>
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
 * Determines the length of side information in a chunk.
 * @return Side information length in bytes.
 */
static int sideInfoBytes(const StreamProfile& p) {
    if (p.versionID == 0b11) return p.isMono() ? 17 : 32;
    return p.isMono() ? 9 : 17;
}

/**
 * Advances the Layer III bit-reservoir level through a single frame and checks that
 * the frame's mainDataBegin is actually satisfiable by what has accumulated so far.
 *
 * `level` is tracked in BITS, not bytes, even though callers (advanceReservoirChunk,
 * DfsState::reservoirLevel) treat it as an opaque running total - see below for why.
 *
 * level_after = clamp(f.mainDataBegin*8 + producedBits - f.part23Bits, 0, maxReservoir*8).
 * The critical detail (cross-checked against minimp3's L3_restore_reservoir/
 * L3_save_reservoir, a widely-used reference decoder): the "history" contribution to
 * level_after is `f.mainDataBegin`, NOT `level_before`. A frame only ever reaches back
 * exactly as far as its own mainDataBegin says; any reservoir surplus beyond that
 * (level_before - mainDataBegin, when the frame didn't need to reach all the way back) is
 * *not* carried forward - the decoder's reservoir buffer only ever keeps the trailing
 * leftover of the (mainDataBegin + produced) window actually used by this frame, so older,
 * unclaimed bytes are gone once a later frame's mainDataBegin doesn't reach them. Using
 * `level_before` instead of `f.mainDataBegin` here was an earlier, incorrect version of
 * this fix - it let unused surplus accumulate indefinitely, and produced a systematic
 * downward drift that rejected `test7_shorter.mp3`'s true, correct chunk order by chunk 7
 * (confirmed empirically: `level_before` and `f.mainDataBegin` diverge starting exactly
 * where a stretch of near-silent frames - each leaving most of a saturated reservoir
 * unclaimed - is followed by louder frames whose real consumption then gets checked
 * against a `level` that never should have kept that surplus).
 *
 * The check itself (`f.mainDataBegin > level_before`) stays: mainDataBegin(N) can never
 * exceed what was genuinely available after frame N-1.
 *
 * Bit-precision also matters: part2_3_length is a bit-granular quantity, so it and
 * `produced` are both kept in bits internally, only flooring to bytes when compared
 * against mainDataBegin (a byte-granular bitstream field) - rounding part2_3_length up to
 * bytes per frame (an earlier version of this fix did exactly that) adds up to ~7 bits of
 * phantom consumption on every frame, which also compounds into false rejections.
 *
 * A first attempt at this function subtracted mainDataBegin itself as a stand-in for
 * consumption (not as the reservoir's history anchor, which is what this version does -
 * a different thing). That regressed real reconstructions: instrumenting a known-good file
 * (sample-3s.mp3) showed mainDataBegin sitting near the 511-byte cap on nearly every
 * frame while `produced` was ~382 bytes/frame, so the subtract-based level went net
 * negative every frame and saturated at 0 within a handful of chunks - after which every
 * subsequent frame with mainDataBegin > 0 was (wrongly) rejected, and the true ordering
 * could no longer pass. part2_3_length is the correct quantity to subtract - validated by
 * replaying the true, unshuffled chunk order through this function (see the pre-shuffle
 * sanity check below) before trusting it inside the DFS.
 *
 * @param level  Current reservoir fill level in BITS; updated in place.
 * @param f      The frame to advance through.
 * @param sideInfo Side-information size in bytes for this stream (see sideInfoBytes).
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
 * Its payload bytes are nonetheless real, physical reservoir bytes, so this credits them on
 * both sides of the split: `meta.headOffset` bytes at the *start* of this chunk complete the
 * previous chunk's split tail (pure payload, no header/side-info overhead - the predecessor
 * already accounted for those), and `meta.tailOverflow` bytes at the *end* of this chunk are
 * the start of the next split frame's payload (once its 4-byte header + side info are
 * subtracted). Omitting either credit was confirmed (by cross-checking against minimp3, a
 * reference decoder, byte-for-byte on `test7_shorter.mp3`) to systematically undercount the
 * reservoir - the level drifts below the bitstream's own true mainDataBegin values within a
 * handful of chunks, which made this check reject the correct, unshuffled chunk order.
 *
 * The split frame's own mainDataBegin/part2_3_length are deliberately *not* validated or
 * consumed here (that would need to happen exactly once, using the combined view of both
 * chunks, which the current per-chunk loop structure doesn't have). Only crediting its
 * payload bytes without also modeling its consumption makes this conservative - it can
 * under-prune (miss a constraint a full model would catch) but cannot wrongly reject a
 * valid chain, which is what correctness requires here.
 *
 * @param level Current reservoir fill level in BITS; updated in place, even on failure
 *              (the caller must restore it from a saved copy rather than trust it after false).
 * @param meta  Metadata for the chunk being appended to the candidate order.
 * @param sideInfo Side-information size in bytes for this stream.
 * @param maxReservoir Reservoir capacity in bytes.
 * @return False as soon as any complete frame in this chunk is invalid; true otherwise.
 */
static bool advanceReservoirChunk(int& level, const ChunkMeta& meta, const int sideInfo, const int maxReservoir) {
    const int maxBits = maxReservoir * 8;

    if (meta.headOffset > 0) {
        level += meta.headOffset * 8;
        if (level > maxBits) level = maxBits;
    }

    for (const FrameSlice& f : meta.frames) {
        if (!advanceReservoirFrame(level, f, sideInfo, maxReservoir)) return false;
    }

    if (meta.tailPartialLen > 0 && meta.tailOverflow > 4 + sideInfo) {
        level += (meta.tailOverflow - 4 - sideInfo) * 8;
        if (level > maxBits) level = maxBits;
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
};

static uint64_t counter = 0;

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
         const int sideInfo,
         const int maxReservoir,
         const bool isLayerIII,
         vector<int>& result) {

    if (state.order.size() == supernodes.size()) {
        // Reservoir validity has already been established incrementally for every
        // chunk placed so far - nothing left to verify.
        result = state.order;
        return true;
    }

    const int cur = state.order.back();
    const auto& candidates = superAdj[cur];

    for (int si : candidates) {
        if (state.used[si]) continue;

        const int savedLevel = state.reservoirLevel;
        bool reservoirOk = true;
        if (isLayerIII) {
            for (int ci : supernodes[si].chunks) {
                if (!advanceReservoirChunk(state.reservoirLevel, metas[ci], sideInfo, maxReservoir)) {
                    reservoirOk = false;
                    break;
                }
            }
        }
        if (!reservoirOk) {
            state.reservoirLevel = savedLevel;
            continue;
        }

        state.used[si] = true;
        state.order.push_back(si);

         if (++counter % COUNTER_FREQUENCY == 0) {
             cout << "Depth: " << state.order.size()
                  << "/" << supernodes.size() << "\n";
         }

        if (dfs(state, supernodes, superAdj, metas, sideInfo, maxReservoir, isLayerIII, result)) return true;

        state.order.pop_back();
        state.used[si] = false;
        state.reservoirLevel = savedLevel;
    }
    return false;
}

int main(int argc, char* argv[]) {
    const string filename = FILENAME;

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

    // --- Pre-shuffle sanity check ---
    {
        StreamProfile prof0{};
        if (deriveProfile(chunks[0], prof0)) {
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
                const int sideInfo0     = sideInfoBytes(prof0);
                const int maxReservoir0 = (prof0.versionID == 0b11) ? 511 : 255;
                int level = 0;
                int reservoirBreaks = 0;
                for (int i = 0; i < static_cast<int>(origMetas.size()); ++i) {
                    if (!advanceReservoirChunk(level, origMetas[i], sideInfo0, maxReservoir0)) {
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

    unsigned seed = chrono::system_clock::now().time_since_epoch().count();
    shuffle(chunks.begin() + 1, chunks.end(), default_random_engine(seed));

    cout << "Total chunks: " << chunks.size() << "\n";

    // Derive stream profile from chunk 0 (pinned, never shuffled)
    StreamProfile profile{};
    if (!deriveProfile(chunks[0], profile)) {
        cerr << "Could not derive stream profile from first chunk\n";
        return 1;
    }
    cout << "Stream profile: versionID=" << static_cast<int>(profile.versionID)
         << " layerID=" << static_cast<int>(profile.layerID)
         << " sampleRate=" << profile.sampleRate
         << " mono=" << profile.isMono() << "\n";

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
    for (int i = 0; i < static_cast<int>(chunks.size()); ++i)
        if (outdeg[i] == 1) ++forced;
    cout << "\nAdjacency graph: "
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
    const int  sideInfo     = sideInfoBytes(profile);
    const int  maxReservoir = (profile.versionID == 0b11) ? 511 : 255;

    DfsState state;
    state.used.resize(supernodes.size(), false);
    state.order.push_back(startSuper);
    state.used[startSuper] = true;
    state.reservoirLevel = 0;

    // Prime the reservoir with chunk 0's own frames before the DFS explores any
    // successor - chunk 0 is pinned, so it is always the first thing placed.
    if (isLayerIII) {
        for (int ci : supernodes[startSuper].chunks) {
            if (!advanceReservoirChunk(state.reservoirLevel, metas[ci], sideInfo, maxReservoir)) {
                cerr << "Reservoir check failed on the pinned starting chunk - "
                        "mainDataBegin metadata is likely wrong for chunk 0\n";
                return 1;
            }
        }
    }

    vector<int> result;
    cout << "Starting DFS over " << supernodes.size() << " supernodes...\n";

    if (dfs(state, supernodes, superAdj, metas, sideInfo, maxReservoir, isLayerIII, result)) {
        cout << "Reconstruction found\n";
        ofstream out("output.mp3", ios::binary | ios::trunc);
        if (!out) { cerr << "Cannot open output.mp3\n"; return 1; }

        size_t total = 0;
        for (int si : result)
            for (int ci : supernodes[si].chunks) {
                out.write(reinterpret_cast<const char*>(chunks[ci].data()),
                          static_cast<long>(chunks[ci].size()));
                total += chunks[ci].size();
            }

        cout << "Written " << total << " bytes to output.mp3\n";
    } else {
        cout << "Failed to reconstruct.\n";
    }

    return 0;
}
