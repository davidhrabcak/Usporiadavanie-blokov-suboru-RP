#ifndef RP_ADJACENCY_HPP
#define RP_ADJACENCY_HPP

#include "chunk_meta.hpp"
#include <vector>
//True if chunk b can legally follow chunk a.
bool canFollow(const ChunkMeta& a, const ChunkMeta& b);

std::vector<std::vector<int>> buildAdjacency(const std::vector<ChunkMeta>& metas);

// --- Frames longer than the chunk (PLAN_headerless_chunks.md) ---
// A full-size chunk with no frame header at all: it lies wholly inside one frame.
bool isHeaderless(const ChunkMeta& m, size_t chunkBytes, long chunkSize);
// For every chunk that must be followed by a headerless chunk ("demander"), the frame offsets
// allowed in the chunk after that slot; empty for every other chunk.
//   Case 1 (whole tail header in the chunk): rem = tailRemaining >= C  ->  {rem - C}.
//   Case 2 (1-3 header bytes): the header completed with each pool chunk's first bytes,
//          {L - tov - C : L - tov >= C}.
// A Case-2 chunk that also has an ordinary successor in `adj` is ambiguous and left empty.
struct GapSlots {
    std::vector<std::vector<int>> offsets;
    int case1 = 0, case2 = 0, ambiguous = 0;
    int demanders() const { return case1 + case2; }
};
GapSlots findGapSlots(const std::vector<ChunkMeta>& metas, const std::vector<std::vector<int>>& adj,
                      const std::vector<std::vector<uint8_t>>& pool, long chunkSize);
// Split-tail chunks: no whole header, just the end of one frame and the first t = 1-3 bytes of
// the next header. Promoted in place to valid chunks (frame start at C - t, Case-2 tail of t
// bytes) when all of:
//   - E_t > 0: some valid chunk's tail leaves exactly C - t bytes for the next chunk (only a
//     split tail can follow it - frame starts are scanned up to C - 4);
//   - the last t bytes complete to a profile-matching header with `proto` (a real header of
//     the stream) supplying the rest;
//   - the first bytes don't complete a header for any t' in {1, 2} with E_t' > 0 (that is what
//     the interior chunk after a split tail looks like). t' = 3 can't be tested: the chunk would
//     supply only header byte 3, which is almost always valid.
// Measured on musiclib (1 KB, CBR bitrate filter on): 4 false positives and 16 false negatives
// among 10854 headerless chunks; split-tail count == E_t on 87 of 101 files. Content only, so it
// gives the same result before and after the shuffle. Returns promoted / expected counts by t.
struct SplitTails { int promoted[4] = {0, 0, 0, 0}; int expected[4] = {0, 0, 0, 0}; };
SplitTails promoteSplitTails(std::vector<ChunkMeta>& metas, const std::vector<std::vector<uint8_t>>& chunks,
                             long chunkSize, const StreamProfile& profile, const uint8_t proto[4]);
// a's Case-2 tail (1-3 header bytes) completed with the first bytes of `next`: the offset in
// `next` where that frame ends (frame length - tov), or -1 if the header is invalid/off-profile.
int splitHeaderOffset(const ChunkMeta& a, const std::vector<uint8_t>& next);
// True if `off` is one of b's frame starts.
bool hasFrameStart(const ChunkMeta& b, int off);
// True if b has a frame start at one of `offsets` (b follows a's slot).
bool canFollowGap(const ChunkMeta& b, const std::vector<int>& offsets);
// Replaces each demander's successor list with its gap edges a => b.
void addGapEdges(std::vector<std::vector<int>>& adj, const std::vector<ChunkMeta>& metas, const GapSlots& g);

// Arc-consistency prune (A4): in any full ordering every chunk but the last has one successor
// and every chunk but `start` one predecessor, so a chunk whose in- or out-neighbourhood is
// already a singleton forces that edge and lets the competing edges be deleted; deletions
// cascade to a fixpoint. Also deletes every edge into `start`. Returns the number of edges
// removed, or -1 (with `adj` restored) if propagation hit a contradiction - which means
// `canFollow` is missing a real edge (e.g. embedded non-audio, item 5), so forcing on the
// survivors would be unsafe.
long propagateArcConsistency(std::vector<std::vector<int>>& adj, int n, int start);

std::vector<int> computeInDegree(const std::vector<std::vector<int>>& adj, int n);
std::vector<int> computeOutDegree(const std::vector<std::vector<int>>& adj, int n);

#endif
