#ifndef RP_CHUNK_META_HPP
#define RP_CHUNK_META_HPP

#include <cstdint>
#include <vector>
#include "Header.hpp"

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

struct FrameSlice {
    int      offset;          // byte offset of frame header within chunk
    int      length;          // frame length in bytes
    int      bitrate;         // kbps
    uint16_t mainDataBegin;   // 9-bit side-info value (Layer III only, else 0)
};

struct ChunkMeta {
    int                   chunkIndex;
    int                   headOffset;      // bytes before first complete frame header
    int                   tailOverflow;    // bytes of the partial tail frame already in this chunk
    int                   tailPartialLen;  // total length of tail partial frame (0 = no tail, -1 = unknown)
    std::vector<FrameSlice> frames;        // complete frames fully within chunk
    bool                  valid;
    StreamProfile         profile;

    // First 4 bytes of the chunk (for cross-chunk header reconstruction when tailOverflow < 4)
    uint8_t chunkHead[4]{};
    // First min(tailOverflow, 3) bytes of the partial tail frame's header
    // (only relevant when 1 <= tailOverflow <= 3)
    uint8_t tailHeadBytes[3]{};
};

// Returns how many bytes of the tail partial frame must appear at the start
// of the next chunk (i.e. the expected headOffset of the successor chunk).
// Returns -1 if unknown (tailOverflow 1-3 and we need cross-chunk header).
inline int tailRemaining(const ChunkMeta& m) {
    if (m.tailPartialLen > 0) return m.tailPartialLen - m.tailOverflow;
    if (m.tailOverflow == 0)  return 0;
    return -1; // need cross-chunk header read
}

// MAX_FRAME_LENGTH: MPEG1 Layer I @ 320kbps / 32kHz + padding = 1441 bytes
static constexpr int MAX_FRAME_LENGTH = 1441;

ChunkMeta computeChunkMeta(int chunkIndex,
                           const std::vector<uint8_t>& chunk,
                           const StreamProfile& expected);

// Derive a StreamProfile from the very first valid frame in a chunk.
// Returns false if no valid header found.
bool deriveProfile(const std::vector<uint8_t>& chunk, StreamProfile& out);

#endif
