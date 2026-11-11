# rp — Chunk Reconstruction

Research project on **file carving**: a file is split into equal-sized chunks and shuffled,
and the original order is reconstructed from the chunks' content alone — no filenames, no
metadata, no external state. Two independent subprojects apply this to different file types.

## `text/`

Reconstructs a shuffled plain-text file. A training corpus builds a frequency dictionary of
3-word sequences, which scores how plausible it is for one chunk to follow another; a
backtracking search over chunk permutations, pruned by that score, finds reconstructions.

- `Segmenter` — splits an input file into fixed-size chunks
- `ThreeSequenceDictionaryCreator` / `ThreeSequenceDictionary` — build and load the dictionary
- `FrequencyThreeWordsValidator` — scores a candidate ordering
- `Backtrack` — the search; writes reconstructions to `found.txt`

```
cd text
cmake -B build && cmake --build build
./build/main
```

Expects `in.txt` (the file to reconstruct, pre-chunked via `Segmenter`) and `data.txt` (the
training corpus) in the working directory.

## `sound/`

Reconstructs a shuffled MP3 file from the MP3 format and the decoded audio: a graph of which
chunk can structurally follow which, pruned with the bit reservoir, scored by how smooth each
junction sounds, then ordered by a ranked search and global matching with repair. Byte-exact
on 192 of 367 real-music inputs at 4 KB chunks (2026-11-09).

```
cd sound
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/rp path/to/input.mp3      # writes output.mp3
```

See `sound/README.md` (layout), `sound/docs/HOW_IT_WORKS.md` (the algorithm) and
`sound/CLAUDE.md` (options, tests, results, open problems).
