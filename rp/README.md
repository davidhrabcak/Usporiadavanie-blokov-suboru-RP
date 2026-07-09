# rp — Chunk Reconstruction

Research project on **file carving**: given a file that has been split into
equal-sized chunks and shuffled, reconstruct the original chunk order using
only structural/content heuristics — no filenames, no metadata, no external
state. Two independent subprojects apply this to different file types.

## Subprojects

### `text/`
Reconstructs a shuffled plain-text file. A training corpus is used to build a
frequency dictionary of 3-word sequences, which scores how plausible it is
for one chunk to follow another. A backtracking search then searches chunk
permutations, pruned by that scoring, to find valid reconstructions.

Implemented:
- `Segmenter` — splits an input file into fixed-size chunks
- `ThreeSequenceDictionaryCreator` / `ThreeSequenceDictionary` — builds and loads
  a word-sequence frequency dictionary from a training corpus
- `FrequencyThreeWordsValidator` — validates/scores a candidate chunk ordering
- `Backtrack` — backtracking search over chunk permutations, writes valid
  reconstructions to `found.txt`

### `sound/`
Reconstructs a shuffled MP3 file using MP3 frame-header structure. Each chunk
is analyzed for frame alignment, an adjacency graph is built from which
chunks can legally follow which others, and a DFS search (sped up by
collapsing forced chains) finds a valid reconstruction, verified with a
bit-reservoir sanity check.

Implemented:
- `Header` / `frame_scanner` — MPEG frame header parsing and stream scanning
- `chunk_meta` — per-chunk frame alignment and tail-byte analysis
- `adjacency` — builds the chunk-adjacency graph (`canFollow`)
- `main` — supernode collapse + DFS reconstruction, writes `output.mp3`

Known limitation: on constant-bitrate files, aliased frame headers can make
several chunks structurally indistinguishable, so the DFS finds *a* valid
ordering that isn't always the *original* one. See `sound/CLAUDE.md` for
full algorithm notes and open problems.

## Build & run

Each subproject builds independently with CMake.

### text
```
cd text
cmake -B build && cmake --build build
./build/main
```
Expects `in.txt` (the file to reconstruct, pre-chunked via `Segmenter`) and
`data.txt` (training corpus for the frequency dictionary) in the working
directory. Output is written to `found.txt`.

### sound
```
cd sound
cmake -B cmake-build-debug && cmake --build cmake-build-debug
./cmake-build-debug/rp path/to/input.mp3
```
The input file's first chunk is treated as pinned. Output is written to
`output.mp3` in the current directory.
