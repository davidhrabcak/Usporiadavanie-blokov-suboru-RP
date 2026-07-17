#include "chunk_meta.hpp"
#include "frame_scanner.hpp"
#include <algorithm>
#include <cstring>

using namespace std;

/**
 * Determines the length of side information in a chunk.
 * @return Side information length in bytes.
 */
static int sideInfoSize(const int versionID, const bool isMono) {
    // MPEG1: 32 (stereo) / 17 (mono)   MPEG2/2.5: 17 (stereo) / 9 (mono)
    if (versionID == 0b11)
        return isMono ? 17 : 32;
    else
        return isMono ? 9 : 17;
}

/**
 * A side-info bit source backed by up to two logically-concatenated byte ranges: `first`
 * immediately followed by `second`. Lets the same MSB-first bit-reading logic
 * (readBitsMsb/computeSideInfoFields below) serve both the ordinary case (a frame's side info
 * fully inside one chunk - `second` unused) and the cross-chunk case (a Case-1 split frame's
 * side info spanning the tail of chunk a and the head of chunk b - see decodeSplitFrameSideInfo),
 * instead of maintaining two separate copies of the bit-layout logic that could drift apart.
 */
struct SideInfoBitSource {
    const uint8_t* first;
    int firstLen;
    const uint8_t* second = nullptr;
    int secondLen = 0;

    int size() const { return firstLen + secondLen; }
    uint8_t byteAt(const int idx) const {
        return idx < firstLen ? first[idx] : second[idx - firstLen];
    }
};

/** Reads `nbits` (<=32) MSB-first from `src` starting at bit offset `bitPos`, advancing `bitPos`. */
static uint32_t readBitsMsb(const SideInfoBitSource& src, int& bitPos, const int nbits) {
    uint32_t val = 0;
    for (int i = 0; i < nbits; ++i) {
        const int byteIdx = bitPos / 8;
        const int bitIdx  = 7 - (bitPos % 8);
        val = (val << 1) | ((src.byteAt(byteIdx) >> bitIdx) & 0x1);
        ++bitPos;
    }
    return val;
}

/**
 * Detects if the current MP3 frame contains a VBR header
 * @return Returns true if the frame at `off` inside `chunk` is a Xing/Info VBR header frame.
 */
static bool isXingFrame(const vector<uint8_t>& chunk, const int off, const Header& h) {
    if (!h.isLayerIII()) return false;
    // Side info size: MPEG1 stereo=32, MPEG1 mono=17, MPEG2/2.5 stereo=17, MPEG2/2.5 mono=9
    const int side = sideInfoSize(h.getVersionID(), h.isMono());
    const int xpos = off + 4 + side;
    if (xpos + 4 > static_cast<int>(chunk.size())) return false;

    const uint8_t* ptr = &chunk[xpos];
    return std::memcmp(ptr, "Xing", 4) == 0 ||
       std::memcmp(ptr, "Info", 4) == 0 ||
       std::memcmp(ptr, "VBRI", 4) == 0;
}

/* Helper function that eliminates repeating code.
 * Check if chunk is long enough for a header are still needed before using this function. */
static uint32_t computeChunkHeader(const vector<uint8_t>& chunk, const int index) {
    return (static_cast<uint32_t>(chunk[index])   << 24) |
           (static_cast<uint32_t>(chunk[index + 1]) << 16) |
           (static_cast<uint32_t>(chunk[index + 2]) << 8)  |
            static_cast<uint32_t>(chunk[index + 3]);
}

/**
 * Extracts `main_data_begin` and the sum of 'part2_3_length' (the actual main-data bits a
 * Layer III frame consumes from the bit reservoir) across every granule/channel, reading from
 * a side-info block that starts at bit 0 of `sideBuf` (i.e. `sideBuf` must already be
 * positioned at the first byte immediately after the frame's 4-byte header).
 *
 * part2_3_length is returned in BITS, not bytes: it is a bit-granular quantity (the exact
 * number of bits spent on scalefactors + Huffman data), and the reservoir bookkeeping in
 * main.cpp accumulates it every frame. Rounding up to bytes here would add up to ~7 bits of
 * phantom "consumption" on *every single frame*, and that systematic bias compounds across a
 * whole file into a large false deficit - confirmed empirically: it caused the reservoir check
 * to reject `test7_shorter.mp3`'s true, correct chunk order by chunk 7 (see CLAUDE.md). Only
 * `mainDataBegin` (a byte-granular field in the bitstream) should ever be compared/rounded at
 * the byte level; the running reservoir level itself must stay bit-precise.
 *
 * Side-info layout (ISO/IEC 11172-3 / 13818-3), all fields MSB-first (CRC is not accounted for,
 * i.e. protection_bit is assumed set / no CRC present):
 *   MPEG1:      main_data_begin(9) + private_bits(mono:5/stereo:3) + scfsi(4 bits/channel)
 *               then 2 granules x numChannels blocks of 59 bits, part2_3_length is the
 *               leading 12 bits of each block.
 *   MPEG2/2.5:  main_data_begin(8) + private_bits(mono:1/stereo:2), no scfsi,
 *               then 1 granule x numChannels blocks of 63 bits, part2_3_length is the
 *               leading 12 bits of each block.
 * (Block sizes/private_bits widths cross-checked against the documented side-info byte totals
 * in sideInfoSize() - both must agree exactly, and they do. Note: main_data_begin's width
 * genuinely differs between MPEG1 (9 bits) and MPEG2/2.5 (8 bits) - unifying this function
 * fixed a latent bug where the old, separate readMainDataBegin() always read 9 bits regardless
 * of version, which would have under/misread mainDataBegin for any MPEG2/2.5 stream; dormant
 * until now because every existing test fixture is MPEG1.)
 *
 * @param sideBuf Bit source positioned at the start of the side-info block.
 * @return .valid is false if the side-info block doesn't fully fit in `sideBuf`.
 */
static SideInfoResult computeSideInfoFields(const SideInfoBitSource& sideBuf, const bool mpeg1, const bool mono) {
    SideInfoResult result;

    const int numChannels = mono ? 1 : 2;
    const int numGranules = mpeg1 ? 2 : 1;
    const int blockBits   = mpeg1 ? 59 : 63;
    const int mdbBits     = mpeg1 ? 9 : 8;
    const int privBits    = mpeg1 ? (mono ? 5 : 3) : (mono ? 1 : 2);
    const int scfsiBits   = mpeg1 ? numChannels * 4 : 0;

    const int totalSideBits = mdbBits + privBits + scfsiBits + numGranules * numChannels * blockBits;
    if (sideBuf.size() * 8 < totalSideBits) return result; // side-info block doesn't fully fit

    int bitPos = 0;
    result.mainDataBegin = static_cast<uint16_t>(readBitsMsb(sideBuf, bitPos, mdbBits));
    bitPos += privBits + scfsiBits;

    int totalBits = 0;
    for (int g = 0; g < numGranules; ++g) {
        for (int c = 0; c < numChannels; ++c) {
            totalBits += static_cast<int>(readBitsMsb(sideBuf, bitPos, 12));
            bitPos += (blockBits - 12); // skip rest of this granule/channel block
        }
    }
    result.part23Bits = static_cast<uint16_t>(totalBits);
    result.valid = true;
    return result;
}

/**
 * Reads `main_data_begin` and part2_3_length for a frame whose side info is fully contained in
 * one chunk (the ordinary case - `off` points at the frame's 4-byte header within `chunk`).
 * @return .valid is false if not Layer III or the side-info block doesn't fully fit in `chunk`.
 */
static SideInfoResult readFrameSideInfo(const vector<uint8_t>& chunk, const int off, const Header& h) {
    if (!h.isLayerIII()) return {};
    const int sideStart = off + 4;
    const SideInfoBitSource src{&chunk[sideStart], static_cast<int>(chunk.size()) - sideStart};
    return computeSideInfoFields(src, h.getVersionID() == 0b11, h.isMono());
}

/**
 * Reads `main_data_begin` and part2_3_length for a Case-1 split frame whose side info may span
 * the chunk boundary: the header (and >=0 bytes of side info) sits in the tail of chunk `a`, and
 * the rest of the side info (if any) is the first bytes of the immediate successor chunk `b`.
 *
 * By construction in tryParse/computeChunkMeta, whenever `a.tailPartialLen > 0` (Case 1) the
 * frame's full 4-byte header was read successfully from within `a`, so `a.tailOverflow >= 4` and
 * `a.tailBytes[0..4)` is that header. This function only needs `a`'s ChunkMeta (for tailBytes/
 * tailOverflow/profile) and `b`'s raw bytes - no header re-decoding, since the header was already
 * validated when `a`'s metadata was computed.
 *
 * @param a      Metadata for the chunk containing the split frame's header and start of payload.
 * @param bBytes Raw bytes of the candidate successor chunk `b` (bytes [0, rem) of `bBytes` are
 *               this frame's payload continuation; `b`'s own next frame starts at `rem`).
 * @return .valid is false if `a` isn't a Case-1 split frame, isn't Layer III, or `b` doesn't
 *         hold enough bytes to complete the side-info block.
 */
SideInfoResult decodeSplitFrameSideInfo(const ChunkMeta& a, const vector<uint8_t>& bBytes) {
    if (!a.profile.isLayerIII()) return {};
    if (a.tailPartialLen <= 0 || a.tailOverflow < 4) return {}; // not a readable Case-1 split frame

    const int headerLen = 4;
    const uint8_t* afterHeader = a.tailBytes.data() + headerLen;
    const int afterHeaderLen   = a.tailOverflow - headerLen;

    const SideInfoBitSource src{afterHeader, afterHeaderLen, bBytes.data(), static_cast<int>(bBytes.size())};
    return computeSideInfoFields(src, a.profile.versionID == 0b11, a.profile.isMono());
}


/**
 * Try parsing a run of frames starting at `startOff` within `chunk`.
 * Returns number of consecutive consistent frames found (all matching `expected`).
 *
 * @param chunk Chunk for parsing.
 * @param startOff Starting offset within chunk where parsing is tried.
 * @param expected Expected frame profile that is matched with all found consecutive frames.
 * @param out Populates `out` with slices for fully-contained frames only.
 * @param tailOverflow Bytes of the partial tail frame that are inside this chunk (0 if ends on boundary).
 * @param tailPartialLen Total length of that partial frame (-1 if we can't read its header).
 * @return Returns number of complete frames found, EXCLUDING any Xing/Info/VBRI header frame
 *         (those are real, valid frames and must still count towards advancing `i` and towards
 *         `out`/reservoir accounting, but they carry no audio and would otherwise bias headOffset
 *         selection towards whichever offset happens to land on one - see computeChunkMeta).
 *         Also sets tailOverflow and tailPartialLen.
*/
static int tryParse(const vector<uint8_t>& chunk,
                    const int startOff,
                    const StreamProfile& expected,
                    vector<FrameSlice>& out,
                    int& tailOverflow,
                    int& tailPartialLen) {

    int i = startOff;
    int count = 0;
    out.clear();
    tailOverflow    = 0;
    tailPartialLen  = 0;

    while (i + 4 <= static_cast<int>(chunk.size())) {
        const uint32_t raw = computeChunkHeader(chunk, i);
        if (!Mp3FrameScanner::isValidHeader(raw)) break;

        int err;
        Header h(raw, err);
        if (err != 0) break;
        if (!expected.matches(h)) break;

        const int len = h.getFrameLength();
        if (len <= 0) break;

        if (i + len > static_cast<int>(chunk.size())) {
            // Partial frame: header is readable, frame spills into next chunk
            tailOverflow   = static_cast<int>(chunk.size()) - i;
            tailPartialLen = len;
            break;
        }

        // Complete frame fully contained in chunk
        uint16_t mdb = 0;
        uint16_t part23 = 0;
        if (expected.isLayerIII()) {
            const SideInfoResult side = readFrameSideInfo(chunk, i, h);
            mdb    = side.mainDataBegin;
            part23 = side.part23Bits;
        }
        out.push_back({i, len, h.getBitrate(), mdb, part23});
        // Xing/Info/VBRI frames are real, valid frames (their bytes are kept in `out` so
        // reservoir accounting and output reconstruction stay correct) but carry no audio,
        // so they must not count towards the "most consecutive frames" contest below -
        // otherwise a chunk that happens to start right on a Xing frame looks artificially
        // strong and biases computeChunkMeta's headOffset choice.
        if (!isXingFrame(chunk, i, h)) count++;
        i += len;
    }

    // Handle case where the loop exited because < 4 bytes remain (can't read header)
    if (tailOverflow == 0 && i < static_cast<int>(chunk.size())) {
        tailOverflow   = static_cast<int>(chunk.size()) - i;
        tailPartialLen = -1; // unknown: not enough bytes to decode the header
    }

    return count;
}

/**
 * Ranks how trustworthy a candidate offset's tail is, from best (no ambiguity
 * at all) to worst (unrecoverable). Used as the primary key when picking
 * `headOffset`, ahead of raw frame count - see computeChunkMeta for why.
 *  3 = tailOverflow == 0: chunk ends exactly on a frame boundary, no tail to resolve at all.
 *  2 = tailPartialLen > 0: tail frame's header was read cleanly, its full length is known (Case 1).
 *  1 = tailPartialLen == -1 with a 1-3 byte tail: header is split across chunks but recoverable (Case 2).
 *  0 = anything else: tailPartialLen == -1 with tailOverflow > 3 - header bytes were present but
 *      failed to decode (garbage / non-audio content / end of profile-matching stream). Unrecoverable.
 */
static int tailQuality(const int tailOverflow, const int tailPartialLen) {
    if (tailOverflow == 0) return 3;
    if (tailPartialLen > 0) return 2;
    if (tailPartialLen == -1 && tailOverflow >= 1 && tailOverflow <= 3) return 1;
    return 0;
}

/**
 * Extract information needed for the struct ChunkMeta
 * @return New ChunkMeta.
 */
ChunkMeta computeChunkMeta(const int chunkIndex,
                           const vector<uint8_t>& chunk,
                           const StreamProfile& expected) {
    ChunkMeta meta;
    meta.chunkIndex     = chunkIndex;
    meta.headOffset     = 0;
    meta.tailOverflow   = 0;
    meta.tailPartialLen = 0;
    meta.valid          = false;

    int bestCount    = 0;
    int bestQuality  = -1;   // -1 so the first candidate found always wins
    int bestOff      = -1;
    vector<FrameSlice> bestFrames;
    int bestTail     = 0;
    int bestTailLen  = 0;

    const int limit = min(static_cast<int>(chunk.size()) - 4, MAX_FRAME_LENGTH - 1);

    for (int off = 0; off <= limit; ++off) {
        const uint32_t raw = computeChunkHeader(chunk, off);

        if (!Mp3FrameScanner::isValidHeader(raw)) continue;

        int err;
        Header h(raw, err);
        if (err != 0) continue;
        if (!expected.matches(h)) continue;

        meta.frameStarts.push_back(off);

        vector<FrameSlice> frames;
        int tail = 0, tailLen = 0;
        const int count = tryParse(chunk, off, expected, frames, tail, tailLen);
        const int quality = tailQuality(tail, tailLen);

        // Composite (tailQuality, frameCount) comparison, tailQuality first: in CBR streams
        // the wrong alignment can win on raw frame count alone (aliased headers repeating
        // every frameLength bytes - see CLAUDE.md "computeChunkMeta picks the wrong
        // headOffset"), but the true boundary almost always has a cleanly-readable tail.
        // Preferring quality over count fixes that class of bug even when it means
        // accepting fewer frames.
        if (quality > bestQuality || (quality == bestQuality && count > bestCount)) {
            bestCount   = count;
            bestQuality = quality;
            bestOff     = off;
            bestFrames  = frames;
            bestTail    = tail;
            bestTailLen = tailLen;
        }
    }

    if (bestOff >= 0) {
        meta.valid          = true;
        meta.headOffset     = bestOff;
        meta.tailOverflow   = bestTail;
        meta.tailPartialLen = bestTailLen;
        meta.frames         = bestFrames;
        meta.profile        = expected;
    }

    // Store first 4 bytes of the chunk for cross-chunk header reconstruction
    for (int j = 0; j < 4 && j < static_cast<int>(chunk.size()); ++j)
        meta.chunkHead[j] = chunk[j];

    // Store first min(tailOverflow, 3) bytes of the partial tail frame's header
    // (only meaningful when tailOverflow 1-3)
    if (meta.tailOverflow >= 1 && meta.tailOverflow <= 3) {
        const int partialStart = static_cast<int>(chunk.size()) - meta.tailOverflow;
        for (int j = 0; j < meta.tailOverflow; ++j)
            meta.tailHeadBytes[j] = chunk[partialStart + j];
    }

    // Store the full partial-tail-frame region (superset of tailHeadBytes above, and long
    // enough to cover a Case-1 split frame's side-info block, not just its 4-byte header) -
    // see decodeSplitFrameSideInfo.
    if (meta.tailOverflow > 0) {
        const int partialStart = static_cast<int>(chunk.size()) - meta.tailOverflow;
        meta.tailBytes.assign(chunk.begin() + partialStart, chunk.end());
    }

    return meta;
}

/**
 * Creates StreamProfile structure from input chunk
 *
 * @param chunk Input chunk from which  profile is derived.
 * @param out Output struct StreamProfile
 * @return True if the profile was constructed successfully.
 */
bool deriveProfile(const vector<uint8_t>& chunk, StreamProfile& out) {
    for (int i = 0; i + 4 <= static_cast<int>(chunk.size()); ++i) {
        const uint32_t raw = computeChunkHeader(chunk, i);

        if (!Mp3FrameScanner::isValidHeader(raw)) continue;

        int err;
        Header h(raw, err);
        if (err != 0) continue;
        const int frameLen = h.getFrameLength();
        if (frameLen <= 0) continue;

        if (isXingFrame(chunk, i, h)) {
            i += frameLen - 1; // skip to next frame (loop will ++i)
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
