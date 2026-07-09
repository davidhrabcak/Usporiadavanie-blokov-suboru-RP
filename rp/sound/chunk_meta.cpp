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
 * Reads the 'main_data_begin' pointer from an MP3 frame's side information block
 * @param chunk The buffer containing the raw MPEG audio stream data.
 * @param off   The starting index of the 4-byte MP3 frame header.
 * @return      The 9-bit 'main_data_begin'.
 *              Returns 0 if the offset is out of bounds of the provided chunk.
 */
static uint16_t readMainDataBegin(const vector<uint8_t>& chunk, const int off) {
    // First 9 bits of side-info block (immediately after the 4-byte header)
    if (off + 5 >= static_cast<int>(chunk.size())) return 0;
    return static_cast<uint16_t>(
        ((chunk[off + 4] << 1) | (chunk[off + 5] >> 7)) & 0x1FF
    );
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
        if (expected.isLayerIII())
            mdb = readMainDataBegin(chunk, i);
        out.push_back({i, len, h.getBitrate(), mdb});
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

    if (bestOff >= 0 && bestCount >= 1) {
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
