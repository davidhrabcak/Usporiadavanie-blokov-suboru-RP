#include "adjacency.hpp"
#include "frame_scanner.hpp"

using namespace std;

bool canFollow(const ChunkMeta& a, const ChunkMeta& b) {
    if (!a.valid || !b.valid) return false;

    int rem = tailRemaining(a);

    if (rem >= 0) {
        return b.headOffset == rem;
    }

    // tailOverflow is 1-3: header of partial tail frame is split across chunks.
    // Reconstruct the 4-byte header from a.tailHeadBytes + first bytes of b.
    int tov = a.tailOverflow; // 1, 2, or 3
    uint8_t hbytes[4];
    for (int j = 0; j < tov; ++j)    hbytes[j]   = a.tailHeadBytes[j];
    for (int j = tov; j < 4; ++j)    hbytes[j]   = b.chunkHead[j - tov];

    uint32_t raw = (uint32_t(hbytes[0]) << 24) | (uint32_t(hbytes[1]) << 16)
                 | (uint32_t(hbytes[2]) << 8)  |  uint32_t(hbytes[3]);

    if (!Mp3FrameScanner::isValidHeader(raw)) return false;

    int err;
    Header h(raw, err);
    if (err != 0) return false;
    if (!a.profile.matches(h)) return false;

    int frameLen = h.getFrameLength();
    if (frameLen <= 0) return false;

    return b.headOffset == (frameLen - tov);
}

vector<vector<int>> buildAdjacency(const vector<ChunkMeta>& metas) {
    int n = (int)metas.size();
    vector<vector<int>> adj(n);
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
            if (i != j && canFollow(metas[i], metas[j]))
                adj[i].push_back(j);
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
