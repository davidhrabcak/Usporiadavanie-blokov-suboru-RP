# MP3 File Carving — Chunk Reconstruction

## Project Goal

Research prototype for **file carving**: given an MP3 file that has been split into equal-sized chunks (1024 bytes) and shuffled, reconstruct the original chunk order using only MP3 structural heuristics — no filename, no metadata, no external state.

Chunk 0 is always pinned (never shuffled); it serves as the anchor for stream profile derivation and the DFS start.

---

## File Structure

| File | Role |
|---|---|
| `Header.cpp / .hpp` | Decodes a raw 32-bit MP3 frame header. Computes frame length, bitrate, sample rate, version, layer, channel mode. |
| `frame_scanner.cpp / .hpp` | Scans a full MP3 file byte-by-byte, collecting frame positions and metadata. Also exposes `isValidHeader()` used everywhere. |
| `chunk_meta.cpp / .hpp` | Core per-chunk analysis. `computeChunkMeta()` scans a 1024-byte chunk to find the best frame alignment and populate `ChunkMeta`. `deriveProfile()` extracts the stream's invariant parameters from the first valid frame. |
| `adjacency.cpp / .hpp` | Builds the directed adjacency graph over chunks. `canFollow(a, b)` decides whether chunk b can legally follow chunk a. `buildAdjacency()` constructs the full graph using a `byFrameStart` index for O(n) Case-1 lookups. |
| `main.cpp` | Orchestrates the pipeline: load → profile → shuffle → per-chunk metadata → adjacency graph → supernode collapse → DFS reconstruction → write output. |

---

## Algorithm Pipeline

### Phase 1 — Pre-shuffle sanity check
Runs `computeChunkMeta` on the original (unshuffled) chunks and verifies that every consecutive pair satisfies `canFollow`. Diagnostic only; output is discarded.

### Phase 2 — Stream profile derivation
`deriveProfile()` reads the first valid, non-Xing frame from chunk 0 and records: MPEG version, layer, sample rate, channel mode. All subsequent frame validation uses this profile.

### Phase 3 — Per-chunk metadata (`ChunkMeta`)
`computeChunkMeta()` scans each chunk from offset 0 to `min(chunk_size-4, MAX_FRAME_LENGTH-1)`, trying every position as a potential frame start. For each position that passes `isValidHeader` and profile matching:
- Records the position in `frameStarts` (sorted list of all valid frame-start offsets)
- Calls `tryParse()` to count consecutive complete frames from that offset

The offset with the most consecutive frames becomes `headOffset`. Tail metadata is set:
- `tailOverflow`: bytes of the partial tail frame inside this chunk
- `tailPartialLen`: full length of the tail frame if its header is readable (≥4 bytes overlap), else -1
- `tailHeadBytes[3]`: first 1–3 bytes of a split tail frame header (when `tailOverflow` ∈ {1,2,3})
- `chunkHead[4]`: first 4 bytes of the chunk (used for cross-chunk header reconstruction)

### Phase 4 — Adjacency graph
`canFollow(a, b)` implements two cases:

**Case 1** (`tailRemaining(a) >= 0`): the tail frame's full length is known.
`rem = tailPartialLen - tailOverflow` is the number of bytes from the tail frame that must appear at the start of chunk b. Checks `hasFrameAt(b, rem)` — i.e., that `rem` is a known valid frame start in b's `frameStarts` list.

**Case 2** (`tailOverflow` ∈ {1,2,3}): the tail frame's 4-byte header is split across chunks.
Reconstructs the header from `a.tailHeadBytes[0..tov-1]` + `b.chunkHead[0..3-tov]`, validates it, and checks that `b` has a frame at `frameLen - tov`.

`buildAdjacency()` uses a `byFrameStart` hash map (frameStart position → list of chunk indices) for O(1) Case-1 lookups. Case-2 chunks still require an O(n) scan.

### Phase 5 — Supernode (forced-chain) collapse
Identifies maximal forced chains: runs where every node has out-degree 1 and its successor has in-degree 1. These chains are merged into single supernodes, reducing the DFS search space.

### Phase 6 — DFS over supernodes
Exhaustive DFS over the supernode graph, starting from the supernode containing chunk 0. At a complete ordering, `validateReservoir()` runs a Layer III bit-reservoir check (monotonically accumulates bytes produced per frame; rejects if any frame's `mainDataBegin` exceeds the accumulated total).

### Phase 7 — Output
Concatenates chunks in the found order and writes `output.mp3`.

---

## Key Data Structures

### `StreamProfile`
Invariant stream parameters used to filter false frame headers:
- `versionID`, `layerID`, `sampleRate`, `channelIdx`

### `ChunkMeta`
Per-chunk analysis result:
- `headOffset` — byte offset of the first complete frame in the chunk
- `tailOverflow` — bytes of the partial tail frame present in this chunk
- `tailPartialLen` — full frame length of the tail frame (−1 if unreadable)
- `frameStarts` — **sorted** list of all offsets in this chunk where a valid, profile-matching frame header exists
- `frames` — list of `FrameSlice` (offset, length, bitrate, mainDataBegin) for complete frames only
- `chunkHead[4]` — first 4 bytes of the chunk
- `tailHeadBytes[3]` — first bytes of a split tail-frame header

### `tailRemaining(m)`
Inline helper: returns `tailPartialLen - tailOverflow` (≥0) if the tail frame's length is known, 0 if no tail, or −1 if the header is split across chunks.

---

## Known Limitations and Open Problems

### 1. CBR files — DFS is infeasible

**Update**: largely resolved for *tractability* (see TODO "Fix computeChunkMeta scoring" below) - `CBR.mp3` (658 chunks) now reconstructs in ~0.17s instead of never terminating. The composite `(tailQuality, frameCount)` scoring in `computeChunkMeta` was the actual fix: it turned out most of the ~32-way branching wasn't inherent aliasing, it was `computeChunkMeta` itself picking a `headOffset` with a worse (or absent) tail purely because it had a higher raw frame count, which fed bad `tailOverflow`/`tailPartialLen` into every downstream adjacency check. Once headOffset selection prefers a clean tail first, the branching factor collapses enough for the existing DFS + supernode collapse to finish quickly.

**What's still open**: tractability is not the same as *correctness*. Diffing `output.mp3` against the original (after skipping the leading ID3 tag both start with) shows chunk 0 matches exactly (it's pinned) but chunk 1 onward diverges from the true original order - the DFS is finding *a* structurally- and reservoir-valid Hamiltonian path, not necessarily *the* original one. CBR streams genuinely can contain multiple chunks that are indistinguishable from header/structure metadata alone (see the "byte-level verification" idea below), so recovering the exact original order still needs a stronger signal than what's implemented. Original symptom/root-cause analysis kept below for context.

**Original symptom**: `buildAdjacency` produced ~21,400 edges (~32 successors per chunk) and the DFS never terminated.

**Original root cause analysis**: in a constant-bitrate stream all frames have the same length (e.g., 418 bytes). With `gcd(1024, 418) = 2`, only 209 distinct `headOffset` values cycle across 658 chunks, so any chunk starting at one of those positions looks structurally identical to ~3 others. This is still true today for the frame *headers* (CBR headers really do repeat byte-for-byte); what changed is that `computeChunkMeta` no longer hands adjacency a wrong `headOffset` on top of that, which was multiplying the ambiguity far beyond what the header aliasing alone would cause.

**On "byte-level junction verification" (the fix originally proposed here)**: attempting this literally - store the last `tailOverflow` bytes of chunk a and the first `tailRemaining` bytes of chunk b and compare them - does not actually work: those are two *different, adjacent* ranges of the same split frame's body, not two copies of the same bytes, so there is nothing to assert equality against without already knowing the true order. The one byte-level check that *is* meaningful (the reservoir's `mainDataBegin`) is covered under item 3 below. Genuinely disambiguating aliased CBR headers likely needs one of the "Alternative approaches" further down (padding-bit tracking, spectral continuity, or exhaustive Hungarian matching), none of which are implemented.

### 2. `computeChunkMeta` picks the wrong headOffset for some CBR chunks

**Status: fixed.** See TODO "Fix computeChunkMeta scoring for clean-tail alignment" below - `computeChunkMeta` now ranks candidate offsets by `(tailQuality, frameCount)` instead of `frameCount` alone, so a 1-frame offset with a cleanly-readable tail beats a 2-frame offset with a split/orphaned tail. This was the main contributor to item 1's branching-factor blowup, not just the 7 residual pre-shuffle breaks originally described here.

### 3. `validateReservoir` does not model bit-reservoir consumption

**Status: partially addressed, with an important caveat.** The check now runs incrementally per supernode placement during the DFS (`advanceReservoirChunk`/`advanceReservoirFrame` in `main.cpp`) instead of once at the end, so a bad reservoir chain is pruned as soon as it occurs rather than after a full ordering is assembled.

True consumption modelling (subtracting each frame's *actual* compressed-data length from the reservoir) was attempted and reverted: it requires `part2_3_length` (summed per granule/channel), which nothing in this codebase parses. A first attempt substituted `mainDataBegin` itself as "bytes consumed this frame" - that's wrong (mainDataBegin is a backward *pointer*, not a consumption amount) and it was caught empirically: instrumenting `sample-3s.mp3`'s known-correct order showed `mainDataBegin` sitting near the 511-byte cap on nearly every frame while each frame only produces ~382 bytes, so the subtract-based level went net-negative every frame, saturated at 0 within a handful of chunks, and then rejected the true ordering outright. The check has been kept at the older, weaker, accumulate-only formula (never subtracts) - it still won't catch much beyond the first chunk or two (same limitation as before), it just now runs earlier. Modelling real consumption remains future work and needs `part2_3_length` parsing added to `Header`/`chunk_meta`.

### 4. Case 2 still O(n) per chunk

**Status: fixed.** `buildAdjacency` now groups valid chunks by their `chunkHead` prefix (`byChunkHeadPrefix`, one map per `tov` value 1-3) and reconstructs/validates the candidate header once per distinct prefix bucket instead of once per chunk in the file - see TODO "Resolve Case-2 O(n) scan" below.

### 5. Non-audio embedded content causes unresolvable breaks

**Symptom**: some chunks in `CBR.mp3` have `tailOverflow > 3` and `tailPartialLen = −1` (orphaned tail bytes). `canFollow` correctly returns false for these, but they represent genuine consecutive pairs in the original file.

**Likely cause**: non-audio blocks (e.g., embedded ID3v2 chapter tags or PRIV frames) inserted mid-stream. The frame parser stops at the non-audio content, leaves the remaining bytes as "orphaned", and the tail metadata becomes useless. These chunks cannot be correctly connected without parsing the non-audio content.

---

## TODO / Future Improvements

### High priority — fixes to the current algorithm

- [ ] **Store junction bytes in `ChunkMeta` and verify them in `canFollow`.**
  **Investigated, not implemented as originally described - see "On byte-level junction verification" under Known Limitations item 1.** The literal version (compare chunk a's tail bytes against chunk b's head bytes for equality) doesn't hold up: those are different, non-overlapping ranges of one split frame's body, not duplicate copies of the same bytes, so there's nothing valid to assert there. What actually fixed the CBR branching explosion was the `computeChunkMeta` scoring fix below, plus the reservoir check now running per-edge instead of only at the end. Real junction disambiguation for aliased CBR headers is still open; see the "Alternative approaches" section.

- [x] **Fix `computeChunkMeta` scoring for clean-tail alignment.**
  Implemented in `chunk_meta.cpp` (`tailQuality()` + the composite comparison in `computeChunkMeta`). The "best offset" metric is now `(tailQuality, frameCount)` compared with `tailQuality` first, where `tailQuality` ranks: `tailOverflow == 0` (chunk ends exactly on a frame boundary, no ambiguity at all) above `tailPartialLen > 0` (clean readable tail) above `tailPartialLen == -1` with a 1-3 byte split header (Case 2 recoverable) above everything else (orphaned/unrecoverable). This is checked *before* frame count, per the worked example in Known Limitations item 2, and empirically turned `CBR.mp3` from non-terminating into a ~0.17s reconstruction.

- [x] **Incremental `validateReservoir` inside the DFS.**
  Implemented in `main.cpp` (`advanceReservoirFrame`/`advanceReservoirChunk`, threaded through `DfsState`/`dfs()`). A candidate supernode's reservoir validity is now checked (and `state.reservoirLevel` saved/restored) as it's tried, not deferred to a full ordering. The "model consumption by subtracting frame length" half of this item was attempted and reverted - see Known Limitations item 3 for why (it requires `part2_3_length`, which isn't parsed, and substituting `mainDataBegin` for it breaks real files). The check itself is still the older accumulate-only approximation; only *when* it runs changed.

- [x] **Resolve Case-2 O(n) scan.**
  Implemented in `adjacency.cpp` (`byChunkHeadPrefix` index + `packPrefix()`). Valid chunks are grouped by the exact `chunkHead` prefix bytes Case 2 needs (one map per `tov` value 1-3); each distinct prefix bucket reconstructs and validates its candidate header once instead of once per chunk in the file.

- [ ] **Handle embedded non-audio content (ID3v2 mid-stream, PRIV frames).**
  Not attempted this pass. Before computing `ChunkMeta`, scan each chunk for embedded ID3v2 tags (`ID3` magic at any offset). If found, skip the tag body and resume frame parsing from after it. Record the tag size so `tailRemaining` can account for the non-audio bytes when computing expected successor `headOffset`.

- [x] **Correctly detect and skip Xing/Info/VBRI headers in `computeChunkMeta`.**
  Implemented in `chunk_meta.cpp` (`isXingFrame` moved above `tryParse` and called from inside its loop). A Xing/Info/VBRI frame still counts towards `i`'s advance and is still kept in `out`/`frames` (its bytes are real and need to end up in the reconstructed output, and dropping it from the reservoir accounting would be its own inaccuracy), but it no longer inflates `count`, so it can no longer bias which offset `computeChunkMeta` picks as `headOffset`.

---

### Medium priority — algorithm improvements

- [ ] **Bitrate as a discriminator in `frameStarts`.**
  Extend `frameStarts` from `vector<int>` (positions) to `vector<pair<int,int>>` (position, bitrate). In VBR streams, consecutive chunks should have correlated bitrate sequences. In `canFollow` Case 1, additionally verify that the bitrate of the frame at `rem` in chunk b is consistent with the last frame of chunk a (e.g., within a plausible VBR range). Add `bitrate` to `StreamProfile` for CBR files to filter false-positive frame headers that happen to have a different bitrate index.

- [ ] **Supernode-level reservoir pre-check.**
  Before admitting a supernode `s` as a candidate successor of the current DFS path, check whether the mainDataBegin of `s`'s first chunk's first frame is satisfiable given the maximum possible accumulated reservoir from the current path. Use the maximum reservoir (511 / 255 bytes) as a ceiling: if `mainDataBegin > min(maxReservoir, accumulated_so_far + max_production_of_s)`, prune without recursing.

- [ ] **Forced-chain detection on `frameStarts` rather than `headOffset`.**
  The current supernode collapse uses the computed `headOffset` for in/out degree counting. For chunks where `headOffset` was computed incorrectly (wrong CBR alignment), the degree counts are wrong and forced chains are not identified. Recompute degrees based on the `byFrameStart` index so that all valid frame positions are considered.

- [ ] **Detect and handle padding-bit patterns for CBR.**
  In CBR streams, the padding bit follows a deterministic sequence (approximately one padded frame per `sampleRate / (bitrate * 1000 / 8)` frames). Tracking the expected padding-bit sequence from chunk 0 and verifying it at each junction would distinguish between correct and incorrect successors in CBR files without needing raw byte comparison.

---

### Alternative / new approaches

- [ ] **Byte-hash junction fingerprinting.**
  At every frame boundary within a chunk, store a short rolling hash (e.g., 8-byte FNV or xxHash) of the surrounding bytes. Two chunks are adjacent iff the hash of the last bytes of a matches the hash of the corresponding first bytes of b. Requires no change to the DFS — just adds a fast hash comparison to `canFollow`. Works for both CBR and VBR.

- [ ] **MDCT / spectral continuity scoring.**
  Decode the MDCT coefficients for the last granule of chunk a and the first granule of chunk b. Adjacent chunks in the original file should have spectrally similar granules (psychoacoustic continuity). Build a similarity score and use it to rank successors in the DFS, or as a hard threshold to prune edges. This is the strongest possible signal but requires a partial MP3 decoder.

- [ ] **Greedy best-first search instead of DFS.**
  Replace the exhaustive DFS with a beam search or A* over the supernode graph. Define a heuristic cost (e.g., mainDataBegin delta, spectral distance, bitrate continuity) and always expand the most-promising successor. Falls back to backtracking only on dead ends. Useful when the correct ordering can be ranked first by the heuristic, avoiding the exponential worst case of DFS.

- [ ] **Constraint propagation (arc consistency) before DFS.**
  After building the adjacency graph, apply AC-3 or similar: if chunk j is the only valid successor of chunk i, force that edge and remove all other successors of i and all other predecessors of j. Repeat until no more forced assignments can be made. This pre-collapse stage can dramatically reduce the search space before DFS begins, complementing the existing supernode approach.

- [ ] **Graph-matching / Hungarian algorithm for dense CBR.**
  Model chunk ordering as a minimum-weight perfect matching on a bipartite graph: left nodes are "chunk as predecessor", right nodes are "chunk as successor". Edge weights encode junction dissimilarity (byte difference, spectral distance, reservoir delta). The Hungarian algorithm finds the globally optimal 1-to-1 matching in O(n³), which is tractable for n < 10,000. This completely sidesteps the exponential DFS.

- [ ] **Multi-hypothesis tracking with a particle filter.**
  Maintain a population of partial orderings (particles). At each step, extend each particle greedily by the highest-scoring next chunk, then resample by likelihood. Converges faster than DFS for large n when the score function is informative. Suitable for very long files (n > 1000 chunks) where even a good DFS is impractical.

- [ ] **Learning-based chunk similarity (offline trained model).**
  Train a small neural network or gradient-boosted classifier offline on known MP3 files to predict whether two chunks are adjacent, using features extracted from their `ChunkMeta` (headOffset, tailOverflow, frameStarts pattern, bitrate sequence, mainDataBegin values). At reconstruction time, use the classifier's confidence as edge weights in a minimum-weight Hamiltonian path solver. Generalises across bitrates, encoders, and content types.

---

## Build

```
cmake -B cmake-build-debug && cmake --build cmake-build-debug
./cmake-build-debug/rp [input.mp3]
```

Output is written to `output.mp3` in the current directory. Chunk 0 of the input file is always treated as pinned (first in the output).
