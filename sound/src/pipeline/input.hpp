#ifndef RP_INPUT_HPP
#define RP_INPUT_HPP

#include <cstdint>
#include <vector>
#include "config.hpp"
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
};

// Reads cfg.input and builds the Problem: chunks, CBR detection, the shuffle, the start chunk,
// the stream profile and the per-chunk metadata; then reports how the structural rule and the
// reservoir model fare on the source order. On a fatal error prints it, sets the run report's
// "error" and returns false.
bool loadProblem(const Config& cfg, Problem& p);

#endif
