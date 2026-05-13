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

**Symptom**: for `CBR.mp3` (658 chunks), `buildAdjacency` produces ~21,400 edges (~32 successors per chunk). The DFS never terminates.

**Root cause**: in a constant-bitrate stream all frames have the same length (e.g., 418 bytes). With `gcd(1024, 418) = 2`, only 209 distinct `headOffset` values cycle across 658 chunks. Any chunk that starts at one of those 209 positions looks structurally identical to ~3 other chunks that start at the same position. The `byFrameStart` index correctly finds ALL of them as valid successors — they are not false sync words, they are genuine frame headers — but the wrong ones.

**What is needed**: byte-level verification at the chunk junction. The last `tailOverflow` bytes of chunk a and the first `tailRemaining` bytes of chunk b together form the body of the split frame. Verifying that those bytes are consistent (e.g., by storing them in `ChunkMeta` and comparing at adjacency-check time) would collapse the branching factor from ~32 to ~1.

**Current workaround**: none. CBR reconstruction does not complete.

### 2. `computeChunkMeta` picks the wrong headOffset for some CBR chunks

**Symptom**: 7 pre-shuffle breaks remain for `CBR.mp3` even after all adjacency fixes.

**Root cause**: `computeChunkMeta` selects the headOffset that maximises the number of consecutive complete frames. In CBR files, multiple starting positions can produce the same (maximum) frame count. The greedy winner is always the lowest offset — which is not always the true frame boundary. The wrong headOffset causes the tail metadata to be wrong, which creates a double break: the wrongly-aligned chunk fails as a successor of its predecessor, and also fails as a predecessor of its own successor.

**Example**: true alignment gives 1 complete frame with tailOvfl=357, tailPartialLen=418. Greedy picks 2 frames with tailOvfl=2, tailPartialLen=−1 (2 bytes of next header split). headOffset difference: 186 (chosen) vs 249 (correct).

**What is needed**: a composite scoring metric in `computeChunkMeta` that prefers alignments with a clean, readable tail (tailPartialLen > 0) over those with an orphaned or split tail, even if the frame count is lower. Or, equivalently, the adjacency check should accept any `rem` that corresponds to a valid frame position in chunk b — which `frameStarts` now enables — rather than requiring `headOffset == rem`.

**Current status**: partially mitigated. The `frameStarts` fallback in `canFollow` fixes 7 of the original 14 breaks. The remaining 7 have `tailPartialLen = −1` with large `tailOverflow` (orphaned bytes), which are cases where the wrong alignment produces a frame run that terminates mid-chunk with no readable next header. These cannot be recovered by `frameStarts` alone.

### 3. `validateReservoir` does not model bit-reservoir consumption

**Symptom**: the reservoir check provides no pruning during the DFS for files with 2+ frames per chunk.

**Root cause**: `validateReservoir` only accumulates bytes produced per frame and caps at `maxReservoir` (511 for MPEG1). It never subtracts consumed bytes. After just 1–2 chunks with 2 frames each (2 × 382 = 764 bytes produced > 511), `accumulated` saturates at 511 and every subsequent `mainDataBegin ≤ 511` check trivially passes. The check is therefore only effective for the very first chunk in the ordering.

**What is needed**: proper consumption modelling — after each frame's audio data is "read" from the reservoir, subtract its length from `accumulated`. This would keep `accumulated` at a realistic level and allow meaningful pruning during the DFS. Alternatively, move reservoir validation into the DFS as an incremental check per supernode placed, rather than a final check at the end.

### 4. Case 2 still O(n) per chunk

**Symptom**: chunks with `tailOverflow` ∈ {1,2,3} (split 4-byte header) scan all n chunks to find valid successors.

**Root cause**: the expected successor's `headOffset` depends on bytes from the candidate chunk b (`b.chunkHead`), so the required position cannot be determined without reading b. Building a secondary index keyed on `chunkHead` prefixes would allow O(1) lookup at the cost of additional bookkeeping.

**Impact**: low for typical files (few Case-2 chunks per run). Not the current bottleneck.

### 5. Non-audio embedded content causes unresolvable breaks

**Symptom**: some chunks in `CBR.mp3` have `tailOverflow > 3` and `tailPartialLen = −1` (orphaned tail bytes). `canFollow` correctly returns false for these, but they represent genuine consecutive pairs in the original file.

**Likely cause**: non-audio blocks (e.g., embedded ID3v2 chapter tags or PRIV frames) inserted mid-stream. The frame parser stops at the non-audio content, leaves the remaining bytes as "orphaned", and the tail metadata becomes useless. These chunks cannot be correctly connected without parsing the non-audio content.

---

## TODO / Future Improvements

### High priority — fixes to the current algorithm

- [ ] **Store junction bytes in `ChunkMeta` and verify them in `canFollow`.**
  The single most impactful change. For Case 1, store the last `min(tailOverflow, N)` bytes of the partial tail frame (e.g., N=16) as `tailBodySuffix` in `ChunkMeta`, and store the first `min(tailRemaining, N)` bytes of each chunk as an extended `chunkPrefix`. In `canFollow`, verify that `a.tailBodySuffix` matches the corresponding prefix of `b.chunkPrefix`. This collapses the ~32-way branching in CBR files to ~1 and makes the DFS tractable.

- [ ] **Fix `computeChunkMeta` scoring for clean-tail alignment.**
  Extend the "best offset" metric from a single frame-count integer to a composite: `(frameCount, tailQuality)` where `tailQuality` ranks `tailPartialLen > 0` above `tailPartialLen == -1` (split header) above orphaned bytes (tailPartialLen = -1, tailOverflow > 3). This eliminates the second class of pre-shuffle breaks where the greedy 2-frame alignment is chosen over a 1-frame alignment with a readable tail.

- [ ] **Incremental `validateReservoir` inside the DFS.**
  Move the bit-reservoir check from the terminal condition into the per-supernode placement step. Thread `reservoirLevel` through `DfsState` and update it as each supernode is placed. Also model consumption: subtract frame audio data length from the reservoir after each frame, not just accumulate. This converts the reservoir check from a post-hoc filter into an actual pruning mechanism.

- [ ] **Resolve Case-2 O(n) scan.**
  Build a secondary index keyed on the first 3 bytes of `chunkHead` (one index per `tov` value 1, 2, 3). For a chunk with `tailOverflow = tov`, only candidates whose `chunkHead[0..3-tov]` could form a valid reconstructed header need to be checked. Eliminates the linear scan for split-header chunks.

- [ ] **Handle embedded non-audio content (ID3v2 mid-stream, PRIV frames).**
  Before computing `ChunkMeta`, scan each chunk for embedded ID3v2 tags (`ID3` magic at any offset). If found, skip the tag body and resume frame parsing from after it. Record the tag size so `tailRemaining` can account for the non-audio bytes when computing expected successor `headOffset`.

- [ ] **Correctly detect and skip Xing/Info/VBRI headers in `computeChunkMeta`.**
  Currently `deriveProfile` skips Xing frames, but `computeChunkMeta` counts them as ordinary frames. A Xing frame at the very beginning of a chunk will inflate `bestCount` and bias `headOffset`. Add the `isXingFrame` check inside `tryParse` so these frames are excluded from the count.

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
