// left_context_check <fixture.mp3> [maxLeft]
//
// Verifies PcmJunction::LeftContext against scoreJunction: for (up to maxLeft) chunks a and
// up to 3 structural predecessor contexts p (plus "no context"), every structural successor b
// is scored both ways; every Metrics field must be bit-identical. Also times both paths.
// RP_CHUNK_SIZE as in rp. Exit code 1 on any mismatch.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "frame_scanner.hpp"
#include "chunk_meta.hpp"
#include "adjacency.hpp"
#include "pcm_junction.hpp"

using namespace std;

static bool same(const PcmJunction::Metrics& x, const PcmJunction::Metrics& y) {
    auto eq = [](double a, double b) { return memcmp(&a, &b, sizeof a) == 0; };
    return x.valid == y.valid && x.silent == y.silent && x.postFailed == y.postFailed &&
           eq(x.flux, y.flux) && eq(x.specDist, y.specDist) && eq(x.rmsJump, y.rmsJump) &&
           eq(x.hfBurst, y.hfBurst) && eq(x.peak, y.peak);
}

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <fixture.mp3> [maxLeft]\n", argv[0]); return 2; }
    const int maxLeft = argc > 2 ? atoi(argv[2]) : 1 << 30;
    int chunkSize = 1024;
    if (const char* cs = getenv("RP_CHUNK_SIZE")) chunkSize = max(1024, atoi(cs));
    Mp3FrameScanner scanner(argv[1]);
    if (scanner.getFrameCount() == 0) { fprintf(stderr, "No frames found\n"); return 1; }
    ifstream file(argv[1], ios::binary);
    file.seekg(static_cast<long>(scanner.getFrames()[0].position), ios::beg);
    vector<vector<uint8_t>> chunks;
    {
        vector<uint8_t> buf(chunkSize);
        while (file.read(reinterpret_cast<char*>(buf.data()), chunkSize) || file.gcount() > 0)
            chunks.emplace_back(buf.begin(), buf.begin() + static_cast<long>(file.gcount()));
    }
    const int n = (int)chunks.size();
    StreamProfile profile{};
    if (!deriveProfile(chunks[0], profile)) { fprintf(stderr, "no profile\n"); return 1; }
    vector<ChunkMeta> metas;
    for (int i = 0; i < n; ++i) metas.push_back(computeChunkMeta(i, chunks[i], profile, chunks[i].size() < chunks[0].size()));
    const auto adj = buildAdjacency(metas);
    vector<vector<int>> preds(n);
    for (int a = 0; a < n; ++a) for (int b : adj[a]) preds[b].push_back(a);

    long windows = 0, valid = 0, mismatches = 0;
    double tOld = 0, tNew = 0;
    using clk = chrono::steady_clock;
    for (int a = 0, done = 0; a < n && done < maxLeft; ++a) {
        if (!metas[a].valid || adj[a].empty()) continue;
        ++done;
        vector<int> ctx = {-1};
        for (size_t k = 0; k < preds[a].size() && k < 3; ++k) if (metas[preds[a][k]].valid) ctx.push_back(preds[a][k]);
        for (int p : ctx) {
            vector<const vector<uint8_t>*> left;
            int first = a;
            if (p >= 0) { left.push_back(&chunks[p]); first = p; }
            left.push_back(&chunks[a]);
            const auto t0 = clk::now();
            PcmJunction::LeftContext lc(left, metas[first].headOffset);
            vector<PcmJunction::Metrics> fast;
            for (int b : adj[a]) fast.push_back(lc.score(chunks[b]));
            const auto t1 = clk::now();
            vector<PcmJunction::Metrics> slow;
            for (int b : adj[a]) {
                auto segs = left;
                segs.push_back(&chunks[b]);
                slow.push_back(PcmJunction::scoreJunction(segs, (int)left.size(), metas[first].headOffset));
            }
            const auto t2 = clk::now();
            tNew += chrono::duration<double>(t1 - t0).count();
            tOld += chrono::duration<double>(t2 - t1).count();
            for (size_t k = 0; k < slow.size(); ++k) {
                ++windows;
                if (slow[k].valid) ++valid;
                if (!same(slow[k], fast[k])) {
                    if (++mismatches <= 5)
                        printf("MISMATCH p=%d a=%d b=%d: valid %d/%d flux %.17g/%.17g postFailed %d/%d\n", p, a, adj[a][k],
                               slow[k].valid, fast[k].valid, slow[k].flux, fast[k].flux, slow[k].postFailed, fast[k].postFailed);
                }
            }
        }
    }
    printf("windows=%ld valid=%ld mismatches=%ld  scoreJunction %.2fs  LeftContext %.2fs  speedup %.2fx\n",
           windows, valid, mismatches, tOld, tNew, tNew > 0 ? tOld / tNew : 0.0);
    return mismatches ? 1 : 0;
}
