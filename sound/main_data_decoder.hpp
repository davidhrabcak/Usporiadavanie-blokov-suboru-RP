#ifndef RP_MAIN_DATA_DECODER_HPP
#define RP_MAIN_DATA_DECODER_HPP

#include <cstdint>
#include <vector>
#include "chunk_meta.hpp"   // GranuleSideInfo, FrameSideInfo

// Milestone 2 - partial Layer III main-data decode for a spectral-continuity signal.
// MPEG1 Layer III only (every fixture is MPEG1); MPEG2/2.5 LSF is out of scope.
//
// The goal is NOT a full decoder - no IMDCT, no synthesis filterbank, and (for now) no
// requantisation. Just enough to recover, per granule, which of the 576 frequency lines carry
// energy, bucketed into a small per-band occupancy profile that adjacent audio frames should
// have in common. See PLAN_sideinfo_mainbits.md Milestone 2.

namespace Mp3MainData {

// MSB-first bit reader over an assembled main-data byte buffer.
struct BitReader {
    const uint8_t* data = nullptr;
    int            nbytes = 0;
    int            bitpos = 0;   // absolute bit offset from data[0]

    bool ok() const { return bitpos <= nbytes * 8; }
    int  pos() const { return bitpos; }

    uint32_t peek(int n) const;      // up to 24 bits; 0-padded past the end
    void     flush(int n) { bitpos += n; }
    uint32_t read(int n) { const uint32_t v = peek(n); bitpos += n; return v; }
};

// Number of scalefactor bands used for the region split / profile bucketing (MPEG1 long).
static constexpr int NBANDS = 12;

struct GranuleSpectrum {
    bool  valid = false;         // false if the granule couldn't be decoded (short block, ran off the buffer, bit-count mismatch)
    int   bitsConsumed = 0;      // main-data bits used (scalefactors + big_values + count1)
    int   nonzero = 0;           // total non-zero lines
    float bandOccupancy[NBANDS] = {0}; // fraction of lines non-zero, per coarse band (low->high freq)
    float bandEnergy[NBANDS]    = {0}; // sum of |raw huffman value| per band, L1-normalised over the granule
};

// Decodes one granule/channel's spectrum from `br` (positioned at the first scalefactor bit of
// this granule's part2_3 block) using `g` (its side info). `part23Length` is the granule's own
// value - decoding stops there. `prevScaleBitsReused`: for granule 1 with scfsi, the scalefactor
// bits are not re-sent; pass granule 0's per-scfsi-group slen contribution so we skip the right
// amount (0 for granule 0, or when not reusing).
//
// `sampleRate` selects the long-block scalefactor-band table (44100/48000/32000).
GranuleSpectrum decodeGranuleSpectrum(BitReader& br, const GranuleSideInfo& g,
                                      int part23Length, int sampleRate,
                                      int granule, uint8_t scfsi, const GranuleSideInfo* gr0);

// Convenience: given an assembled main-data buffer and a frame's full side info, decode every
// granule/channel and return them in [granule][channel] order. `mainDataBitStart` is the bit
// offset within `buf` where this frame's main data begins (i.e. after walking back
// `mainDataBegin` bytes from the frame header - the caller does that assembly).
struct FrameSpectra {
    bool valid = false;
    int  granulesDecoded = 0;
    int  bitMismatch = 0;         // # of granule blocks whose bitsConsumed != part23Length
    GranuleSpectrum gr[2][2];
};
FrameSpectra decodeFrameSpectra(const std::vector<uint8_t>& buf, int mainDataBitStart,
                                const FrameSideInfo& si, int sampleRate);

// Decode one complete frame of `chunk` - the frame whose header is at chunk byte offset
// `frameOffset` (must be a complete frame in `chunkMeta.frames`) - assembling its main data
// from the predecessor chunk's main-data stream (headers/side-info removed) followed by
// `chunk`'s. The Layer III bit reservoir routinely reaches into the previous chunk, so
// `prevMeta`/`prevBytes` are normally required; pass prevMeta = nullptr when `chunk` is first.
// Returns .valid = false when the reservoir reaches back past the predecessor, or the frame is
// window-switching.
FrameSpectra decodeChunkFrame(const ChunkMeta* prevMeta, const std::vector<uint8_t>& prevBytes,
                              const std::vector<uint8_t>& chunk, const ChunkMeta& chunkMeta,
                              int frameOffset, int sampleRate);

} // namespace Mp3MainData

#endif
