#ifndef RP_CHUNK_META_HPP
#define RP_CHUNK_META_HPP

#include <cstdint>
#include <vector>
#include "chunk_meta.hpp"
#include "Header.hpp"

/**
 * Holds basic frame information that should remain similar across frames.
 */
struct StreamProfile {
    uint8_t versionID;
    uint8_t layerID;
    uint8_t channelIdx;
    int     sampleRate;

    bool isMono() const    { return channelIdx == 0b11; }
    bool isLayerIII() const{ return layerID == 0b01; }

    bool matches(const Header& h) const {
        return h.getVersionID()    == versionID
            && h.getLayerID()      == layerID
            && h.getSampleRate()   == sampleRate;
    }
};

// Result of extracting main_data_begin/part2_3_length from a Layer III side-info block -
// see computeSideInfoFields / decodeSplitFrameSideInfo in chunk_meta.cpp.
struct SideInfoResult {
    uint16_t mainDataBegin = 0;
    uint16_t part23Bits    = 0;
    bool     valid         = false; // false if not Layer III or the side-info block didn't fit
};

// Full per-granule/-channel Layer III side info (ISO 11172-3 2.4.1.7 / 13818-3). Everything a
// decoder needs *except* the main-data payload itself. Populated by parseFrameSideInfoAt;
// consumed by the side-info-only continuity probe (test/sideinfo_probe.cpp) and, later,
// Milestone 2's partial main-data decoder.
struct GranuleSideInfo {
    uint16_t part23Length     = 0; // 12 bits - main-data bits for this granule/channel
    uint16_t bigValues        = 0; // 9 bits  - half the count of the big_values spectral region
    uint16_t globalGain        = 0; // 8 bits  - granule quantiser step / loudness
    uint16_t scalefacCompress = 0; // 4 bits (MPEG1) / 9 bits (MPEG2 LSF)
    uint8_t  windowSwitching  = 0; // 1 bit
    uint8_t  blockType        = 0; // 2 bits (0 = normal long; 1 start, 2 short, 3 stop) - 0 if !windowSwitching
    uint8_t  mixedBlock       = 0; // 1 bit
    uint8_t  tableSelect[3]   = {0,0,0}; // 5 bits each ([2] unused when windowSwitching)
    uint8_t  subblockGain[3]  = {0,0,0}; // 3 bits each (windowSwitching only)
    uint8_t  region0Count     = 0; // 4 bits (long blocks only; short blocks: not in bitstream)
    uint8_t  region1Count     = 0; // 3 bits (long blocks only)
    uint8_t  preflag          = 0; // 1 bit (MPEG1 only)
    uint8_t  scalefacScale    = 0; // 1 bit
    uint8_t  count1TableSelect = 0; // 1 bit
};

// One frame's complete side info: main_data_begin + every granule/channel block.
struct FrameSideInfo {
    uint16_t mainDataBegin  = 0;
    uint32_t part23BitsTotal = 0; // sum of part23Length over all granules/channels (== SideInfoResult.part23Bits)
    int      numGranules    = 0;  // 2 (MPEG1) / 1 (MPEG2/2.5)
    int      numChannels    = 0;  // 1 (mono) / 2
    GranuleSideInfo gr[2][2];     // [granule][channel]
    bool     valid          = false;
};

struct FrameSlice {
    int      offset;          // byte offset of frame header within chunk
    int      length;          // frame length in bytes
    int      bitrate;         // kbps
    uint16_t mainDataBegin;   // 9-bit side-info value (Layer III only, else 0)
    uint16_t part23Bits;      // sum of part2_3_length across all granules/channels, in BITS
                               // (Layer III only, else 0) - actual main-data bits this frame
                               // consumes from the reservoir. Kept bit-precise (not rounded
                               // to bytes) so per-frame rounding doesn't compound into a
                               // systematic drift over many frames - see computeSideInfoFields.
};

struct ChunkMeta {
    int                   chunkIndex;
    int                   headOffset;      // bytes before first complete frame header
    int                   tailOverflow;    // bytes of the partial tail frame already in this chunk
    int                   tailPartialLen;  // total length of tail partial frame (0 = no tail, -1 = unknown)
    std::vector<FrameSlice> frames;        // complete frames fully within chunk
    bool                  valid;
    StreamProfile         profile;

    // All byte offsets within this chunk where a valid, profile-matching frame header starts.
    // Sorted ascending. Used as a fallback when headOffset was computed at a different
    // alignment than what the predecessor chunk's tailRemaining requires.
    std::vector<int> frameStarts;

    // First 4 bytes of the chunk (for cross-chunk header reconstruction when tailOverflow < 4)
    uint8_t chunkHead[4]{};

    // Full partial-tail-frame region of this chunk (size == tailOverflow), i.e. chunk[chunk.size()
    // - tailOverflow .. chunk.size()). When 1 <= tailOverflow <= 3 this doubles as the split
    // header's first bytes; when tailOverflow >= 4 it also covers a Case-1 split frame's side
    // info, reconstructed once a successor chunk is known - see decodeSplitFrameSideInfo.
    std::vector<uint8_t> tailBytes;
};

/**
 * Returns how many bytes of the tail partial frame must appear at the start
 * of the next chunk (i.e. the expected headOffset of the successor chunk).
 * Returns -1 if unknown (tailOverflow 1-3, and we need cross-chunk header).
 */
inline int tailRemaining(const ChunkMeta& m) {
    if (m.tailPartialLen > 0) return m.tailPartialLen - m.tailOverflow;
    if (m.tailOverflow == 0)  return 0;
    return -1; // need cross-chunk header read
}

// MAX_FRAME_LENGTH: MPEG1 Layer I @ 320kbps / 32kHz + padding = 1441 bytes
static constexpr int MAX_FRAME_LENGTH = 1441;

// Layer III side-info block size in bytes: MPEG1 32 (stereo) / 17 (mono), MPEG2/2.5 17 (stereo) / 9 (mono).
int sideInfoSize(int versionID, bool isMono);

ChunkMeta computeChunkMeta(int chunkIndex,
                           const std::vector<uint8_t>& chunk,
                           const StreamProfile& expected);

// Derive a StreamProfile from the very first valid frame in a chunk.
// Returns false if no valid header found.
bool deriveProfile(const std::vector<uint8_t>& chunk, StreamProfile& out);

// Decodes the real main_data_begin/part2_3_length of a Case-1 split frame (one whose 4-byte
// header and start of payload are in `a`'s tail, continuing into successor chunk `b`), by
// reconstructing its side-info block from both chunks' raw bytes. See chunk_meta.cpp for the
// Case-1 invariant this relies on (a.tailOverflow >= 4 whenever a.tailPartialLen > 0).
// Returns a result with .valid == false if `a` isn't a Case-1 split Layer III frame, or `b`
// doesn't hold enough bytes to complete the side-info block.
SideInfoResult decodeSplitFrameSideInfo(const ChunkMeta& a, const std::vector<uint8_t>& bBytes);

// Parses the full Layer III side-info block of the frame whose 4-byte header starts at `off`
// within `buf`. `mpeg1` = (versionID == 0b11); `mono` selects the 1- vs 2-channel layout.
// Returns a result with .valid == false if the side-info block doesn't fully fit in `buf`.
// Does not require `buf` to be a whole chunk - any byte range containing header + side info works.
FrameSideInfo parseFrameSideInfoAt(const std::vector<uint8_t>& buf, int off, bool mpeg1, bool mono);

#endif
