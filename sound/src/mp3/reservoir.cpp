#include "mp3/reservoir.hpp"

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

int junctionSlack(const ChunkMeta& a, const ChunkMeta& b, const vector<uint8_t>& bBytes,
                  const int sideInfo, const int maxReservoir) {
    if (a.frames.empty()) return -2;
    const int maxBits = maxReservoir * 8;
    int level = maxBits;   // any level that lets a's last frame pass: its output level doesn't depend on it
    if (!advanceReservoirFrame(level, a.frames.back(), sideInfo, maxReservoir)) return -2;
    int slack = 0;
    auto step = [&](const FrameSlice& f) {   // false: stop (result in `slack`)
        if (level >= maxBits) { slack = -2; return false; }
        const int unused = level / 8 - f.mainDataBegin;
        if (unused < 0 || !advanceReservoirFrame(level, f, sideInfo, maxReservoir)) { slack = -1; return false; }
        slack += unused;
        return true;
    };
    if (a.tailPartialLen > 0) {
        const SideInfoResult side = decodeSplitFrameSideInfo(a, bBytes);
        if (!side.valid) return -2;
        FrameSlice split{-1, a.tailPartialLen, 0, side.mainDataBegin, side.part23Bits};
        split.crcBytes = (a.tailBytes[1] & 1) ? 0 : 2;
        if (!step(split)) return slack;
    } else if (a.tailOverflow != 0) {
        return -2;   // split header: the frame's length is in b
    }
    if (b.frames.empty()) return slack;
    step(b.frames.front());
    return slack;
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

