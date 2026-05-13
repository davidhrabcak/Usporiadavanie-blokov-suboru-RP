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
#define COUNTER_FREQUENCY 9999999 // modulus for counter with printouts during dfs

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
 * Validates the bit reservoir integrity for a specific sequence of MP3 frames.
 * @param order The order of MP3 frames to be validated.
 * @param metas Metadata for each chunk.
 * @return Returns true if the proposed chunk ordering produces a valid Layer III bit-reservoir chain.
 *         For non-Layer-III files always returns true.
 */
bool validateReservoir(const vector<int>& order,
                       const vector<ChunkMeta>& metas) {
    const StreamProfile& p = metas[order[0]].profile;
    if (!p.isLayerIII()) return true;

    const int sideInfo = sideInfoBytes(p);
    const int maxReservoir = (p.versionID == 0b11) ? 511 : 255;

    int accumulated = 0;
    for (int ci : order) {
        for (const FrameSlice& f : metas[ci].frames) {
            int produced = f.length - 4 - sideInfo;
            if (produced < 0) produced = 0;
            accumulated += produced;
            if (accumulated > maxReservoir) accumulated = maxReservoir;
            if (f.mainDataBegin > accumulated) return false;
        }
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
    int reservoirLevel = 0;   // unused: reservoir check runs on full order
};

static uint64_t counter = 0;

/**
 * Exhaustive DFS over the supernode graph, starting from the supernode containing chunk 0.
 * At a complete ordering, validateReservoir() runs a Layer III bit-reservoir check
 * (monotonically accumulates bytes produced per frame;
 * rejects if any frame's mainDataBegin exceeds the accumulated total).
 * @param state The current state of the search.
 * @param supernodes A list of supernode structures containing grouped MP3 chunks.
 * @param superAdj The adjacency list representing valid transitions between supernodes.
 * @param metas Metadata for all chunks.
 * @param result If found, the valid supernode sequence is filled into result.
 * @return True if a valid, decodable sequence of all supernodes exists.
 */
bool dfs(DfsState& state,
         const vector<Supernode>& supernodes,
         const vector<vector<int>>& superAdj,
         const vector<ChunkMeta>& metas,
         vector<int>& result) {

    if (state.order.size() == supernodes.size()) {
        // Flatten to chunk order and do final reservoir check
        vector<int> chunkOrder;
        for (const int si : state.order)
            for (int ci : supernodes[si].chunks)
                chunkOrder.push_back(ci);

        if (!validateReservoir(chunkOrder, metas)) return false;
        result = state.order;
        return true;
    }

    const int cur = state.order.back();
    const auto& candidates = superAdj[cur];

    for (int si : candidates) {
        if (state.used[si]) continue;

        state.used[si] = true;
        state.order.push_back(si);

         if (++counter % COUNTER_FREQUENCY == 0) {
             cout << "Depth: " << state.order.size()
                  << "/" << supernodes.size() << "\n";
         }

        if (dfs(state, supernodes, superAdj, metas, result)) return true;

        state.order.pop_back();
        state.used[si] = false;
    }
    return false;
}

int main(int argc, char* argv[]) {
    const string filename = "/home/david/Desktop/python/rp/sound/sample-3s.mp3";

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

    DfsState state;
    state.used.resize(supernodes.size(), false);
    state.order.push_back(startSuper);
    state.used[startSuper] = true;
    state.reservoirLevel = 0;

    vector<int> result;
    cout << "Starting DFS over " << supernodes.size() << " supernodes...\n";

    if (dfs(state, supernodes, superAdj, metas, result)) {
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
