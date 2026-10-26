#include "slot_fill.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <limits>
#include <thread>

#include "adjacency.hpp"
#include "pcm_junction.hpp"

namespace SlotFill {

static constexpr double EXCLUDED = 1e6;   // ruled out by a constraint (still a finite cost so
                                          // the assignment always exists; counted as `forced`)
static constexpr double UNDECODABLE = 30; // window decoded but frames failed after the junction

// MPEG audio CRC-16 (poly 0x8005, init 0xFFFF) over header bytes 2-3 + side info, compared with
// the 2 bytes after the header. `f` starts at the frame header. True when it matches, when the
// stream isn't protected, or when not enough bytes are there to tell.
static bool crcOk(const std::vector<uint8_t>& f, const int side) {
    if (f.size() < static_cast<size_t>(6 + side) || (f[1] & 1)) return true;
    unsigned crc = 0xFFFF;
    auto feed = [&](uint8_t byte) {
        for (int k = 7; k >= 0; --k) {
            const bool bit = ((crc >> 15) ^ (byte >> k)) & 1;
            crc = (crc << 1) & 0xFFFF;
            if (bit) crc ^= 0x8005;
        }
    };
    feed(f[2]);
    feed(f[3]);
    for (int k = 0; k < side; ++k) feed(f[6 + k]);
    return crc == (static_cast<unsigned>(f[4]) << 8 | f[5]);
}

// Min-cost assignment, rows = slots, cols = pool chunks, rectangular (padded with zero-cost
// dummies). O(n^3) potentials / Dijkstra form (e-maxx). Returns row -> col, -1 for a dummy.
static std::vector<int> hungarian(const std::vector<std::vector<double>>& cost, const int rows, const int cols) {
    const int n = std::max(rows, cols);
    const double INF = std::numeric_limits<double>::infinity();
    auto a = [&](int i, int j) { return (i < rows && j < cols) ? cost[i][j] : 0.0; };
    std::vector<double> u(n + 1, 0), v(n + 1, 0);
    std::vector<int> p(n + 1, 0), way(n + 1, 0);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(n + 1, INF);
        std::vector<char> used(n + 1, 0);
        do {
            used[j0] = 1;
            const int i0 = p[j0];
            double delta = INF;
            int j1 = 0;
            for (int j = 1; j <= n; ++j) {
                if (used[j]) continue;
                const double cur = a(i0 - 1, j - 1) - u[i0] - v[j];
                if (cur < minv[j]) { minv[j] = cur; way[j] = j0; }
                if (minv[j] < delta) { delta = minv[j]; j1 = j; }
            }
            for (int j = 0; j <= n; ++j) {
                if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
                else minv[j] -= delta;
            }
            j0 = j1;
        } while (p[j0] != 0);
        do { const int j1 = way[j0]; p[j0] = p[j1]; j0 = j1; } while (j0);
    }
    std::vector<int> rowToCol(rows, -1);
    for (int j = 1; j <= n; ++j)
        if (p[j] >= 1 && p[j] <= rows && j <= cols) rowToCol[p[j] - 1] = j - 1;
    return rowToCol;
}

std::vector<int> assign(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
                        const std::vector<Slot>& slots, const std::vector<std::vector<uint8_t>>& pool,
                        const long chunkSize, int threads, Stats* stats) {
    const auto t0 = std::chrono::steady_clock::now();
    const int S = static_cast<int>(slots.size()), H = static_cast<int>(pool.size());
    const int C = static_cast<int>(chunkSize);
    std::vector<std::vector<double>> cost(S, std::vector<double>(H, EXCLUDED));
    std::vector<long> nDec(S, 0), nExcl(S, 0), nUnmeas(S, 0);

    auto scoreSlot = [&](int s) {
        const Slot& sl = slots[s];
        const ChunkMeta& a = metas[sl.left.back()];
        const bool case2 = a.tailPartialLen == -1 && a.tailOverflow >= 1 && a.tailOverflow <= 3;
        const int side = sideInfoSize(a.profile.versionID, a.profile.isMono());
        std::vector<const std::vector<uint8_t>*> segs;
        for (int c : sl.left) segs.push_back(&chunks[c]);
        const PcmJunction::LeftContext left(segs, metas[sl.left.front()].headOffset);
        for (int m = 0; m < H; ++m) {
            if (case2) {
                const int off = splitHeaderOffset(a, pool[m]);
                if (off < C || !hasFrameStart(metas[sl.b], off - C)) { ++nExcl[s]; continue; }
            }
            std::vector<uint8_t> frame(a.tailBytes);   // the straddling frame from its header on
            frame.insert(frame.end(), pool[m].begin(), pool[m].begin() + std::min<size_t>(pool[m].size(), 64));
            if (!crcOk(frame, side)) { ++nExcl[s]; continue; }
            std::vector<uint8_t> right(pool[m]);
            right.insert(right.end(), chunks[sl.b].begin(), chunks[sl.b].end());
            if (sl.after >= 0) right.insert(right.end(), chunks[sl.after].begin(), chunks[sl.after].end());
            const PcmJunction::Metrics mt = left.score(right);
            ++nDec[s];
            if (mt.valid && mt.postFailed == 0 && !mt.silent) cost[s][m] = std::log1p(mt.flux);
            else if (mt.valid && mt.postFailed == 0) { cost[s][m] = std::numeric_limits<double>::quiet_NaN(); ++nUnmeas[s]; }
            else { cost[s][m] = UNDECODABLE; ++nUnmeas[s]; }
        }
    };
    threads = std::max(1, std::min(threads, S));
    std::atomic<int> next(0);
    std::vector<std::thread> pool_;
    for (int t = 0; t < threads; ++t)
        pool_.emplace_back([&] { for (int s; (s = next.fetch_add(1)) < S;) scoreSlot(s); });
    for (auto& th : pool_) th.join();

    // silent windows (nothing to compare) get the median measured cost
    std::vector<double> finite;
    for (const auto& row : cost) for (double c : row) if (!std::isnan(c) && c < UNDECODABLE) finite.push_back(c);
    double neutral = 1.0;
    if (!finite.empty()) {
        std::nth_element(finite.begin(), finite.begin() + finite.size() / 2, finite.end());
        neutral = finite[finite.size() / 2];
    }
    for (auto& row : cost) for (double& c : row) if (std::isnan(c)) c = neutral;

    std::vector<int> out = hungarian(cost, S, H);
    if (stats) {
        for (int s = 0; s < S; ++s) {
            stats->decodes += nDec[s];
            stats->excluded += nExcl[s];
            stats->unmeasured += nUnmeas[s];
            if (out[s] >= 0 && cost[s][out[s]] >= EXCLUDED) ++stats->forced;
        }
        stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    return out;
}

} // namespace SlotFill
