// buildAdjacency (indexed) and canFollow (one pair at a time) implement the same two cases;
// this checks they agree on every ordered pair of chunks. Usage: graph_check FILE.mp3...
// (RP_CHUNK_SIZE as in rp, default 1024). Exit 1 on any disagreement.
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>
#include "mp3/frame_scanner.hpp"
#include "mp3/chunk_meta.hpp"
#include "graph/adjacency.hpp"

using namespace std;

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s FILE.mp3...\n", argv[0]); return 2; }
    long chunkSize = 1024;
    if (const char* cs = getenv("RP_CHUNK_SIZE")) chunkSize = max(1024L, strtol(cs, nullptr, 10));
    int bad = 0;
    for (int a = 1; a < argc; ++a) {
        Mp3FrameScanner scanner(argv[a]);
        const vector<vector<uint8_t>> chunks = scanner.chunksFromFirstFrame(chunkSize);
        StreamProfile profile{};
        if (chunks.empty() || !voteProfile(chunks, profile)) { printf("%s: no profile\n", argv[a]); bad = 1; continue; }
        int kbps = 0;
        if (detectCBRUnordered(chunks, profile, kbps)) { profile.isCBR = true; profile.bitrateKbps = kbps; }
        const int n = static_cast<int>(chunks.size());
        vector<ChunkMeta> metas;
        for (int i = 0; i < n; ++i)
            metas.push_back(computeChunkMeta(i, chunks[i], profile, static_cast<long>(chunks[i].size()) < chunkSize));
        const vector<vector<int>> adj = buildAdjacency(metas);
        long onlyGraph = 0, onlyPair = 0, arcs = 0;
        for (int i = 0; i < n; ++i) {
            const set<int> out(adj[i].begin(), adj[i].end());
            arcs += static_cast<long>(out.size());
            for (int j = 0; j < n; ++j) {
                if (j == i) continue;
                const bool g = out.count(j) > 0, p = canFollow(metas[i], metas[j]);
                if (g == p) continue;
                if (g) ++onlyGraph; else ++onlyPair;
                if (onlyGraph + onlyPair <= 5)
                    printf("  %d -> %d: %s (a valid=%d tailOvfl=%d tailLen=%d; b valid=%d)\n", i, j,
                           g ? "graph only" : "canFollow only", metas[i].valid, metas[i].tailOverflow,
                           metas[i].tailPartialLen, metas[j].valid);
            }
        }
        printf("%s @%ld: %d chunks, %ld arcs, graph-only %ld, canFollow-only %ld -> %s\n", argv[a], chunkSize, n,
               arcs, onlyGraph, onlyPair, (onlyGraph || onlyPair) ? "MISMATCH" : "OK");
        if (onlyGraph || onlyPair) bad = 1;
    }
    return bad;
}
