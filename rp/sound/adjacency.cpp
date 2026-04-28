#include "adjacency.hpp"

#include <iostream>
#include <unordered_map>

#include "frame_scanner.hpp"

using namespace std;

// Returns true if position rem is a known valid frame start in chunk b.
static bool hasFrameAt(const ChunkMeta& b, int rem) {
    // frameStarts is sorted ascending (built in computeChunkMeta scan order).
    auto it = lower_bound(b.frameStarts.begin(), b.frameStarts.end(), rem);
    return it != b.frameStarts.end() && *it == rem;
}

bool canFollow(const ChunkMeta& a, const ChunkMeta& b) {
    if (!a.valid || !b.valid) return false;

    int rem = tailRemaining(a);

    if (rem >= 0) {
        // Accept if rem is any known valid frame start in b, not just the
        // "best" headOffset that computeChunkMeta picked.
        return hasFrameAt(b, rem);
    }

    // tailOverflow is 1-3: header of partial tail frame is split across chunks.
    // Reconstruct the 4-byte header from a.tailHeadBytes + first bytes of b.
    int tov = a.tailOverflow;
    if (tov < 1 || tov > 3) return false;

    uint8_t hbytes[4];
    for (int j = 0; j < tov; ++j)    hbytes[j] = a.tailHeadBytes[j];
    for (int j = tov; j < 4; ++j)    hbytes[j] = b.chunkHead[j - tov];

    uint32_t raw = (uint32_t(hbytes[0]) << 24) | (uint32_t(hbytes[1]) << 16)
                 | (uint32_t(hbytes[2]) << 8)  |  uint32_t(hbytes[3]);

    if (!Mp3FrameScanner::isValidHeader(raw)) return false;

    int err;
    Header h(raw, err);
    if (err != 0) return false;
    if (!a.profile.matches(h)) return false;

    int frameLen = h.getFrameLength();
    if (frameLen <= 0) return false;

    int expectedHead = frameLen - tov;
    return hasFrameAt(b, expectedHead);
}

vector<vector<int>> buildAdjacency(const vector<ChunkMeta>& metas) {
    int n = (int)metas.size();
    vector<vector<int>> adj(n);

    // Index: frameStart position -> list of chunk indices that have a valid frame there.
    // Allows O(1) lookup for Case 1: given tailRemaining(a) = rem, find all b with a frame at rem.
    unordered_map<int, vector<int>> byFrameStart;
    byFrameStart.reserve(n * 4);
    for (int i = 0; i < n; ++i)
        for (int pos : metas[i].frameStarts)
            byFrameStart[pos].push_back(i);

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
            int tov = metas[i].tailOverflow;
            if (tov < 1 || tov > 3) continue;
            for (int j = 0; j < n; ++j) {
                if (j == i || !metas[j].valid) continue;
                uint8_t hbytes[4];
                for (int k = 0; k < tov; ++k) hbytes[k] = metas[i].tailHeadBytes[k];
                for (int k = tov; k < 4; ++k) hbytes[k] = metas[j].chunkHead[k - tov];
                uint32_t raw = (uint32_t(hbytes[0]) << 24) | (uint32_t(hbytes[1]) << 16)
                             | (uint32_t(hbytes[2]) << 8)  |  uint32_t(hbytes[3]);
                if (!Mp3FrameScanner::isValidHeader(raw)) continue;
                int err; Header h(raw, err);
                if (err != 0) continue;
                if (!metas[i].profile.matches(h)) continue;
                int frameLen = h.getFrameLength();
                if (frameLen <= 0) continue;
                if (hasFrameAt(metas[j], frameLen - tov))
                    adj[i].push_back(j);
            }
        }
    }

    return adj;
}

vector<int> computeInDegree(const vector<vector<int>>& adj, int n) {
    vector<int> indeg(n, 0);
    for (int i = 0; i < n; ++i)
        for (int j : adj[i])
            indeg[j]++;
    return indeg;
}

vector<int> computeOutDegree(const vector<vector<int>>& adj, int n) {
    vector<int> outdeg(n, 0);
    for (int i = 0; i < n; ++i)
        outdeg[i] = (int)adj[i].size();
    return outdeg;
}
