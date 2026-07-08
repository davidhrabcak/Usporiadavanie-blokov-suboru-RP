#include "adjacency.hpp"
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
    // Reconstruct the 4-byte header from a.tailHeadBytes + first bytes of b.
    const int tail_ov = a.tailOverflow;
    if (tail_ov < 1 || tail_ov > 3) return false;

    uint8_t head_ov[4];
    for (int j = 0; j < tail_ov; ++j)    head_ov[j] = a.tailHeadBytes[j];
    for (int j = tail_ov; j < 4; ++j)    head_ov[j] = b.chunkHead[j - tail_ov];

    const uint32_t raw = (static_cast<uint32_t>(head_ov[0]) << 24) | (static_cast<uint32_t>(head_ov[1]) << 16)
                 | (static_cast<uint32_t>(head_ov[2]) << 8)  |  static_cast<uint32_t>(head_ov[3]);

    if (!Mp3FrameScanner::isValidHeader(raw)) return false;

    int err;
    const Header h(raw, err);
    if (err != 0) return false;
    if (!a.profile.matches(h)) return false;

    const int frameLen = h.getFrameLength();
    if (frameLen <= 0) return false;

    const int expectedHead = frameLen - tail_ov;
    return hasFrameAt(b, expectedHead);
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
                uint8_t hbytes[4];
                for (int k = 0; k < tail_ov; ++k) hbytes[k] = metas[i].tailHeadBytes[k];
                for (int k = tail_ov; k < 4; ++k) hbytes[k] = static_cast<uint8_t>(prefixKey >> (8 * (3 - k)));

                const uint32_t raw = (static_cast<uint32_t>(hbytes[0]) << 24) | (static_cast<uint32_t>(hbytes[1]) << 16)
                             | (static_cast<uint32_t>(hbytes[2]) << 8)  |  static_cast<uint32_t>(hbytes[3]);
                if (!Mp3FrameScanner::isValidHeader(raw)) continue;
                int err; Header h(raw, err);
                if (err != 0) continue;

                if (!metas[i].profile.matches(h)) continue;

                const int frameLen = h.getFrameLength();
                if (frameLen <= 0) continue;
                const int expectedHead = frameLen - tail_ov;

                for (int j : members) {
                    if (j == i) continue;
                    if (hasFrameAt(metas[j], expectedHead)) adj[i].push_back(j);
                }
            }
        }
    }

    return adj;
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
