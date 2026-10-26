#include "chunk_meta.hpp"
#include "frame_scanner.hpp"
#include <algorithm>
#include <cstring>

using namespace std;

int sideInfoSize(const int versionID, const bool isMono) {
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
    const int xpos = off + 4 + h.crcBytes() + side;
    if (xpos + 4 > static_cast<int>(chunk.size())) return false;

    const uint8_t* ptr = &chunk[xpos];
    return std::memcmp(ptr, "Xing", 4) == 0 ||
       std::memcmp(ptr, "Info", 4) == 0 ||
       std::memcmp(ptr, "VBRI", 4) == 0;
}

// Caller must first check the chunk has at least 4 bytes remaining from `index`.
static uint32_t computeChunkHeader(const vector<uint8_t>& chunk, const int index) {
    return packHeaderBytes(&chunk[index]);
}

/**
 * Extracts `main_data_begin` and the sum of `part2_3_length` (actual main-data bits this frame
 * consumes from the bit reservoir) across every granule/channel, from a side-info block starting
 * at bit 0 of `sideBuf` (i.e. immediately after the frame's 4-byte header and CRC, if any).
 *
 * part2_3_length is returned in BITS, not bytes, and must stay that way: it's a bit-granular
 * quantity, and rounding it up to bytes on every frame compounds into a large false reservoir
 * deficit over a whole file. Only `mainDataBegin` is a byte-granular bitstream field.
 *
 * Side-info layout (ISO/IEC 11172-3 / 13818-3), all fields MSB-first. It starts right after the
 * header, or 2 bytes later when the header's protection bit is 0 (16-bit CRC) - callers skip it:
 *   MPEG1:      main_data_begin(9) + private_bits(mono:5/stereo:3) + scfsi(4 bits/channel),
 *               then 2 granules x numChannels blocks of 59 bits (leading 12 = part2_3_length).
 *   MPEG2/2.5:  main_data_begin(8) + private_bits(mono:1/stereo:2), no scfsi,
 *               then 1 granule x numChannels blocks of 63 bits (leading 12 = part2_3_length).
 *
 * @param sideBuf Bit source positioned at the start of the side-info block.
 * @return .valid is false if the side-info block doesn't fully fit in `sideBuf`.
 */
/**
 * Parses one granule/channel side-info block (59 bits MPEG1, 63 bits MPEG2/2.5 LSF) starting at
 * `bitPos`. Field order/widths per ISO/IEC 11172-3 2.4.1.7 (MPEG1) and 13818-3 (LSF: 9-bit
 * scalefac_compress, no preflag). Advances `bitPos` past the whole block.
 */
static GranuleSideInfo parseGranuleSideInfo(const SideInfoBitSource& src, int& bitPos, const bool mpeg1) {
    GranuleSideInfo g;
    g.part23Length     = static_cast<uint16_t>(readBitsMsb(src, bitPos, 12));
    g.bigValues        = static_cast<uint16_t>(readBitsMsb(src, bitPos, 9));
    g.globalGain       = static_cast<uint16_t>(readBitsMsb(src, bitPos, 8));
    g.scalefacCompress = static_cast<uint16_t>(readBitsMsb(src, bitPos, mpeg1 ? 4 : 9));
    g.windowSwitching  = static_cast<uint8_t>(readBitsMsb(src, bitPos, 1));

    if (g.windowSwitching) {
        g.blockType  = static_cast<uint8_t>(readBitsMsb(src, bitPos, 2));
        g.mixedBlock = static_cast<uint8_t>(readBitsMsb(src, bitPos, 1));
        g.tableSelect[0] = static_cast<uint8_t>(readBitsMsb(src, bitPos, 5));
        g.tableSelect[1] = static_cast<uint8_t>(readBitsMsb(src, bitPos, 5));
        g.tableSelect[2] = 0;
        for (int r = 0; r < 3; ++r)
            g.subblockGain[r] = static_cast<uint8_t>(readBitsMsb(src, bitPos, 3));
        // region0_count/region1_count are not in the bitstream for short blocks - the decoder
        // uses fixed conventions (see ISO 2.4.2.7). Not needed for the continuity probe; left 0.
    } else {
        for (int r = 0; r < 3; ++r)
            g.tableSelect[r] = static_cast<uint8_t>(readBitsMsb(src, bitPos, 5));
        g.region0Count = static_cast<uint8_t>(readBitsMsb(src, bitPos, 4));
        g.region1Count = static_cast<uint8_t>(readBitsMsb(src, bitPos, 3));
    }

    if (mpeg1)
        g.preflag = static_cast<uint8_t>(readBitsMsb(src, bitPos, 1));
    g.scalefacScale     = static_cast<uint8_t>(readBitsMsb(src, bitPos, 1));
    g.count1TableSelect = static_cast<uint8_t>(readBitsMsb(src, bitPos, 1));
    return g;
}

/**
 * Parses a full Layer III side-info block starting at bit 0 of `sideBuf` (immediately after the
 * frame's 4-byte header and its CRC, if any). See computeSideInfoFields's old comment (now below) for the
 * top-level layout. `.valid` is false if the block doesn't fully fit in `sideBuf`.
 */
static FrameSideInfo parseFrameSideInfo(const SideInfoBitSource& sideBuf, const bool mpeg1, const bool mono) {
    FrameSideInfo out;

    const int numChannels = mono ? 1 : 2;
    const int numGranules = mpeg1 ? 2 : 1;
    const int blockBits   = mpeg1 ? 59 : 63;
    const int mdbBits     = mpeg1 ? 9 : 8;
    const int privBits    = mpeg1 ? (mono ? 5 : 3) : (mono ? 1 : 2);
    const int scfsiBits   = mpeg1 ? numChannels * 4 : 0;

    const int totalSideBits = mdbBits + privBits + scfsiBits + numGranules * numChannels * blockBits;
    if (sideBuf.size() * 8 < totalSideBits) return out; // side-info block doesn't fully fit

    int bitPos = 0;
    out.mainDataBegin = static_cast<uint16_t>(readBitsMsb(sideBuf, bitPos, mdbBits));
    bitPos += privBits;
    if (mpeg1) {
        for (int c = 0; c < numChannels; ++c)
            out.scfsi[c] = static_cast<uint8_t>(readBitsMsb(sideBuf, bitPos, 4));
    } else {
        bitPos += scfsiBits; // 0 for MPEG2/2.5
    }
    out.numGranules = numGranules;
    out.numChannels = numChannels;

    uint32_t part23Sum = 0;
    for (int g = 0; g < numGranules; ++g) {
        for (int c = 0; c < numChannels; ++c) {
            const int blockStart = bitPos;
            out.gr[g][c] = parseGranuleSideInfo(sideBuf, bitPos, mpeg1);
            bitPos = blockStart + blockBits; // authoritative advance - keeps the part2_3_length
                                             // sum bit-identical to the pre-refactor skip loop
                                             // even if a per-field width above is ever wrong
            part23Sum += out.gr[g][c].part23Length;
        }
    }
    out.part23BitsTotal = part23Sum;
    out.valid = true;
    return out;
}

/**
 * Extracts `main_data_begin` and the sum of `part2_3_length` (actual main-data bits this frame
 * consumes from the bit reservoir) across every granule/channel, from a side-info block starting
 * at bit 0 of `sideBuf` (i.e. immediately after the frame's 4-byte header and CRC, if any).
 *
 * part2_3_length is returned in BITS, not bytes, and must stay that way: it's a bit-granular
 * quantity, and rounding it up to bytes on every frame compounds into a large false reservoir
 * deficit over a whole file. Only `mainDataBegin` is a byte-granular bitstream field.
 *
 * Side-info layout (ISO/IEC 11172-3 / 13818-3), all fields MSB-first. It starts right after the
 * header, or 2 bytes later when the header's protection bit is 0 (16-bit CRC) - callers skip it:
 *   MPEG1:      main_data_begin(9) + private_bits(mono:5/stereo:3) + scfsi(4 bits/channel),
 *               then 2 granules x numChannels blocks of 59 bits (leading 12 = part2_3_length).
 *   MPEG2/2.5:  main_data_begin(8) + private_bits(mono:1/stereo:2), no scfsi,
 *               then 1 granule x numChannels blocks of 63 bits (leading 12 = part2_3_length).
 *
 * Thin wrapper over parseFrameSideInfo - kept as the reservoir path's entry point so that code
 * doesn't carry the full per-granule struct it doesn't use.
 *
 * @param sideBuf Bit source positioned at the start of the side-info block.
 * @return .valid is false if the side-info block doesn't fully fit in `sideBuf`.
 */
static SideInfoResult computeSideInfoFields(const SideInfoBitSource& sideBuf, const bool mpeg1, const bool mono) {
    const FrameSideInfo f = parseFrameSideInfo(sideBuf, mpeg1, mono);
    SideInfoResult result;
    result.mainDataBegin = f.mainDataBegin;
    result.part23Bits    = static_cast<uint16_t>(f.part23BitsTotal);
    result.valid         = f.valid;
    return result;
}

/**
 * Reads `main_data_begin` and part2_3_length for a frame whose side info is fully contained in
 * one chunk (the ordinary case - `off` points at the frame's 4-byte header within `chunk`).
 * @return .valid is false if not Layer III or the side-info block doesn't fully fit in `chunk`.
 */
static SideInfoResult readFrameSideInfo(const vector<uint8_t>& chunk, const int off, const Header& h) {
    if (!h.isLayerIII()) return {};
    const int sideStart = off + 4 + h.crcBytes();
    if (sideStart >= static_cast<int>(chunk.size())) return {};
    const SideInfoBitSource src{&chunk[sideStart], static_cast<int>(chunk.size()) - sideStart};
    return computeSideInfoFields(src, h.getVersionID() == 0b11, h.isMono());
}

FrameSideInfo parseFrameSideInfoAt(const vector<uint8_t>& buf, const int off, const bool mpeg1, const bool mono) {
    if (off < 0 || off + 4 > static_cast<int>(buf.size())) return {};
    const int sideStart = off + 4 + ((buf[off + 1] & 1) ? 0 : 2);   // + CRC when protection bit is 0
    if (sideStart >= static_cast<int>(buf.size())) return {};
    const SideInfoBitSource src{&buf[sideStart], static_cast<int>(buf.size()) - sideStart};
    return parseFrameSideInfo(src, mpeg1, mono);
}

/**
 * Reads `main_data_begin` and part2_3_length for a Case-1 split frame whose side info may span
 * the chunk boundary: the header (and >=0 bytes of side info) sits in the tail of chunk `a`, and
 * the rest of the side info (if any) is the first bytes of the immediate successor chunk `b`.
 *
 * Whenever `a.tailPartialLen > 0` (Case 1), tryParse/computeChunkMeta guarantee the frame's full
 * 4-byte header was already read from within `a`, so `a.tailOverflow >= 4` and `a.tailBytes[0..4)`
 * is that header - no header re-decoding needed here.
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

    // header + CRC (protection bit 0); the CRC itself may straddle the chunk boundary
    const int skip = 4 + ((a.tailBytes[1] & 1) ? 0 : 2);
    const uint8_t* bHead = bBytes.data();
    int bLen = static_cast<int>(bBytes.size());
    const uint8_t* afterHeader = a.tailBytes.data() + std::min(skip, a.tailOverflow);
    const int afterHeaderLen   = std::max(0, a.tailOverflow - skip);
    if (a.tailOverflow < skip) {
        const int inB = skip - a.tailOverflow;
        if (bLen < inB) return {};
        bHead += inB;
        bLen  -= inB;
    }

    const SideInfoBitSource src{afterHeader, afterHeaderLen, bHead, bLen};
    return computeSideInfoFields(src, a.profile.versionID == 0b11, a.profile.isMono());
}


/**
 * Plausibility of the Layer III side info of the frame whose header is at `off`: 0 when it is
 * impossible for a real frame (big_values > 288, window switching with block_type 0, the unused
 * Huffman tables 4 / 14, more part2_3_length bits than the frame plus its reservoir can hold),
 * 1 when it passes, -1 when it can't be told (not Layer III, or the block runs past the chunk).
 * A false sync carries random side info and fails ~80% of the time; a real frame never does.
 */
static int sideInfoPlausible(const vector<uint8_t>& chunk, const int off, const Header& h) {
    if (!h.isLayerIII()) return -1;
    const int sideStart = off + 4 + h.crcBytes();
    if (sideStart >= static_cast<int>(chunk.size())) return -1;
    const SideInfoBitSource src{&chunk[sideStart], static_cast<int>(chunk.size()) - sideStart};
    const FrameSideInfo f = parseFrameSideInfo(src, h.getVersionID() == 0b11, h.isMono());
    if (!f.valid) return -1;
    for (int g = 0; g < f.numGranules; ++g)
        for (int c = 0; c < f.numChannels; ++c) {
            const GranuleSideInfo& s = f.gr[g][c];
            if (s.bigValues > 288) return 0;
            if (s.windowSwitching && s.blockType == 0) return 0;
            for (int k = 0; k < (s.windowSwitching ? 2 : 3); ++k)
                if (s.tableSelect[k] == 4 || s.tableSelect[k] == 14) return 0;
        }
    const long payload = static_cast<long>(h.getFrameLength()) - 4 - h.crcBytes()
                       - sideInfoSize(h.getVersionID(), h.isMono());
    return f.part23BitsTotal > 8L * (f.mainDataBegin + payload) ? 0 : 1;
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

        bool ok;
        Header h(raw, ok);
        if (!ok) break;
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
        FrameSlice fs{i, len, h.getBitrate(), mdb, part23};
        fs.crcBytes = static_cast<uint8_t>(h.crcBytes());
        out.push_back(fs);
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
                           const StreamProfile& expected,
                           const bool shortChunk) {
    ChunkMeta meta;
    meta.chunkIndex     = chunkIndex;
    meta.headOffset     = 0;
    meta.tailOverflow   = 0;
    meta.tailPartialLen = 0;
    meta.valid          = false;

    int bestCount    = 0;
    int bestQuality  = -1;   // -1 so the first candidate found always wins
    bool bestPlausible = true;
    int bestOff      = -1;
    vector<FrameSlice> bestFrames;
    int bestTail     = 0;
    int bestTailLen  = 0;

    const int limit = min(static_cast<int>(chunk.size()) - 4, MAX_FRAME_LENGTH - 1);

    for (int off = 0; off <= limit; ++off) {
        const uint32_t raw = computeChunkHeader(chunk, off);

        if (!Mp3FrameScanner::isValidHeader(raw)) continue;

        bool ok;
        Header h(raw, ok);
        if (!ok) continue;
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
        // Third key, ties only: a candidate whose side info is impossible loses (it used to be
        // "earlier offset wins"). At 320 kbps / 1 KB a chunk holds at most one real frame
        // start, so any false sync before it tied and won, giving the chunk a wrong tail
        // (1-7 true-order breaks per musiclib track; PLAN_headerless_chunks.md).
        const bool plausible = sideInfoPlausible(chunk, off, h) != 0;
        if (quality > bestQuality || (quality == bestQuality && count > bestCount) ||
            (quality == bestQuality && count == bestCount && plausible && !bestPlausible)) {
            bestPlausible = plausible;
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
    } else if (shortChunk && !chunk.empty() && static_cast<int>(chunk.size()) < MAX_FRAME_LENGTH) {
        // The tail of the final frame and nothing else (routine whenever the file's length mod
        // chunk size is shorter than the last frame). Without this the true order has a break
        // at the very end, which also disables AC-3 and edge locking for the whole file.
        meta.valid          = true;
        meta.finalFragment  = true;
        meta.headOffset     = static_cast<int>(chunk.size());
        meta.frameStarts    = {static_cast<int>(chunk.size())};
        meta.profile        = expected;
    }

    // Store first 4 bytes of the chunk for cross-chunk header reconstruction
    for (int j = 0; j < 4 && j < static_cast<int>(chunk.size()); ++j)
        meta.chunkHead[j] = chunk[j];

    // Store the full partial-tail-frame region: its first 1-3 bytes double as a split header's
    // leading bytes (Case 2), and the whole thing covers a Case-1 split frame's side-info block
    // once a successor chunk is known - see decodeSplitFrameSideInfo.
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
bool detectCBR(const vector<uint8_t>& stream, const StreamProfile& prof, int& kbpsOut, const int minFrames) {
    int i = 0, frames = 0, kbps = -1;
    while (i + 4 <= static_cast<int>(stream.size())) {
        const uint32_t raw = computeChunkHeader(stream, i);
        if (!Mp3FrameScanner::isValidHeader(raw)) break;
        bool ok;
        Header h(raw, ok);
        if (!ok || !prof.matches(h)) break;
        const int len = h.getFrameLength();
        if (len <= 0) break;
        const int br = h.getBitrate();
        if (kbps < 0) kbps = br;
        else if (br != kbps) return false; // a second bitrate -> not CBR
        // A Xing (as opposed to Info) header marks the stream as VBR even if these first
        // frames happen to share a bitrate - bail so we don't wrongly filter.
        if (frames == 0 && isXingFrame(stream, i, h)) {
            const int tag = i + 4 + h.crcBytes() + sideInfoSize(h.getVersionID(), h.isMono());
            if (tag + 4 <= static_cast<int>(stream.size()) &&
                std::memcmp(&stream[tag], "Xing", 4) == 0)
                return false;
        }
        ++frames;
        i += len;
    }
    if (frames < minFrames || kbps <= 0) return false;
    kbpsOut = kbps;
    return true;
}

bool deriveProfile(const vector<uint8_t>& chunk, StreamProfile& out) {
    // The Xing/Info/VBRI frame carries a valid header for the stream, so it is the fallback
    // when chunk 0 holds no other frame start (e.g. a 320 kbps Info frame is ~1044 bytes, so
    // skipping it runs past a 1024-byte chunk). Only chunk 0 is in true order, so we can't
    // look further.
    bool haveXing = false;
    StreamProfile xing{};
    for (int i = 0; i + 4 <= static_cast<int>(chunk.size()); ++i) {
        const uint32_t raw = computeChunkHeader(chunk, i);

        if (!Mp3FrameScanner::isValidHeader(raw)) continue;

        bool ok;
        Header h(raw, ok);
        if (!ok) continue;
        const int frameLen = h.getFrameLength();
        if (frameLen <= 0) continue;

        if (isXingFrame(chunk, i, h)) {
            if (!haveXing) {
                haveXing = true;
                xing.versionID  = h.getVersionID();
                xing.layerID    = h.getLayerID();
                xing.channelIdx = h.getChannelIdx();
                xing.sampleRate = h.getSampleRate();
                xing.hasCrc     = h.hasCrc();
            }
            i += frameLen - 1; // skip to next frame (loop will ++i)
            continue;
        }

        out.versionID  = h.getVersionID();
        out.layerID    = h.getLayerID();
        out.channelIdx = h.getChannelIdx();
        out.sampleRate = h.getSampleRate();
        out.hasCrc     = h.hasCrc();
        return true;
    }
    if (haveXing) { out = xing; return true; }
    return false;
}
