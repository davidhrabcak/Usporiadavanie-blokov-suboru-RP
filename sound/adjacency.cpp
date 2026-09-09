#include "adjacency.hpp"
#include <algorithm>
#include <unordered_map>

#include "frame_scanner.hpp"

using namespace std;

/**
 * Checks if chunk 'b' has a known valid frame start in position 'rem'
 * @param  b Chunk where the frame may be found.
 * @param rem Index of possible frame start.
 * @return True if position rem is a known valid frame start in chunk b.
 */
static bool hasFrameAt(const ChunkMeta& b, const int rem) {
    // frameStarts is sorted ascending (built in computeChunkMeta scan order).
    const auto it = lower_bound(b.frameStarts.begin(), b.frameStarts.end(), rem);
    return it != b.frameStarts.end() && *it == rem;
}

/**
 * Reconstructs a Case-2 split frame's 4-byte header from its first `tov` bytes (`tailHead`,
 * held by the predecessor chunk) plus the following `4-tov` bytes (`rest`, from the candidate
 * successor), and validates it against `profile`.
 * @return The byte offset within the successor chunk where the frame's remainder must start
 *         (frameLen - tov), or -1 if the reconstructed header is invalid/off-profile.
 */
static int reconstructTailHeaderOffset(const uint8_t* tailHead, const int tov,
                                        const uint8_t* rest, const StreamProfile& profile) {
    uint8_t hdr[4];
    for (int j = 0; j < tov; ++j) hdr[j] = tailHead[j];
    for (int j = tov; j < 4; ++j) hdr[j] = rest[j - tov];

    const uint32_t raw = packHeaderBytes(hdr);
    if (!Mp3FrameScanner::isValidHeader(raw)) return -1;

    bool ok;
    const Header h(raw, ok);
    if (!ok || !profile.matches(h)) return -1;

    const int frameLen = h.getFrameLength();
    if (frameLen <= 0) return -1;

    return frameLen - tov;
}

/**
 * Checks if chunk 'a' can be followed by chunk 'b'.
 * @return True if b is a valid continuation of chunk a.
 */
bool canFollow(const ChunkMeta& a, const ChunkMeta& b) {
    if (!a.valid || !b.valid) return false;

    const int rem = tailRemaining(a);

    if (rem >= 0) {
        // Accept if rem is any known valid frame start in b, not just the
        // "best" headOffset that computeChunkMeta picked.
        return hasFrameAt(b, rem);
    }

    // tailOverflow is 1-3: header of partial tail frame is split across chunks.
    // Reconstruct the 4-byte header from a.tailBytes + first bytes of b.
    const int tail_ov = a.tailOverflow;
    if (tail_ov < 1 || tail_ov > 3) return false;

    const int expectedHead = reconstructTailHeaderOffset(a.tailBytes.data(), tail_ov, b.chunkHead, a.profile);
    return expectedHead >= 0 && hasFrameAt(b, expectedHead);
}

/**
 * Packs the first `len` bytes of `chunkHead` (len in 1..3) into an integer key, most
 * significant byte first. Used to group chunks by the exact bytes Case 2 needs from
 * their chunkHead, so lookup doesn't require scanning every chunk - see the
 * byChunkHeadPrefix index in buildAdjacency.
 */
static uint32_t packPrefix(const uint8_t chunkHead[4], const int len) {
    uint32_t key = 0;
    for (int k = 0; k < len; ++k) key = (key << 8) | chunkHead[k];
    return key;
}

/**
 * Constructs the full graph using a byFrameStart index for O(n) Case-1 lookups.
 * @return Constructed adjacency list of output graph.
 *         adj[i] = list of chunk indices that can follow chunk i.
 */
vector<vector<int>> buildAdjacency(const vector<ChunkMeta>& metas) {
    const int n = static_cast<int>(metas.size());
    vector<vector<int>> adj(n);

    // Index: frameStart position -> list of chunk indices that have a valid frame there.
    // Allows O(1) lookup for Case 1: given tailRemaining(a) = rem, find all b with a frame at rem.
    unordered_map<int, vector<int>> byFrameStart;
    byFrameStart.reserve(n * 4);
    for (int i = 0; i < n; ++i)
        for (int pos : metas[i].frameStarts)
            byFrameStart[pos].push_back(i);

    // Index for Case 2: for each possible tov (1..3), group valid chunks by the
    // (4-tov)-byte chunkHead prefix that a reconstructed header would need from them.
    // Chunks sharing a prefix reconstruct to the *same* header, so isValidHeader/Header
    // decoding/profile matching is done once per distinct prefix instead of once per
    // candidate chunk - see CLAUDE.md TODO "Resolve Case-2 O(n) scan".
    unordered_map<uint32_t, vector<int>> byChunkHeadPrefix[3]; // index 0 -> tov=1 (3-byte prefix), etc.
    for (int j = 0; j < n; ++j) {
        if (!metas[j].valid) continue;
        for (int tov = 1; tov <= 3; ++tov) {
            byChunkHeadPrefix[tov - 1][packPrefix(metas[j].chunkHead, 4 - tov)].push_back(j);
        }
    }

    for (int i = 0; i < n; ++i) {
        int rem = tailRemaining(metas[i]);
        if (rem >= 0) {
            auto it = byFrameStart.find(rem);
            if (it == byFrameStart.end()) continue;
            for (int j : it->second) {
                if (j != i) adj[i].push_back(j);
            }
        } else {
            // Case 2: tailOverflow 1-3, expected headOffset depends on b.chunkHead.
            const int tail_ov = metas[i].tailOverflow;
            if (tail_ov < 1 || tail_ov > 3) continue;

            for (const auto& entry : byChunkHeadPrefix[tail_ov - 1]) {
                const uint32_t prefixKey = entry.first;
                const vector<int>& members = entry.second;

                // Unpack prefixKey's (4-tail_ov) bytes, MSB first, matching packPrefix above.
                const int prefixLen = 4 - tail_ov;
                uint8_t prefixBytes[3];
                for (int k = 0; k < prefixLen; ++k)
                    prefixBytes[k] = static_cast<uint8_t>(prefixKey >> (8 * (prefixLen - 1 - k)));

                const int expectedHead = reconstructTailHeaderOffset(
                    metas[i].tailBytes.data(), tail_ov, prefixBytes, metas[i].profile);
                if (expectedHead < 0) continue;

                for (int j : members) {
                    if (j == i) continue;
                    if (hasFrameAt(metas[j], expectedHead)) adj[i].push_back(j);
                }
            }
        }
    }

    return adj;
}

long propagateArcConsistency(vector<vector<int>>& adj, const int n, const int start) {
    const vector<vector<int>> original = adj; // for rollback on contradiction

    vector<vector<int>> pred(n);
    for (int u = 0; u < n; ++u)
        for (int v : adj[u]) pred[v].push_back(u);

    long removed = 0;
    bool contradiction = false;

    auto removeEdge = [&](const int u, const int v) {
        auto& au = adj[u];
        const auto ai = find(au.begin(), au.end(), v);
        if (ai == au.end()) return;
        au.erase(ai);
        auto& pv = pred[v];
        pv.erase(find(pv.begin(), pv.end(), u));
        ++removed;
    };

    // Nothing precedes the pinned first chunk.
    for (const int u : vector<int>(pred[start])) removeEdge(u, start);

    bool changed = true;
    while (changed && !contradiction) {
        changed = false;

        for (int v = 0; v < n; ++v) {
            if (v == start || pred[v].size() != 1) continue;
            const int u = pred[v][0];                 // u -> v is forced
            for (const int w : vector<int>(adj[u]))
                if (w != v) { removeEdge(u, w); changed = true; }
        }
        for (int u = 0; u < n; ++u) {
            if (adj[u].size() != 1) continue;
            const int v = adj[u][0];                  // u -> v is forced
            for (const int t : vector<int>(pred[v]))
                if (t != u) { removeEdge(t, v); changed = true; }
        }

        int noSucc = 0;
        for (int x = 0; x < n; ++x) {
            if (adj[x].empty()) ++noSucc;
            if (x != start && pred[x].empty()) contradiction = true;
        }
        if (noSucc > 1 || adj[start].empty()) contradiction = true;
    }

    if (contradiction) { adj = original; return -1; }
    return removed;
}

/**
 * Find the degree of incoming edges for every node.
 * @param adj Adjacency list of graph.
 * @param n Number of vertices in graph.
 * @return Degrees of incoming edges for each node.
 */
vector<int> computeInDegree(const vector<vector<int>>& adj, const int n) {
    vector<int> indeg(n, 0);
    for (int i = 0; i < n; ++i)
        for (const int j : adj[i])
            indeg[j]++;
    return indeg;
}

/**
 * Find the degree of outgoing edges for every node.
 * @param adj Adjacency list of graph.
 * @param n Number of vertices in graph.
 * @return Degrees of outgoing edges for each node.
 */
vector<int> computeOutDegree(const vector<vector<int>>& adj, const int n) {
    vector<int> outdeg(n, 0);
    for (int i = 0; i < n; ++i)
        outdeg[i] = static_cast<int>(adj[i].size());
    return outdeg;
}
