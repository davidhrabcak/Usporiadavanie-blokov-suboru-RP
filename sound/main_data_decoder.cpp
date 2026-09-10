#include "main_data_decoder.hpp"
#include "huffman_tables.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace Mp3MainData {

using Mp3Huffman::kBigValueTable;
using Mp3Huffman::kBigValueTableIndex;
using Mp3Huffman::kLinbits;
using Mp3Huffman::kCount1TableA;
using Mp3Huffman::kCount1TableB;

// ---- bit reader -------------------------------------------------------------------------

uint32_t BitReader::peek(int n) const {
    // MSB-first, up to 24 bits, zero-padded past the end.
    if (n <= 0) return 0;
    if (n > 24) n = 24;
    uint32_t acc = 0;
    int got = 0;
    int bp = bitpos;
    while (got < n) {
        const int byteIdx = bp >> 3;
        const int bitIdx  = 7 - (bp & 7);
        const uint32_t bit = (byteIdx >= 0 && byteIdx < nbytes)
                             ? ((data[byteIdx] >> bitIdx) & 1u) : 0u;
        acc = (acc << 1) | bit;
        ++got;
        ++bp;
    }
    return acc;
}

// ---- MPEG1 long-block scalefactor-band boundaries (line index of each sfb start, + 576) ---

static const int kSfbLong44100[23] = {0,4,8,12,16,20,24,30,36,44,52,62,74,90,110,134,162,196,238,288,342,418,576};
static const int kSfbLong48000[23] = {0,4,8,12,16,20,24,30,36,42,50,60,72,88,106,128,156,190,230,276,330,384,576};
static const int kSfbLong32000[23] = {0,4,8,12,16,20,24,30,36,44,54,66,82,102,126,156,194,240,296,364,448,576};

static const int* sfbLongTable(int sampleRate) {
    if (sampleRate == 48000) return kSfbLong48000;
    if (sampleRate == 32000) return kSfbLong32000;
    return kSfbLong44100; // default / 44100
}

// MPEG1 scalefac_compress -> (slen1, slen2)
static void slenFor(int sfc, int& slen1, int& slen2) {
    static const int s1[16] = {0,0,0,0,3,1,1,1,2,2,2,3,3,3,4,4};
    static const int s2[16] = {0,1,2,3,0,1,2,3,1,2,3,1,2,3,2,3};
    if (sfc < 0 || sfc > 15) { slen1 = slen2 = 0; return; }
    slen1 = s1[sfc];
    slen2 = s2[sfc];
}

// Scalefactor bit count for one MPEG1 long-block granule/channel. scfsi groups (granule 1
// only): g0 = sfb 0-5 (6, slen1), g1 = sfb 6-10 (5, slen1), g2 = sfb 11-15 (5, slen2),
// g3 = sfb 16-20 (5, slen2). We skip the *values*, only the count matters here.
static int scaleFactorBits(const GranuleSideInfo& g, int granule, uint8_t scfsi, const GranuleSideInfo* gr0) {
    int slen1, slen2;
    slenFor(g.scalefacCompress, slen1, slen2);
    const int groupBands[4] = {6, 5, 5, 5};
    const int groupSlen[4]  = {slen1, slen1, slen2, slen2};
    int bits = 0;
    for (int grp = 0; grp < 4; ++grp) {
        const bool reused = (granule == 1 && gr0 && (scfsi & (1u << (3 - grp))));
        if (!reused) bits += groupBands[grp] * groupSlen[grp];
    }
    return bits;
}

// ---- Huffman decode -------------------------------------------------------------------

// Decodes one big_values pair from codebook `tsel`; returns magnitudes (>=0) and advances br.
// Returns false if the leaf chain runs away (corrupt / desynced).
static bool decodePair(BitReader& br, int tsel, int& x, int& y) {
    if (tsel == 0) { x = y = 0; return true; } // table 0: all zero, no bits
    const int16_t* cb = kBigValueTable + kBigValueTableIndex[tsel];
    const int linbits = kLinbits[tsel];

    int w = 5;
    int leaf = cb[br.peek(w)];
    int guard = 0;
    while (leaf < 0) {
        if (++guard > 6) return false;
        br.flush(w);
        w = leaf & 7;
        leaf = cb[br.peek(w) - (leaf >> 3)];
    }
    br.flush(leaf >> 8);

    int mag[2];
    for (int j = 0; j < 2; ++j) {
        int lsb = leaf & 0x0F;
        if (lsb == 15 && linbits) {
            lsb += static_cast<int>(br.peek(linbits));
            br.flush(linbits);
        }
        mag[j] = lsb;
        if (lsb) br.flush(1); // sign bit
        leaf >>= 4;
    }
    x = mag[0];
    y = mag[1];
    return true;
}

// Decodes one count1 quad; sets `nz` = number of non-zero (±1) values in the quad (0..4).
static bool decodeQuad(BitReader& br, int count1TableSelect, int& nz) {
    const uint8_t* cb = count1TableSelect ? kCount1TableB : kCount1TableA;
    int leaf = cb[br.peek(4)];
    if (!(leaf & 8)) {
        const int extra = leaf & 3;
        const int idx = (leaf >> 3) + static_cast<int>((br.peek(4 + extra)) & ((1u << extra) - 1u));
        leaf = cb[idx];
    }
    br.flush(leaf & 7);
    nz = 0;
    for (int s = 0; s < 4; ++s) {
        if (leaf & (128 >> s)) {
            ++nz;
            br.flush(1); // sign bit
        }
    }
    return true;
}

// ---- granule spectrum -----------------------------------------------------------------

static int lineToBand(int line, const int* sfb) {
    // sfb has 22 boundaries for bands 0..20 plus 576. Map to a coarse NBANDS bucket.
    int b = 20;
    for (int s = 0; s < 21; ++s) {
        if (line < sfb[s + 1]) { b = s; break; }
    }
    return (b * NBANDS) / 21;
}

GranuleSpectrum decodeGranuleSpectrum(BitReader& br, const GranuleSideInfo& g,
                                      int part23Length, int sampleRate,
                                      int granule, uint8_t scfsi, const GranuleSideInfo* gr0) {
    GranuleSpectrum out;
    const int blockStart = br.pos();
    const int part23End  = blockStart + part23Length;

    if (part23Length < 0) { out.valid = false; return out; }
    if (part23Length == 0) {
        // Silent granule/channel: no scalefactors, no Huffman data. A valid, all-zero spectrum
        // (this is real information for continuity - a silent boundary is very distinctive).
        out.valid = true;
        out.bitsConsumed = 0;
        return out;
    }

    // Any window-switching block (short/start/stop): region0_count/region1_count aren't in the
    // bitstream, there are only 2 Huffman regions (no table_select[2]), and short blocks use a
    // different sfb layout. Out of scope for the profile - skip (caller falls back to no signal).
    if (g.windowSwitching) {
        br.flush(part23Length);
        out.bitsConsumed = part23Length;
        out.valid = false;
        return out;
    }

    const int* sfb = sfbLongTable(sampleRate);

    // Skip scalefactor bits (values not needed for a raw-occupancy profile).
    const int scfBits = scaleFactorBits(g, granule, scfsi, gr0);
    br.flush(scfBits);

    const int bigPairs = g.bigValues;               // number of pairs in the big_values region
    int line = 0;

    // Region boundaries in coefficient (line) index. region0 = sfb [0 .. region0_count],
    // so region1 starts at sfb boundary [region0_count + 1]; region2 starts at
    // [region0_count + region1_count + 2]. sfb[] has 23 entries (0..22, last = 576).
    const int i0 = std::min(g.region0Count + 1, 22);
    const int i1 = std::min(g.region0Count + g.region1Count + 2, 22);
    int bound0 = sfb[i0];
    int bound1 = sfb[i1];
    bound0 = std::min(bound0, 2 * bigPairs);
    bound1 = std::min(std::max(bound1, bound0), 2 * bigPairs);

    float energy[NBANDS] = {0};
    int   nzCount[NBANDS] = {0};
    int   lineCount[NBANDS] = {0};
    int   totalNz = 0;

    auto accum = [&](int mag) {
        const int band = lineToBand(std::min(line, 575), sfb);
        ++lineCount[band];
        if (mag) { ++nzCount[band]; energy[band] += static_cast<float>(mag); ++totalNz; }
    };

    bool corrupt = false;
    for (int p = 0; p < bigPairs && line + 1 < 576; ++p) {
        int tsel;
        if      (line < bound0) tsel = g.tableSelect[0];
        else if (line < bound1) tsel = g.tableSelect[1];
        else                    tsel = g.tableSelect[2];

        int x = 0, y = 0;
        if (!decodePair(br, tsel, x, y)) { corrupt = true; break; }
        accum(x); ++line;
        accum(y); ++line;
        if (br.pos() > part23End + 64) { corrupt = true; break; } // ran well past the block
    }

    // count1 region: quads of 0/±1 until part23End (or 576 lines).
    while (!corrupt && br.pos() < part23End && line + 3 < 576) {
        int nz = 0;
        if (!decodeQuad(br, g.count1TableSelect, nz)) { corrupt = true; break; }
        for (int k = 0; k < 4; ++k) { accum(k < nz ? 1 : 0); ++line; }
        if (br.pos() > part23End + 64) { corrupt = true; break; }
    }

    out.bitsConsumed = br.pos() - blockStart;
    out.nonzero = totalNz;

    // Land the reader on the block boundary regardless (the caller advances by part23Length,
    // but keep it consistent for a standalone call).
    br.bitpos = part23End;

    if (corrupt) { out.valid = false; return out; }

    // Normalise: occupancy = fraction of lines non-zero per band; energy = L1-normalised.
    float esum = 0;
    for (int b = 0; b < NBANDS; ++b) esum += energy[b];
    for (int b = 0; b < NBANDS; ++b) {
        out.bandOccupancy[b] = lineCount[b] ? static_cast<float>(nzCount[b]) / lineCount[b] : 0.0f;
        out.bandEnergy[b]    = esum > 0 ? energy[b] / esum : 0.0f;
    }
    out.valid = true;
    return out;
}

FrameSpectra decodeFrameSpectra(const std::vector<uint8_t>& buf, int mainDataBitStart,
                                const FrameSideInfo& si, int sampleRate) {
    FrameSpectra fs;
    if (!si.valid) return fs;

    BitReader br{buf.data(), static_cast<int>(buf.size()), mainDataBitStart};

    for (int g = 0; g < si.numGranules; ++g) {
        for (int c = 0; c < si.numChannels; ++c) {
            const int blockStart = br.pos();
            const int p23 = si.gr[g][c].part23Length;
            const GranuleSideInfo* gr0 = (g == 1) ? &si.gr[0][c] : nullptr;
            GranuleSpectrum gs = decodeGranuleSpectrum(br, si.gr[g][c], p23, sampleRate, g, si.scfsi[c], gr0);
            fs.gr[g][c] = gs;
            if (gs.valid) ++fs.granulesDecoded;
            if (std::abs(gs.bitsConsumed - p23) > 8) ++fs.bitMismatch;
            br.bitpos = blockStart + p23; // authoritative advance
        }
    }
    fs.valid = fs.granulesDecoded > 0;
    return fs;
}

// A chunk's own main-data stream: the leading headOffset bytes (tail of the previous chunk's
// split frame - main data) followed by every complete frame's payload (bytes after its 4-byte
// header + side info). `payMdStart[k]` = md offset of frame k's payload.
static std::vector<uint8_t> buildChunkMainData(const std::vector<uint8_t>& bytes, const ChunkMeta& m,
                                               int sideSz, std::vector<int>& payMdStart) {
    payMdStart.clear();
    std::vector<uint8_t> md(bytes.begin(),
                            bytes.begin() + std::min<int>(std::max(0, m.headOffset), (int)bytes.size()));
    for (const auto& fr : m.frames) {
        payMdStart.push_back((int)md.size());
        const int ps = fr.offset + 4 + sideSz;
        const int pe = fr.offset + fr.length;
        if (ps >= 0 && pe <= (int)bytes.size() && ps < pe)
            md.insert(md.end(), bytes.begin() + ps, bytes.begin() + pe);
    }
    return md;
}

FrameSpectra decodeChunkFrame(const ChunkMeta* prevMeta, const std::vector<uint8_t>& prevBytes,
                              const std::vector<uint8_t>& chunk, const ChunkMeta& chunkMeta,
                              int frameOffset, int sampleRate) {
    FrameSpectra fs;
    if (!chunkMeta.valid || chunkMeta.frames.empty()) return fs;

    const bool mpeg1 = chunkMeta.profile.versionID == 0b11;
    const bool mono  = chunkMeta.profile.isMono();
    const int  sideSz = sideInfoSize(chunkMeta.profile.versionID, mono);

    std::vector<int> payMdStart;
    std::vector<uint8_t> chunkMd = buildChunkMainData(chunk, chunkMeta, sideSz, payMdStart);

    int frameIdx = -1;
    for (size_t k = 0; k < chunkMeta.frames.size(); ++k)
        if (chunkMeta.frames[k].offset == frameOffset) { frameIdx = (int)k; break; }
    if (frameIdx < 0) return fs;

    const FrameSideInfo fsi = parseFrameSideInfoAt(chunk, frameOffset, mpeg1, mono);
    if (!fsi.valid) return fs;

    // Prepend the predecessor's main-data stream so the reservoir back-pointer resolves.
    // prevMd ends exactly where chunkMd begins (chunkMd's leading headOffset bytes are the
    // continuation of prevMd's trailing split frame), so the concatenation is contiguous.
    std::vector<uint8_t> fullMd;
    int prevLen = 0;
    if (prevMeta && prevMeta->valid) {
        std::vector<int> pStarts;
        std::vector<uint8_t> prevMd = buildChunkMainData(prevBytes, *prevMeta, sideSz, pStarts);
        prevLen = (int)prevMd.size();
        fullMd = std::move(prevMd);
    }
    fullMd.insert(fullMd.end(), chunkMd.begin(), chunkMd.end());

    const int mdb = (int)fsi.mainDataBegin;
    const int startByte = prevLen + payMdStart[frameIdx] - mdb;
    if (startByte < 0) return fs; // reservoir reaches back past the predecessor chunk

    return decodeFrameSpectra(fullMd, startByte * 8, fsi, sampleRate);
}

} // namespace Mp3MainData
