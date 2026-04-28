#include "chunk_meta.hpp"
#include "frame_scanner.hpp"
#include <algorithm>
#include <cstring>

using namespace std;

static int sideInfoSize(const StreamProfile& p) {
    // MPEG1: 32 (stereo) / 17 (mono)   MPEG2/2.5: 17 (stereo) / 9 (mono)
    if (p.versionID == 0b11)
        return p.isMono() ? 17 : 32;
    else
        return p.isMono() ? 9 : 17;
}

static uint16_t readMainDataBegin(const vector<uint8_t>& chunk, int off) {
    // First 9 bits of side-info block (immediately after the 4-byte header)
    if (off + 5 >= (int)chunk.size()) return 0;
    return static_cast<uint16_t>(
        ((chunk[off + 4] << 1) | (chunk[off + 5] >> 7)) & 0x1FF
    );
}

// Try parsing a run of frames starting at `startOff` within `chunk`.
// Returns number of consecutive consistent frames found (all matching `expected`).
// Populates `out` with slices for fully-contained frames only.
// Returns number of complete frames found. Also sets tailOverflow and tailPartialLen.
// tailOverflow = bytes of the partial tail frame that are inside this chunk (0 if ends on boundary).
// tailPartialLen = total length of that partial frame (-1 if we can't read its header).
static int tryParse(const vector<uint8_t>& chunk,
                    int startOff,
                    const StreamProfile& expected,
                    vector<FrameSlice>& out,
                    int& tailOverflow,
                    int& tailPartialLen) {

    int i = startOff;
    int count = 0;
    out.clear();
    tailOverflow    = 0;
    tailPartialLen  = 0;

    while (i + 4 <= (int)chunk.size()) {
        uint32_t raw =
            (uint32_t(chunk[i])   << 24) |
            (uint32_t(chunk[i+1]) << 16) |
            (uint32_t(chunk[i+2]) << 8)  |
             uint32_t(chunk[i+3]);

        if (!Mp3FrameScanner::isValidHeader(raw)) break;

        int err;
        Header h(raw, err);
        if (err != 0) break;
        if (!expected.matches(h)) break;

        int len = h.getFrameLength();
        if (len <= 0) break;

        if (i + len > (int)chunk.size()) {
            // Partial frame: header is readable, frame spills into next chunk
            tailOverflow   = (int)chunk.size() - i;
            tailPartialLen = len;
            break;
        }

        // Complete frame fully contained in chunk
        uint16_t mdb = 0;
        if (expected.isLayerIII())
            mdb = readMainDataBegin(chunk, i);
        out.push_back({i, len, h.getBitrate(), mdb});
        count++;
        i += len;
    }

    // Handle case where the loop exited because < 4 bytes remain (can't read header)
    if (tailOverflow == 0 && i < (int)chunk.size()) {
        tailOverflow   = (int)chunk.size() - i;
        tailPartialLen = -1; // unknown: not enough bytes to decode the header
    }

    return count;
}

ChunkMeta computeChunkMeta(int chunkIndex,
                           const vector<uint8_t>& chunk,
                           const StreamProfile& expected) {
    ChunkMeta meta;
    meta.chunkIndex     = chunkIndex;
    meta.headOffset     = 0;
    meta.tailOverflow   = 0;
    meta.tailPartialLen = 0;
    meta.valid          = false;

    int bestCount    = 0;
    int bestOff      = -1;
    vector<FrameSlice> bestFrames;
    int bestTail     = 0;
    int bestTailLen  = 0;

    int limit = min((int)chunk.size() - 4, MAX_FRAME_LENGTH - 1);

    for (int off = 0; off <= limit; ++off) {
        uint32_t raw =
            (uint32_t(chunk[off])   << 24) |
            (uint32_t(chunk[off+1]) << 16) |
            (uint32_t(chunk[off+2]) << 8)  |
             uint32_t(chunk[off+3]);

        if (!Mp3FrameScanner::isValidHeader(raw)) continue;

        int err;
        Header h(raw, err);
        if (err != 0) continue;
        if (!expected.matches(h)) continue;

        meta.frameStarts.push_back(off);

        vector<FrameSlice> frames;
        int tail = 0, tailLen = 0;
        int count = tryParse(chunk, off, expected, frames, tail, tailLen);

        if (count > bestCount) {
            bestCount   = count;
            bestOff     = off;
            bestFrames  = frames;
            bestTail    = tail;
            bestTailLen = tailLen;
        }
    }

    if (bestOff >= 0 && bestCount >= 1) {
        meta.valid          = true;
        meta.headOffset     = bestOff;
        meta.tailOverflow   = bestTail;
        meta.tailPartialLen = bestTailLen;
        meta.frames         = bestFrames;
        meta.profile        = expected;
    }

    // Store first 4 bytes of the chunk for cross-chunk header reconstruction
    for (int j = 0; j < 4 && j < (int)chunk.size(); ++j)
        meta.chunkHead[j] = chunk[j];

    // Store first min(tailOverflow, 3) bytes of the partial tail frame's header
    // (only meaningful when tailOverflow 1-3)
    if (meta.tailOverflow >= 1 && meta.tailOverflow <= 3) {
        int partialStart = (int)chunk.size() - meta.tailOverflow;
        for (int j = 0; j < meta.tailOverflow; ++j)
            meta.tailHeadBytes[j] = chunk[partialStart + j];
    }

    return meta;
}

// Returns true if the frame at `off` inside `chunk` is a Xing/Info VBR header frame.
static bool isXingFrame(const vector<uint8_t>& chunk, int off, const Header& h) {
    if (!h.isLayerIII()) return false;
    // Side info size: MPEG1 stereo=32, MPEG1 mono=17, MPEG2/2.5 stereo=17, MPEG2/2.5 mono=9
    int si = (h.getVersionID() == 0b11) ? (h.isMono() ? 17 : 32)
                                        : (h.isMono() ?  9 : 17);
    int xpos = off + 4 + si;
    if (xpos + 4 > (int)chunk.size()) return false;
    return (chunk[xpos]=='X' && chunk[xpos+1]=='i' && chunk[xpos+2]=='n' && chunk[xpos+3]=='g')
        || (chunk[xpos]=='I' && chunk[xpos+1]=='n' && chunk[xpos+2]=='f' && chunk[xpos+3]=='o')
        || (chunk[xpos]=='V' && chunk[xpos+1]=='B' && chunk[xpos+2]=='R' && chunk[xpos+3]=='I');
}

bool deriveProfile(const vector<uint8_t>& chunk, StreamProfile& out) {
    for (int i = 0; i + 4 <= (int)chunk.size(); ++i) {
        uint32_t raw =
            (uint32_t(chunk[i])   << 24) |
            (uint32_t(chunk[i+1]) << 16) |
            (uint32_t(chunk[i+2]) << 8)  |
             uint32_t(chunk[i+3]);

        if (!Mp3FrameScanner::isValidHeader(raw)) continue;

        int err;
        Header h(raw, err);
        if (err != 0) continue;
        int flen = h.getFrameLength();
        if (flen <= 0) continue;

        if (isXingFrame(chunk, i, h)) {
            i += flen - 1; // skip to next frame (loop will ++i)
            continue;
        }

        out.versionID  = h.getVersionID();
        out.layerID    = h.getLayerID();
        out.channelIdx = h.getChannelIdx();
        out.sampleRate = h.getSampleRate();
        return true;
    }
    return false;
}
