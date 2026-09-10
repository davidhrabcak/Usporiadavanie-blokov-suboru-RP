#ifndef RP_HUFFMAN_TABLES_HPP
#define RP_HUFFMAN_TABLES_HPP

#include <cstdint>

/**
 * Layer III "big_values" and "count1" Huffman code tables (ISO/IEC 11172-3 Annex B, tables
 * 0-31 for big_values pairs + tables A/B for the count1 quad region).
 *
 * This data is taken verbatim from minimp3 (https://github.com/lieff/minimp3, CC0 1.0 /
 * public domain), specifically the `tabs`/`tabindex`/`g_linbits`/`tab32`/`tab33` arrays in
 * `minimp3.h`'s `L3_huffman()` - copied rather than hand-transcribed from the ISO spec's own
 * table listings, per this project's practice of sourcing bit-exact reference data from a
 * working, widely-used decoder instead of re-deriving it (a transcription slip in ~2200
 * numbers would be very hard to notice later; a diff against a known-good decoder's source
 * is not).
 *
 * IMPORTANT - this is NOT a naive (code, length) -> (value0, value1) lookup table. It's
 * minimp3's compact self-describing VLC representation: `kBigValueTable` is indexed by
 * `kBigValueTableIndex[table_select] + peek(5 bits)`, and the entry (a "leaf") is one of:
 *   - leaf >= 0: a fully-resolved code. `leaf >> 8` is the number of bits this code actually
 *     occupies (<= 5); the low byte packs two 4-bit nibbles, one per value in the pair
 *     (`(leaf >> 4) & 0xF` and `leaf & 0xF`), each 0-15 - a nibble of 15 signals "escape",
 *     meaning `linbits` more bits follow in the bitstream as an unsigned extension added to 15
 *     (see `kLinbits`). Every value's sign is a single bit read from the stream *after* the
 *     magnitude (only present when the magnitude is nonzero).
 *   - leaf < 0: the code needs more than 5 bits to resolve. `leaf & 7` is how many *additional*
 *     bits to peek, and `-(leaf >> 3)` is added to that peek to index back into the same table
 *     for the final (always leaf >= 0) entry. This can chain more than once for the longest
 *     codes.
 * `kCount1TableA`/`kCount1TableB` (count1_table_select 0/1) use the same leaf encoding but
 * over a 4-value (quad) group instead of a pair, peeking 4 bits initially.
 * The actual bitstream-integrated decode loop (peek/flush against this project's own bit
 * reader, not minimp3's) is implemented separately - see the Huffman region decoder.
 *
 * Deliberately NOT included here (out of scope for table data - see later Milestone 2 steps):
 *   - `g_pow43`/`L3_pow_43` (magnitude requantization - needs its own step).
 *   - Scalefactor-band tables (`g_scf_long`/`g_scf_short` equivalents - scale factor decoding).
 */
namespace Mp3Huffman {

// Combined big_values codebook for all 32 table_select values, minimp3's `tabs`. Index into
// this via kBigValueTableIndex[table_select] + peek(w bits) per the leaf-decoding scheme above.
extern const int16_t kBigValueTable[2164];

// Offset into kBigValueTable for each table_select (0-31). Several table_select values that
// share the same codebook (differing only in kLinbits) alias to the same offset, matching the
// ISO spec (e.g. 16-23 all point at the same codebook as 16; 24-31 at the same as 24).
extern const int16_t kBigValueTableIndex[32];

// Escape "linbits" per table_select (0-31): extra unsigned bits appended to a decoded nibble
// of 15 to extend the magnitude range. 0 for table_select values with no escape mechanism.
extern const uint8_t kLinbits[32];

// count1 (quad) region codebooks - selected by the frame side info's count1table_select bit.
extern const uint8_t kCount1TableA[28]; // count1table_select == 0 (minimp3 `tab32`)
extern const uint8_t kCount1TableB[16]; // count1table_select == 1 (minimp3 `tab33`)

} // namespace Mp3Huffman

#endif
