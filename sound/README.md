# sound - MP3 chunk reconstruction

An MP3 file is cut into equal chunks (1 KB by default) and shuffled. `rp` puts them back in
order from the bytes alone, using the MP3 format and the decoded audio.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/rp test/ci_sanity.mp3        # writes output.mp3, prints how much is in place
test/ci_sanity.sh build              # the end-to-end check CI runs
```

## Layout

```
src/main.cpp      the pipeline, start here
src/config.*      every RP_* option, parsed once
src/mp3/          the MP3 format: frame headers, per-chunk metadata (chunk_meta),
                  Layer III side info / main data, the bit-reservoir model
src/graph/        which chunk can follow which (adjacency, arc consistency),
                  gap slots for chunks with no frame header (slot_fill)
src/scoring/      junction scoring: decode a candidate junction to PCM and measure the
                  spectral flux (pcm_junction), lock confident edges (edge_lock)
tools/            measurement probes, linked against the same code (rpcore)
test/             scripts (ci_sanity, regression, corpus.py) and fixtures
docs/             HOW_IT_WORKS.md (the algorithm, chapter by chapter), PLAN_*.md, benchmarks
third_party/      minimp3 (CC0)
```

## Pipeline

1. Cut the file into chunks; find the stream profile and whether it is CBR.
2. Shuffle; find the first chunk from content.
3. Per chunk: where the frames start and how the last one spills over (`ChunkMeta`).
4. Build the graph of chunks that can structurally follow each other; prune it with the bit
   reservoir and arc consistency; lock edges whose decoded audio is clearly best.
5. Collapse forced chains, then search (DFS ranked by junction cost, global matching as a
   fallback) and repair the order on the summed PCM flux.
6. Write `output.mp3` and compare it with the source.

`docs/HOW_IT_WORKS.md` explains each step. `CLAUDE.md` holds the measurements, the open
problems and the full table of `RP_*` options.
