#ifndef RP_SLOT_FILL_HPP
#define RP_SLOT_FILL_HPP

#include <cstdint>
#include <vector>

#include "chunk_meta.hpp"

// Stage B of PLAN_headerless_chunks.md: once the chunks with a header are ordered, put the
// headerless ("pool") chunks into the slots after the demanders. Exact min-cost assignment of
// slots to pool chunks on the PCM flux of each slot's window, with hard constraints.
namespace SlotFill {

struct Slot {
    std::vector<int> left;   // path tail ending with the demander a (oldest first), no other demander
    int b = -1;              // the chunk after the slot
    int after = -1;          // the chunk after b, or -1 (end, or b is a demander itself)
};

struct Stats {
    long decodes = 0;
    long excluded = 0;       // (slot, chunk) pairs ruled out by the header / CRC constraints
    long unmeasured = 0;     // pairs whose window gave no flux (silent / undecodable)
    int  forced = 0;         // slots that only got an excluded chunk (no allowed one was left)
    double seconds = 0;
};

// Cost of pool chunk m in slot s (lower = better). The right side of the window is m, b and, when
// known, the chunk after b: one chunk holds less than one frame at 320 kbps / 1 KB, and the flux
// only ranks successors well with two (PLAN_headerless_chunks.md, CLAUDE.md item 9).
// Constraints (pair excluded when violated):
//   - a Case-2 demander (split tail): m's first bytes must complete a's header to a frame whose
//     remainder ends exactly at a frame start of b;
//   - CRC-protected streams: the frame CRC over header bytes 2-3 + side info, assembled from a's
//     tail and m, must match (checked only when those bytes mix a and m, or lie in m).
// Returns, per slot, the pool index assigned (-1: none - more slots than pool chunks).
std::vector<int> assign(const std::vector<std::vector<uint8_t>>& chunks, const std::vector<ChunkMeta>& metas,
                        const std::vector<Slot>& slots, const std::vector<std::vector<uint8_t>>& pool,
                        long chunkSize, int threads, Stats* stats);

} // namespace SlotFill

#endif
