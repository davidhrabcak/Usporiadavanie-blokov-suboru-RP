#ifndef RP_RESERVOIR_HPP
#define RP_RESERVOIR_HPP

#include <cstdint>
#include <vector>
#include "chunk_meta.hpp"

// Layer III bit-reservoir model: does a candidate chunk order give every frame the main data
// its main_data_begin reaches back for? Used by the DFS (per placed chunk), the reservoir arc
// filter, the repair and the true-order diagnostic. Layer I/II have no reservoir.

// Bit-reservoir capacity in bytes (the largest main_data_begin): 511 MPEG1, 255 MPEG2/2.5.
inline int maxReservoirBytes(int versionID) { return versionID == 0b11 ? 511 : 255; }

/**
 * Advances the Layer III bit-reservoir level through a single frame and checks that
 * the frame's mainDataBegin is actually satisfiable by what has accumulated so far.
 *
 *
 * level_after = clamp(f.mainDataBegin*8 + producedBits - f.part23Bits, 0, maxReservoir*8).
 * Cross-checked against minimp3's L3_restore_reservoir/L3_save_reservoir: the "history"
 * contribution to level_after is `f.mainDataBegin`, NOT `level_before` - a frame only ever
 * reaches back exactly as far as its own mainDataBegin declares, so any reservoir surplus
 * beyond that is discarded, not carried forward. The separate check `f.mainDataBegin >
 * level_before` still applies: mainDataBegin(N) can never exceed what was genuinely
 * available after frame N-1.
 *
 * part2_3_length/producedBits are kept in BITS throughout, only compared against
 * mainDataBegin (byte-granular) at the byte level - rounding part2_3_length up to bytes
 * per frame would add ~7 bits of phantom consumption per frame, compounding into false
 * rejections over a whole file.
 *
 * @param level  Current reservoir fill level in bits; updated in place.
 * @param f      The frame to advance through.
 * @param sideInfo Side-information size in bytes for this stream (see sideInfoSize).
 * @param maxReservoir Reservoir capacity in bytes (511 for MPEG1, 255 for MPEG2/2.5).
 * @return False if `f.mainDataBegin` reaches further back than `level` allows, or if this
 *         frame's actual consumption (`part23Bits`) exceeds what became available.
 */
bool advanceReservoirFrame(int& level, const FrameSlice& f, const int sideInfo, const int maxReservoir);

/**
 * Advances the reservoir through every frame of a single chunk, in order. Used to
 * incrementally validate/extend the reservoir as each supernode is placed during the DFS,
 * rather than only checking the fully-assembled order at the end (see dfs()).
 *
 * `meta.frames` only holds *complete* frames (fully contained in this chunk) - a frame that
 * straddles a chunk boundary (routine, since chunk_size=1024 rarely divides evenly into
 * frame lengths of ~100-800 bytes) contributes no FrameSlice to *either* chunk on its own.
 *
 * When `prevMeta` is a Case-1 split (`prevMeta->tailPartialLen > 0`), the split frame's own
 * side info is decoded exactly from `prevMeta`'s tail bytes + `metaBytes`'s head
 * (`decodeSplitFrameSideInfo`, chunk_meta.cpp) and validated/consumed via `advanceReservoirFrame`
 * like any other frame - possible here because the DFS already knows both chunks of the
 * candidate edge being tried. If the exact decode isn't possible (not enough bytes to complete
 * the side-info block), this falls back to crediting `meta.headOffset` raw payload bytes without
 * validating the split frame's own mainDataBegin/part2_3_length.
 *
 * @param level     Current reservoir fill level in BITS; updated in place, even on failure
 *                  (the caller must restore it from a saved copy rather than trust it after false).
 * @param prevMeta  Metadata for the chunk immediately preceding `meta` in the candidate order,
 *                  or nullptr if `meta` is the first chunk placed (chunk 0).
 * @param meta      Metadata for the chunk being appended to the candidate order.
 * @param metaBytes Raw bytes of `meta`'s own chunk (needed to decode `prevMeta`'s split-frame
 *                  side info, which may extend past `prevMeta`'s own bytes).
 * @param sideInfo  Side-information size in bytes for this stream.
 * @param maxReservoir Reservoir capacity in bytes.
 * @return False as soon as any complete frame (or the exactly-decoded split frame) is invalid;
 *         true otherwise.
 */
bool advanceReservoirChunk(int& level,
                           const ChunkMeta* prevMeta,
                           const ChunkMeta& meta,
                           const std::vector<uint8_t>& metaBytes,
                           const int sideInfo,
                           const int maxReservoir);

// Whether the whole order `seq` (chunk indices into `metas` / `chunks`) passes the check,
// starting from an empty reservoir.
bool reservoirValid(const std::vector<ChunkMeta>& metas, const std::vector<std::vector<uint8_t>>& chunks,
                    const std::vector<int>& seq, const int sideInfo, const int maxReservoir);

// Number of chunks at which the reservoir check fails, continuing after each failure with a
// full reservoir (so later failures are counted independently). `where` gets their positions.
int reservoirBreaks(const std::vector<ChunkMeta>& metas, const std::vector<std::vector<uint8_t>>& chunks,
                    const std::vector<int>& seq, const int sideInfo, const int maxReservoir,
                    std::vector<int>* where = nullptr);

#endif
