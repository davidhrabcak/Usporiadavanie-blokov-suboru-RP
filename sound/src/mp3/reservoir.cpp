#include "mp3/reservoir.hpp"

#include <algorithm>

using namespace std;

bool advanceReservoirFrame(int& level, const FrameSlice& f, const int sideInfo, const int maxReservoir) {
    if (f.mainDataBegin > level / 8) return false; // mainDataBegin is byte-granular

    int producedBits = (f.length - 4 - f.crcBytes - sideInfo) * 8;
    if (producedBits < 0) producedBits = 0;

    int newLevel = f.mainDataBegin * 8 + producedBits - f.part23Bits;
    if (newLevel < 0) return false;
    const int maxBits = maxReservoir * 8;
    if (newLevel > maxBits) newLevel = maxBits;
    level = newLevel;
    return true;
}

bool advanceReservoirChunk(int& level,
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
            FrameSlice splitFrame{-1, prevMeta->tailPartialLen, 0, side.mainDataBegin, side.part23Bits};
            splitFrame.crcBytes = (prevMeta->tailBytes[1] & 1) ? 0 : 2;
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

int frameSlack(int& level, const FrameSlice& f, const int sideInfo, const int maxReservoir, const int capEff,
               const bool fullRule) {
    const int have = level / 8;
    if (f.mainDataBegin > have) return -1;
    if (have >= capEff && !fullRule) return -2;
    const int unused = std::min(have, capEff) - f.mainDataBegin;
    if (!advanceReservoirFrame(level, f, sideInfo, maxReservoir)) return -1;
    return std::max(unused, 0);
}

int junctionSlack(const ChunkMeta& a, const ChunkMeta& b, const vector<uint8_t>& bBytes,
                  const int sideInfo, const int maxReservoir, const int capEff, const bool fullRule) {
    if (a.frames.empty()) return -3;
    int level = maxReservoir * 8;   // any level that lets a's last frame pass: its output level doesn't depend on it
    if (!advanceReservoirFrame(level, a.frames.back(), sideInfo, maxReservoir)) return -3;
    int slack = 0;
    if (a.tailPartialLen > 0) {
        const SideInfoResult side = decodeSplitFrameSideInfo(a, bBytes);
        if (!side.valid) return -3;
        FrameSlice split{-1, a.tailPartialLen, 0, side.mainDataBegin, side.part23Bits};
        split.crcBytes = (a.tailBytes[1] & 1) ? 0 : 2;
        const int u = frameSlack(level, split, sideInfo, maxReservoir, capEff, fullRule);
        if (u < 0) return u;
        slack += u;
    } else if (a.tailOverflow != 0) {
        return -3;   // split header: the frame's length is in b
    }
    if (b.frames.empty()) return slack;
    const int u = frameSlack(level, b.frames.front(), sideInfo, maxReservoir, capEff, fullRule);
    return u < 0 ? u : slack + u;
}

bool reservoirValid(const vector<ChunkMeta>& metas, const vector<vector<uint8_t>>& chunks,
                    const vector<int>& seq, const int sideInfo, const int maxReservoir) {
    int level = 0;
    const ChunkMeta* prev = nullptr;
    for (int ci : seq) {
        if (!advanceReservoirChunk(level, prev, metas[ci], chunks[ci], sideInfo, maxReservoir)) return false;
        prev = &metas[ci];
    }
    return true;
}


int reservoirBreaks(const vector<ChunkMeta>& metas, const vector<vector<uint8_t>>& chunks,
                    const vector<int>& seq, const int sideInfo, const int maxReservoir,
                    vector<int>* where) {
    int level = 0, breaks = 0;
    const ChunkMeta* prev = nullptr;
    for (size_t k = 0; k < seq.size(); ++k) {
        const int ci = seq[k];
        if (!advanceReservoirChunk(level, prev, metas[ci], chunks[ci], sideInfo, maxReservoir)) {
            ++breaks;
            if (where) where->push_back((int)k);
            level = maxReservoir * 8;
        }
        prev = &metas[ci];
    }
    return breaks;
}

