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

    if (prevMeta != nullptr && prevMeta->gapAfter) {
        // A headerless chunk (not placed yet) sits between: the straddling frame's side info
        // is in it. A full reservoir is sound - the check is monotone in the entry level
        // (PLAN_headerless_chunks.md).
        level = maxBits;
        creditedExactly = true;
    } else if (prevMeta != nullptr && prevMeta->tailPartialLen > 0) {
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

