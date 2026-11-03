#ifndef RP_INPUT_HPP
#define RP_INPUT_HPP

#include <cstdint>
#include <vector>
#include "config.hpp"
#include "graph/chunk_graph.hpp"
#include "mp3/chunk_meta.hpp"

// What the reconstruction works on: the file cut into chunks and shuffled, the first chunk found
// from content and moved to index 0. The source order is kept for diagnostics only (the
// "[vs source: ...]" log lines and the final score); nothing that decides the order reads it.
struct Problem {
    std::vector<std::vector<uint8_t>> chunks;   // shuffled, chunk 0 = the found start
    std::vector<ChunkMeta> metas;               // per chunk, with `profile`
    std::vector<int> origIndexOf;               // source position of each chunk (diagnostic)
    StreamProfile profile;                      // from chunk 0, + the CBR bitrate filter
    long chunkSize = 0;

    // Frames longer than the chunk (PLAN_headerless_chunks.md): the chunks with no frame header
    // ("pool") are taken out before the shuffle and put into the slots after their demanders
    // at output.
    std::vector<std::vector<uint8_t>> pool;     // shuffled like the rest
    std::vector<int> poolFull;                  // their index in the full source stream
    std::vector<int> keptFull;                  // full-stream index of each chunk in `chunks` (source order); empty = no pool
    int splitTails = 0;                         // split-tail chunks promoted on the true order

    TrueOrderCheck truth;                       // gates AC-3, the reservoir arc filter and locking
};

// Reads cfg.input and builds the Problem: chunks, CBR detection, the headerless pool and the
// true-order diagnostic (before the shuffle), the shuffle, the start chunk, the stream profile
// and the per-chunk metadata. On a fatal error prints it, sets the run report's "error" and
// returns false.
bool loadProblem(const Config& cfg, Problem& p);

// The per-chunk metadata table (head offset, tail overflow, complete frames).
void printChunkTable(const std::vector<ChunkMeta>& metas);

#endif
