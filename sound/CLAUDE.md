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
| `huffman_tables.cpp / .hpp` | Static Layer III Huffman code tables (big_values tables 0-31 + count1 quad tables A/B), sourced verbatim from [minimp3](https://github.com/lieff/minimp3) (CC0) rather than hand-transcribed - see the header for the (non-obvious, compact self-describing VLC) leaf encoding these tables use. Data only so far - not yet wired into a decoder; see `PLAN_sideinfo_mainbits.md` Milestone 2. |
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

**2026-07-09 correction - the "~0.17s" claim above does not currently reproduce.** Running today's build against the same, untouched `CBR.mp3` (mtime 2026-03-24, i.e. unchanged since before the commit that wrote the claim above) does not terminate within 30-60s, and `main.cpp`'s own diagnostic prints `Adjacency graph: 1/658 chunks have exactly one valid successor` - i.e. the supernode collapse has almost nothing to collapse (a graph that sparse can't plausibly finish an exhaustive DFS over 658 nodes in 0.17s on just the weak accumulate-only reservoir check). Investigation so far, before being asked to stop and write this up:

- **Ruled out: the fix in item 6/TODO "Fix computeChunkMeta's validity gate for VBR" (this session's change to `computeChunkMeta`'s `bestOff >= 0` gate).** Verified with `git stash` on `chunk_meta.cpp` to A/B the pre- and post-fix binary against `CBR.mp3` directly: both report the identical `1/658` adjacency stat and both fail to terminate in the same timeframe. This change is not the cause.
- **Ruled out: `aa8c7ab` ("Dedup frame header parsing").** Diffed directly - it only extracts the repeated 4-byte header-parsing into `computeChunkHeader()`; mechanically identical to the inlined version it replaced, no logic changed.
- **Ruled out: `612acac` ("improve code consistency").** Only touched `frame_scanner.cpp` (removed already-dead commented-out logging), and only touched the same dead code this session's frame_scanner TODO fix also touched - see below.
- **Ruled out: this session's `frame_scanner.cpp` TODO fix.** `Header::decode()` leaves `frameLength` at its zero-initialized default on every failure path, and `scanFrames()` already skipped forward via its own `frameLength <= 0` check regardless - the TODO fix (`++i; continue;` on decode failure) is behaviorally a no-op for this, it only stops relying on that implicitly. Confirmed by inspection, not just assumption (see this session's history above under the frame_scanner TODO writeup).
- **Ruled out: `CBR.mp3` itself being swapped for a different file.** It's untracked (never committed) so `git` has no history for it, but its mtime (2026-03-24) predates even the commit that wrote the "~0.17s" claim (`27988a6`, 2026-07-08) and hasn't changed since - the file sitting in `sound/test/` today is the same file that would have been used for that original measurement, if one was actually run against it.
- **`adjacency.cpp` is unchanged since `27988a6`** (confirmed with `git log --follow`) - so the graph-construction logic that produces the `1/658` stat has not changed since the commit that documented 0.17s. Since out-degree is a property of each chunk's own byte content (invariant to the random shuffle order - shuffling only permutes which array index holds which chunk, not which chunks' content can follow which), this `1/658` figure would have been exactly the same back when `27988a6` was written too.

**Working hypothesis, not yet confirmed**: given the above, either (a) the "~0.17s" claim was never actually re-measured against `CBR.mp3` after being written (an unverified/aspirational documentation claim), or (b) DFS completion time is highly sensitive to the shuffle's random seed even for a *fixed* graph - `adj[i]`'s candidate order depends on chunk index, which the shuffle changes, so a "lucky" seed could stumble onto the correct full ordering quickly while an "unlucky" one backtracks through much more of the space - and the original measurement simply got a lucky run. (a) seems more likely given how sparse the forced-chain structure (`1/658`) is, but this needs multiple timed runs across several random seeds to actually confirm or rule out (b); that was in progress (about to test by pointing `FILENAME` at `CBR.mp3` and timing several runs) when this session was asked to stop testing and write up findings instead. **Next step for whoever picks this up**: run `./cmake-build-debug/rp` against `CBR.mp3` several times (each gets a fresh random shuffle seed from `chrono::system_clock::now()`) and record wall-clock time per run - if variance is huge (some sub-second, some never finishing), it's (b) and the fix is a smarter DFS ordering/heuristic (see "Greedy best-first search" and "Constraint propagation" under Alternative approaches); if it's consistently slow/non-terminating, the "~0.17s" claim is simply wrong and item 1's "largely resolved" status above should be walked back.

**What's still open**: tractability is not the same as *correctness*. Diffing `output.mp3` against the original (after skipping the leading ID3 tag both start with) shows chunk 0 matches exactly (it's pinned) but chunk 1 onward diverges from the true original order - the DFS is finding *a* structurally- and reservoir-valid Hamiltonian path, not necessarily *the* original one. CBR streams genuinely can contain multiple chunks that are indistinguishable from header/structure metadata alone (see the "byte-level verification" idea below), so recovering the exact original order still needs a stronger signal than what's implemented. Original symptom/root-cause analysis kept below for context.

**Original symptom**: `buildAdjacency` produced ~21,400 edges (~32 successors per chunk) and the DFS never terminated.

**Original root cause analysis**: in a constant-bitrate stream all frames have the same length (e.g., 418 bytes). With `gcd(1024, 418) = 2`, only 209 distinct `headOffset` values cycle across 658 chunks, so any chunk starting at one of those positions looks structurally identical to ~3 others. This is still true today for the frame *headers* (CBR headers really do repeat byte-for-byte); what changed is that `computeChunkMeta` no longer hands adjacency a wrong `headOffset` on top of that, which was multiplying the ambiguity far beyond what the header aliasing alone would cause.

**On "byte-level junction verification" (the fix originally proposed here)**: attempting this literally - store the last `tailOverflow` bytes of chunk a and the first `tailRemaining` bytes of chunk b and compare them - does not actually work: those are two *different, adjacent* ranges of the same split frame's body, not two copies of the same bytes, so there is nothing to assert equality against without already knowing the true order. The one byte-level check that *is* meaningful (the reservoir's `mainDataBegin`) is covered under item 3 below. Genuinely disambiguating aliased CBR headers likely needs one of the "Alternative approaches" further down (padding-bit tracking, spectral continuity, or exhaustive Hungarian matching), none of which are implemented.

### 2. `computeChunkMeta` picks the wrong headOffset for some CBR chunks

**Status: fixed.** See TODO "Fix computeChunkMeta scoring for clean-tail alignment" below - `computeChunkMeta` now ranks candidate offsets by `(tailQuality, frameCount)` instead of `frameCount` alone, so a 1-frame offset with a cleanly-readable tail beats a 2-frame offset with a split/orphaned tail. This was the main contributor to item 1's branching-factor blowup, not just the 7 residual pre-shuffle breaks originally described here.

### 3. `validateReservoir` does not model bit-reservoir consumption

**Status: fixed, with a caveat on split frames.** The check runs incrementally per supernode placement during the DFS (`advanceReservoirChunk`/`advanceReservoirFrame` in `main.cpp`), and (2026-07-11) now does real two-sided consumption modelling using `part2_3_length`, not just the old accumulate-only approximation - see the TODO "Implement real `part2_3_length`-based reservoir consumption" for the two bugs found and fixed while building this (wrong update-anchor formula, and a chunk-boundary split-frame accounting gap), both confirmed against minimp3 as an independent reference decoder.

A first attempt at true consumption modelling substituted `mainDataBegin` itself as "bytes consumed this frame" - that's wrong (mainDataBegin is a backward *pointer*, not a consumption amount) and was caught empirically: instrumenting `sample-3s.mp3`'s known-correct order showed `mainDataBegin` sitting near the 511-byte cap on nearly every frame while each frame only produces ~382 bytes, so the subtract-based level went net-negative every frame and rejected the true ordering outright. That's why the fix needed real `part2_3_length` parsing (now implemented) rather than a stand-in.

**Caveat**: the current model doesn't validate/consume a frame that straddles a chunk boundary using its own `mainDataBegin`/`part2_3_length` - only its payload byte *contribution* to the reservoir is credited (see the TODO for why - doing the full check needs a combined view of both chunks in a candidate transition, which the current per-chunk-in-isolation loop doesn't have). This makes the check conservative (never wrongly rejects a valid chain, confirmed against 5 fixtures) but weaker than a complete model would be - see item 7's 2026-07-11 update for the measured DFS impact (none, on `test7_shorter.mp3`).

**2026-07-18 update - this caveat is now closed for Case-1 split frames** (branch `sideinfo-mainbits`, see `PLAN_sideinfo_mainbits.md` Milestone 1). `ChunkMeta` now stores the chunk's full tail-region bytes (`tailBytes`, not just the first 3), and `decodeSplitFrameSideInfo` (`chunk_meta.cpp`) reconstructs a split frame's real side-info block by concatenating `a.tailBytes` (after the 4-byte header) with the candidate successor `b`'s head bytes - possible because `tryParse` already guarantees the header is fully readable inside `a` whenever `tailPartialLen > 0` (Case 1), so `a.tailOverflow >= 4`. `advanceReservoirChunk` now calls this at exactly the point the DFS tries a candidate edge (both chunks of the transition are known there) and runs the result through the normal `advanceReservoirFrame` check/consume, falling back to the old byte-credit only if the exact decode isn't possible. This also fixed a latent, previously-dormant bug: the old separate `readMainDataBegin` always read 9 bits regardless of MPEG version, when MPEG2/2.5 uses 8 - dormant only because every existing fixture is MPEG1.

Validated against all 5 fixtures' true unshuffled order: `sample-3s.mp3`, `test7_shorter.mp3`, `test7_short.mp3`, and `CBR.mp3` all still report `"Reservoir check PASSED on true order"` with zero breaks. `test7.mp3` (the full, uncut track) reports 2 reservoir breaks at chunks 108 and 642 onward - **confirmed pre-existing, not a regression**: A/B'd against the unmodified baseline binary (same file), which shows the identical 2 structural `canFollow` breaks at 107->108 and 641->642 (`tailOvfl=11 tailPartialLen=365 headOff=363` and `tailOvfl=347 tailPartialLen=835 headOff=63`) - i.e. these are chunks our own structural heuristic already couldn't connect (likely item 5's embedded non-audio content, unconfirmed), and CLAUDE.md's prior claim that `test7.mp3` "passes... with 0 breaks" doesn't reproduce even on the unmodified baseline - another stale/unverified claim in the vein of the CBR "~0.17s" saga above, not something this change introduced.

**Measured impact (pinned-seed A/B, same methodology as item 7's 2026-07-11 entry)**: this is a genuine accuracy win, not a pure wash. On `sample-3s.mp3` (51 chunks, tractable to full completion), the exact check finds a substantially more accurate reconstruction than the old conservative-credit model at the same pinned seed: exact-position 73.6% vs 55.2%, Kendall's tau +0.666 vs +0.443, adjacent-run coverage 94.4% vs 66.4% (`evaluate_order`), at a real but modest absolute cost (1.04s vs 0.007s wall-clock - both trivially fast at this file size). On the harder VBR fixture `test7_shorter.mp3`, a fixed-call-count benchmark (both binaries stopped at exactly 20,000,000 raw `dfs()` calls) shows the exact check costs about 22x more wall-clock per call (8.73s vs 0.40s) - expected, since it now does real per-candidate validation work instead of an unconditional byte credit, and a stricter check means more candidates get rejected (more backtracking) rather than accepted. Neither the old nor new model completes the full DFS on `test7_shorter.mp3` within a 60s budget at this seed, so a full-completion wall-clock comparison isn't available for this fixture - consistent with this file's already-documented intractability (item 7). **Net takeaway**: closing this gap measurably improves *which* answer the DFS lands on when it can complete (small/CBR-like files), but doesn't by itself fix the large-VBR-file tractability problem (still item 7's open branching-factor issue) - the two are different axes (answer quality vs. search-space size).

**New data point (2026-07-09, real-world VBR file `test7_shorter.mp3`)**: unlike `sample-3s.mp3`, `mainDataBegin` here is *not* saturated - across all 859 true frames it spans the full 0-511 range (only 2 frames are 0, only 44% exceed 480). This was expected to be a strong discriminating signal for VBR content - see item 7's 2026-07-11 update for why it didn't translate into a measured DFS speedup in practice.

### 4. Case 2 still O(n) per chunk

**Status: fixed.** `buildAdjacency` now groups valid chunks by their `chunkHead` prefix (`byChunkHeadPrefix`, one map per `tov` value 1-3) and reconstructs/validates the candidate header once per distinct prefix bucket instead of once per chunk in the file - see TODO "Resolve Case-2 O(n) scan" below.

### 5. Non-audio embedded content causes unresolvable breaks

**Symptom**: some chunks in `CBR.mp3` have `tailOverflow > 3` and `tailPartialLen = −1` (orphaned tail bytes). `canFollow` correctly returns false for these, but they represent genuine consecutive pairs in the original file.

**Likely cause**: non-audio blocks (e.g., embedded ID3v2 chapter tags or PRIV frames) inserted mid-stream. The frame parser stops at the non-audio content, leaves the remaining bytes as "orphaned", and the tail metadata becomes useless. These chunks cannot be correctly connected without parsing the non-audio content.

### 6. VBR files - `computeChunkMeta` wrongly invalidates chunks whose only frame doesn't fully fit

**Status: fixed (documentation here was stale).** The fix described below (`chunk_meta.cpp:206`, `bestOff >= 0` instead of `bestOff >= 0 && bestCount >= 1`) was actually applied in commit `c83ffda`, in the same commit that wrote this section - the code was changed but this status line and the TODO checkbox below were never updated to match, so they sat for two commits describing a fix as "not yet applied" that had already shipped. Verified live: `./rp` against `test7_shorter.mp3` now prints `Pre-shuffle check PASSED: all 320 consecutive pairs compatible` (0 breaks, matching the 17→0 prediction below).

**Test fixtures added (2026-07-09)**: `test7.mp3` / `test7_short.mp3` / `test7_shorter.mp3` - a real-world VBR-encoded song (Justin Bieber, "Baby"; full track / ~41s / ~22s clips), added because every existing fixture (`CBR.mp3`, `sample-3s.mp3`, `test1..6*`) is constant-bitrate. Frame sizes in `test7_shorter.mp3` range from 104 to 835 bytes (vs. a fixed 417/418 in the CBR fixtures) - genuine VBR, confirmed with `ffprobe -show_entries frame=pkt_size`.

**Symptom**: running the *pre-shuffle* sanity check (Phase 1, on the true, unshuffled order) against `test7_shorter.mp3` reports 17 broken consecutive pairs out of 319 - i.e. real bugs, not shuffle-induced ambiguity, since this check never shuffles anything.

**Root cause**: `computeChunkMeta` (chunk_meta.cpp:206) only sets `meta.valid = true` when `bestCount >= 1`, i.e. when at least one *complete* frame is fully contained in the chunk. But `bestCount` excludes the case where a chunk's only content is the tail of a large frame plus the head of the next one, with zero full frames in between - which is routine once frame sizes approach a meaningful fraction of `CHUNK_SIZE` (1024), as VBR frames here do (up to 835 bytes). When this happens, `tryParse` still correctly computes `tailOverflow`/`tailPartialLen` for the chunk's one real candidate offset (which *is* recorded in `frameStarts`), but `computeChunkMeta` throws that away and resets the chunk to `valid=false, headOffset=0, tailOverflow=0, tailPartialLen=0` - and `canFollow` unconditionally rejects any edge touching an invalid chunk (`adjacency.cpp:25`). A chunk hit by this bug becomes a total island: nothing can precede it, nothing can follow it, in both the true order and the shuffled graph.

**Verified**: instrumented the real binary (temporary debug prints in `main.cpp`, reverted after) against `test7_shorter.mp3`. Confirmed for chunk 128: `frameStarts = [532]` (one legitimate candidate, matching the true frame boundary exactly), `tryParse` from 532 finds `tailOverflow=492, tailPartialLen=522, count=0` (the 522-byte frame doesn't fit in the remaining 492 bytes) - so it's marked `valid=0`, `headOffset=0`, `tailOverflow=0`, `tailPartialLen=0`, destroying real data that was already computed correctly. Across the whole file, exactly 9/320 chunks are affected (128, 131, 132, 182, 223, 235, 267, 276, 293), every one of them with a legitimate `frameStarts` candidate (none is a genuine "no header found" case). Each invalidated chunk breaks both its incoming and outgoing edge, and two of the nine are adjacent (131, 132, sharing one break) - `2*9 - 1 = 17`, exactly matching the observed break count. In a Python reimplementation of `computeChunkMeta`/`canFollow`, relaxing the validity gate to `bestOff >= 0` (any decodable, profile-matching candidate, regardless of `bestCount`) drops breaks from 17 to ~0-2 (the residual is an artifact of the Python prototype not implementing Case 2, not a real remaining bug - needs confirming in C++ after the real fix).

### 7. VBR files - branching factor stays high even after fixing item 6 (needs a stronger discriminator, not a bug)

Fixing item 6 (in the Python prototype) removes essentially all *correctness* breaks but barely moves the *branching factor*: out-degree histogram for `test7_shorter.mp3` (320 chunks, full graph, order-independent) goes from `{0:15, 1:127, 2:120, 3:50, 4:8}` to `{0:2, 1:130, 2:125, 3:51, 4:12}` - still only ~130/320 chunks (41%) have exactly one valid successor, essentially unchanged. This is *not* the CBR-style periodic `headOffset` aliasing from item 1 (frame lengths here vary, so that specific mechanism doesn't apply) - it's that Case 1's `hasFrameAt(b, rem)` accepts *any* decodable, profile-matching header at the required byte offset, and this stream contains many short, frequently-repeating frames (e.g. 104-byte quiet-passage frames occur dozens of times), so several *different* chunks legitimately have a valid header sitting at whatever `rem` a given predecessor demands, purely by chance.

**Tried and rejected**: strengthening `hasFrameAt` to require that `tryParse(b, rem)` yields at least one *additional* complete frame after the candidate (not just a decodable header) was tested in the Python prototype and made things *worse*, not better - true-order breaks went from 2 to 11 (`min_count=1`) and 130 (`min_count=2`), because plenty of genuine successors also start with a large frame that itself doesn't leave room for a second complete frame in the same chunk (i.e. the same phenomenon as item 6, one level further down the chain). Do not re-attempt this specific approach without a different underlying signal.

**2026-07-11 update - `part2_3_length` reservoir consumption implemented, doesn't move this number.** See the TODO "Implement real part2_3_length-based reservoir consumption" above - it's correct (validated against minimp3, never rejects any of 5 fixtures' true orders) but measured *no* net DFS speedup on `test7_shorter.mp3` (controlled A/B, pinned seed: 1500 vs 1777 nodes explored in the same 280s, i.e. slightly fewer, not more - the extra per-node arithmetic roughly cancels out whatever pruning it adds). The reason traces back to `canFollow`'s Case 1 still being the thing that determines *structural* branching (the out-degree histogram this section's numbers describe) - the reservoir check only prunes *inside* the DFS, after a structurally-legal-but-wrong edge has already been taken, and it's currently conservative by design (item 3's split-frame fix deliberately doesn't validate/consume the split frame's own `mainDataBegin`/`part2_3_length`, only credits its bytes - see that TODO). Closing that gap for real - validating a split frame's own reservoir fields using a combined view of both chunks in a candidate transition - would need to move into `canFollow`/`buildAdjacency` itself (so it prunes the *structural* graph, not just the DFS search inside it) rather than staying a post-hoc DFS-only check. That's a bigger change (needs the *pair* of chunks being tested as successors, not just one chunk's own metadata) and hasn't been attempted.

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

- [x] **Fix `computeChunkMeta`'s validity gate for VBR (Known Limitations item 6).**
  Implemented in commit `c83ffda` (this checkbox and item 6's status line were just never updated to match at the time - see item 6). `chunk_meta.cpp:206` reads `if (bestOff >= 0)`. Verified live: `test7_short.mp3`/`test7_shorter.mp3` pass the Phase-1 pre-shuffle check with 0 breaks.
  **2026-07-18 correction**: the claim above that `test7.mp3` (the full, uncut track) *also* passes with 0 breaks does not reproduce - re-verified live on both the current code and an unmodified baseline build: `test7.mp3` reports 2 breaks (at 107->108 and 641->642), on baseline too, so this was never actually true, not a regression. See item 3's 2026-07-18 update for the confirming A/B. Another stale/unverified claim in the vein of the CBR "~0.17s" saga (Known Limitations item 1) - this doc has a recurring pattern of claims not getting re-verified after being written; treat any un-dated claim here with suspicion until re-checked.

- [x] **Implement real `part2_3_length`-based reservoir consumption (supersedes the "model consumption" half of Known Limitations item 3).**
  Implemented in `chunk_meta.cpp` (`computePart23Bits`, a bit-level MSB-first reader for the Layer III side-info block - `main_data_begin`(9/8 bits) + `private_bits`(mono/stereo-dependent) + `scfsi`(MPEG1 only) followed by 2×nch (MPEG1) or 1×nch (MPEG2/2.5) granule/channel blocks of 59/63 bits each, `part2_3_length` being the leading 12 bits of each) and wired into `main.cpp`'s `advanceReservoirFrame`/`advanceReservoirChunk`. Two real bugs were found and fixed during validation, both confirmed by cross-checking byte-for-byte against [minimp3](https://github.com/lieff/minimp3) (`L3_read_side_info`/`L3_restore_reservoir`/`L3_save_reservoir`) instrumented to trace `reserv`/`main_data_begin`/`part_23_sum` per frame on `test7_shorter.mp3`:
  1. **Wrong reservoir-update anchor.** The natural-looking formula `level_after = level_before + produced - part23` is *wrong* - it lets unused reservoir surplus accumulate indefinitely. The real mechanism (confirmed against minimp3) is `level_after = mainDataBegin*8 + producedBits - part23Bits`: a frame only ever reaches back exactly as far as its own `mainDataBegin` declares, and any surplus beyond that is discarded, not carried forward. Using `level_before` caused a systematic downward drift that rejected the true, correct order by chunk 7.
  2. **Split-frame accounting gap.** `ChunkMeta.frames` only holds frames fully contained in one chunk, but chunk_size=1024 rarely divides frame lengths (~100-800 bytes here) evenly, so a large fraction of real frames straddle a chunk boundary and contributed *no* produced bytes to *either* chunk's reservoir tally under the naive per-frame loop. Fixed by crediting `meta.headOffset` bytes (completion of the previous chunk's split frame - pure payload, no header/side-info overhead) at the start of `advanceReservoirChunk`, and `meta.tailOverflow - 4 - sideInfo` bytes (start of the next split frame's payload, when its header+side-info fully fits in this chunk) at the end. The split frame's own `mainDataBegin`/`part2_3_length` are deliberately *not* validated/consumed (would need a combined view of both chunks) - crediting only its bytes without modeling its consumption is conservative (can under-prune, cannot wrongly reject a valid chain).
  With both fixes, the reservoir check passes on the true, unshuffled order for all 5 available fixtures (`test7.mp3`, `test7_short.mp3`, `test7_shorter.mp3`, `sample-3s.mp3`, `CBR.mp3` - correctness is file/bitrate-mode independent, as it should be).
  **DFS impact measured, and it's a wash, not the hoped-for win**: controlled A/B (pinned RNG seed, identical shuffle, `-O2`, 280s budget) on `test7_shorter.mp3` - neither the old accumulate-only model nor this one terminates; the new one explores *fewer* total DFS nodes in the same wall-clock budget (1500 vs 1777 `Depth:` progress lines), i.e. the extra per-node arithmetic costs slightly more than the conservative pruning currently saves. The "highest-leverage fix for item 7" framing this TODO used to carry was optimistic - see item 7's update below for why, and what closing the gap for real (validating the split frame's own mainDataBegin/part2_3_length, not just crediting its bytes) would need.

- [x] **Validate/consume a Case-1 split frame's own `mainDataBegin`/`part2_3_length` at the DFS edge (closes Known Limitations item 3's caveat).**
  Implemented on branch `sideinfo-mainbits` (`PLAN_sideinfo_mainbits.md` Milestone 1) - see item 3's 2026-07-18 update for the full writeup, mechanism, and measurements. Short version: `ChunkMeta.tailBytes` + `decodeSplitFrameSideInfo` reconstruct the split frame's real side info from both chunks of a candidate DFS edge, and `advanceReservoirChunk` now validates/consumes it exactly instead of crediting raw bytes unconditionally. Measurably improves reconstruction accuracy on tractable fixtures (`sample-3s.mp3`: exact-position 73.6% vs 55.2%, tau +0.666 vs +0.443) at a real per-candidate cost (~22x more wall-clock per DFS call on `test7_shorter.mp3`, measured via fixed-call-count benchmark) - unlike the item-3-caveat/item-7 `part2_3_length` finding directly above (a pure wash), this one is a genuine quality-of-answer win, just not (yet) a proven wall-clock win on large/hard VBR files.

---

### Medium priority — algorithm improvements

- [ ] **Bitrate as a discriminator in `frameStarts`.**
  Extend `frameStarts` from `vector<int>` (positions) to `vector<pair<int,int>>` (position, bitrate). In VBR streams, consecutive chunks should have correlated bitrate sequences. In `canFollow` Case 1, additionally verify that the bitrate of the frame at `rem` in chunk b is consistent with the last frame of chunk a (e.g., within a plausible VBR range). Add `bitrate` to `StreamProfile` for CBR files to filter false-positive frame headers that happen to have a different bitrate index.
  **Caution (2026-07-09)**: a related-but-different strengthening of Case 1 - requiring a decoded successor candidate to be followed by at least one more complete frame, as a proxy for "this is a real frame chain, not just a lone sync" - was tried against `test7_shorter.mp3` and made both correctness and branching *worse* (see Known Limitations item 7). A bitrate-continuity check is a distinct signal (doesn't require additional frames to fit in the chunk) and wasn't itself tested, but implement and measure against `test7_shorter.mp3`'s true-order breaks before trusting it, the same way item 6/7 were validated - don't assume it's safe by construction.

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

`input.mp3` defaults to the `FILENAME` macro in `main.cpp` if omitted (2026-07-18: this CLI override was previously undocumented-but-broken - `main()` ignored `argv` entirely despite this doc already showing the `[input.mp3]` usage; fixed alongside Milestone 1 so the two now actually agree).

Output is written to `output.mp3` in the current directory. Chunk 0 of the input file is always treated as pinned (first in the output).

An `RP_SEED` environment variable overrides the shuffle's RNG seed (normally drawn from the system clock) - added for the pinned-seed A/B methodology used throughout this doc (e.g. item 3's 2026-07-18 update); not needed for normal use.
