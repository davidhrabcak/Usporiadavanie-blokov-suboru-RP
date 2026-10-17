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
| `huffman_tables.cpp / .hpp` | Static Layer III Huffman code tables (big_values tables 0-31 + count1 quad tables A/B), sourced verbatim from [minimp3](https://github.com/lieff/minimp3) (CC0) rather than hand-transcribed - see the header for the (non-obvious, compact self-describing VLC) leaf encoding these tables use. Consumed by `main_data_decoder`. |
| `main_data_decoder.cpp / .hpp` | Milestone 2 partial Layer III main-data decode (MPEG1 only): MSB-first bit reader, big_values pair + count1 quad Huffman decode against `huffman_tables`, scalefactor-bit skipping, region0/1 boundary mapping, and a coarse 12-band per-granule occupancy/energy profile. No IMDCT / requantisation / synthesis. Window-switching (short/start/stop) blocks are out of scope. `decodeChunkFrame` does the cross-chunk main-data assembly the DFS needs. Validated bit-exact (`bitsConsumed == part2_3_length`) on ~46k long-block granules across all fixtures. |
| `pcm_junction.cpp / .hpp` | Full-decode junction scoring (branch `full-pcm-decode`, `PLAN_full_pcm_decode.md`). Decodes `[PP, P, cur, candidate]` chunks to PCM with the vendored minimp3 (fresh decoder per window, frame-exact feeding) and measures the spectral-flux spike at the junction vs the flux inside the already-placed audio. Window decode is sample-exact vs a full-file decode. `test/pcm_probe.cpp` is its measurement tool. |
| `edge_lock.cpp / .hpp` | Confident-edge locking: forces edges whose median-context PCM flux beats every competitor on both sides by `RP_LOCK_TAU`. Run iteratively with AC-3 before the supernode collapse. |
| `pcm_decoder_impl.cpp`, `third_party/minimp3.h` | Vendored [minimp3](https://github.com/lieff/minimp3) (CC0), compiled once, float output. Full Layer I/II/III decoder: requantise, stereo, IMDCT, synthesis. |
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

**2026-09-10 - long CBR is not combinatorially plausible to reconstruct *exactly*, and the reason is now nailed down.** The CBR adjacency graph is dense (~33 out-edges/chunk at n=658, and this *grows* with file length - roughly n/209 per demanded offset), forced chains are ~1/n, and AC-3 (item A4) removes ~1 edge. The only content-dependent per-edge signals - `global_gain` (AUC 0.68), Milestone-2 spectral (AUC 0.83 but true-successor rank-1 only 18%) - are far too weak to thread a graph that dense: a 2-min / ~1875-chunk CBR file has ~90+ successors/chunk, so even perfect-first ranking at 18% rank-1 dies within ~1 node of a greedy/beam descent. **The decisive check we hoped would work - decode the straddling frame's Huffman with the candidate successor's bytes and require a bit-exact `part2_3_length` - does not discriminate** (`sideinfo_probe` Measurement E: wrong successors pass 93-100%, nonzero-coefficient ratio 1.00). The partial decoder consumes exactly `part2_3_length` *by construction* (fixed `big_values` count, count1 bounded by the bit budget, complete tables → no invalid-leaf errors); only the coefficient *values* differ, which is just the (already weak) spectral profile. See `PLAN_huffman_junction_filter.md` for the full write-up. Additionally, runs of digitally-identical (silent) frames are ambiguous *in principle* - any order is equally valid (perceptually harmless; detect via `part2_3_length == 0` and collapse). **Bottom line**: small/medium CBR works; large CBR can only get a *best-effort* valid Hamiltonian path (bounded beam search), not the exact original. Reaching exact would need a genuinely stronger signal - requantised spectral + audio-continuity scoring, or global min-cost matching - both uncertain.

**Older update (stale wording kept for context)**: largely resolved for *tractability* (see TODO "Fix computeChunkMeta scoring" below) - `CBR.mp3` (658 chunks) now reconstructs in ~0.17s instead of never terminating. The composite `(tailQuality, frameCount)` scoring in `computeChunkMeta` was the actual fix: it turned out most of the ~32-way branching wasn't inherent aliasing, it was `computeChunkMeta` itself picking a `headOffset` with a worse (or absent) tail purely because it had a higher raw frame count, which fed bad `tailOverflow`/`tailPartialLen` into every downstream adjacency check. Once headOffset selection prefers a clean tail first, the branching factor collapses enough for the existing DFS + supernode collapse to finish quickly.

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

**2026-09-09 - does not reproduce on any fixture; the remaining test7.mp3 breaks are a different bug.** `sideinfo_probe` now counts the item-5 signature (`tailOverflow > 3 && tailPartialLen == -1`) and dumps every true-order break with a hex/`ID3`/`APE` scan. Across all 5 fixtures: **0 orphaned tails, 0 invalid chunks, no `ID3`/`APE` magic mid-stream anywhere.** The `CBR.mp3` orphaned-tail claim below is stale (same pattern as the "~0.17s" saga - treat un-dated claims here with suspicion).

`test7.mp3`'s 2 pre-shuffle breaks (107→108, 641→642) are **not** embedded non-audio. Both are `computeChunkMeta` latching a **false-sync spilling header**:
- 107→108: it picks a 112 kbps / len-365 "tail" at chunk offset 1013, which sits *inside* the real complete frame `[605, 1022)`. The real alignment is: frame @605, then a 2-byte Case-2 split header at 1022 (`ff fb` + `80 64` from chunk 108 → 112 kbps / 365 → successor frame at reconstructed offset 363, which chunk 108 **does** have). But `tailQuality` ranks the spurious `tailPartialLen>0` tail (quality 2) above the real Case-2 tail (quality 1).
- 641→642: it picks a spurious 256 kbps / len-835 frame at offset 677 over the real 112 kbps / len-365 at offset 722 (both spill, both quality 2, so the earlier offset wins the tie).

**Tried and reverted**: making "has ≥1 complete frame" the top scoring key (above `tailQuality`) fixes these 2 but creates **6** different breaks elsewhere in `test7.mp3` - a genuine large-frame-spanning chunk (item 6 shape) then loses to a spurious `count≥1` candidate. Net worse. A safe fix needs a cross-candidate check: *reject candidate offset X when X lies strictly inside `[Y, Y+len(Y))` for another candidate Y whose frame is complete in this chunk.* Not attempted (its own aliasing risk: a wrong early alignment can produce a "complete frame" that spuriously covers a real later start).

**Original (stale) symptom**: some chunks in `CBR.mp3` have `tailOverflow > 3` and `tailPartialLen = −1` (orphaned tail bytes); `canFollow` correctly returns false for these, but they represent genuine consecutive pairs. **Likely cause was hypothesised as** non-audio blocks (embedded ID3v2 chapter tags / PRIV frames) mid-stream - unconfirmed and now not reproducible.

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

**2026-09-03 update - `global_gain` continuity as a DFS candidate-ordering heuristic. This is the discriminator this section has been asking for.** Branch `sideinfo-mainbits`, `PLAN_sideinfo_mainbits.md` Milestone 1.5. The full Layer III per-granule side info is now parsed (`parseFrameSideInfo`/`GranuleSideInfo` in `chunk_meta.cpp`; the M1 reservoir path is now a thin wrapper over it, bit-identical, regression-checked on all 5 fixtures). A standalone probe (`test/sideinfo_probe.cpp`, built by CMake) measured every side-info field's discriminating power on the true unshuffled order of all 5 fixtures:
- **`global_gain` (granule quantiser/loudness) is a real, consistent signal.** Mann-Whitney AUC of `|Δ global_gain|` across a chunk junction, true-consecutive vs random pairs: sample-3s 0.58, CBR 0.68, test7_shorter 0.77, test7_short 0.73, test7 0.69.
- `big_values` and `part2_3_length` are weak secondaries (AUC ~0.56-0.65). **Everything else is noise** (`block_type`, `window_switching`, `table_select`, `scalefac_compress`, region counts: AUC 0.48-0.57). A z-scored combo of the top 3 is *worse* than `global_gain` alone (dilution) - keep it single-feature.
- **Ranking power (the DFS-relevant metric)**: sort a chunk's structural candidates `adj[i]` by `|Δ global_gain|`, measure the rank of the true successor. `test7_shorter`: mean rank 1.31 vs 1.69 random baseline, true successor ranked #1 in 74% of branch points (60% among the degree>=3 points that actually cause backtracking). `test7_short`: 64% / 55%. `test7`: 27% / 27% (~1.5x baseline, degree ~5-7). `CBR`: rank1 9% but mean rank 10.4 vs 17.0 (degree ~33 - aliasing-dominated, item 1).

Wired into `dfs()` (`main.cpp`): a per-chunk `BoundaryGain` (leading/trailing granule `global_gain`), and each `dfs()` call `stable_sort`s a copy of `superAdj[cur]` by junction `|Δ global_gain|` (unknown sorts last). **Pure exploration ordering - never rejects an edge, reservoir check untouched.** `RP_RANK=0` disables it for A/B.

**Measured impact - `sample-3s.mp3` (completes fast, 5 seeds, RP_RANK 0 vs 1)**: dramatic and clean. Rank OFF: exact-position 13-63%, Kendall tau +0.03..+0.48. Rank ON: exact-position 76-100%, tau +0.69..+1.00, **4 of the 5 seeds a perfect reconstruction** (tau exactly +1.0000, every chunk in its true slot). Same wall-clock (<1s both). This is the first change in the project's history to reliably recover the *exact* original order rather than just *a* structurally-valid one. Contrast with item 1's CBR result (finds *a* Hamiltonian path) and M1's "wash" on hard VBR.

**Hard-fixture completion (`test7_shorter`)**: ranking does **not** make it terminate - `RP_SEED=1 RP_RANK=1` still doesn't complete the exhaustive DFS within 95s (neither does the baseline). Expected: the structural out-degree histogram is unchanged (this is a DFS *traversal-order* change, not a graph change), and this fixture's intractability was already documented here. So `global_gain` ranking improves *which* answer the DFS lands on when it can complete (small/medium files) but does not, alone, make the largest VBR files tractable - same axis distinction as M1's item-3 finding. The remaining levers for tractability proper are Milestone 2's spectral continuity, or switching off exhaustive DFS (beam/A*, "Alternative approaches"). The evidence for `global_gain`'s value is Measurement B (candidate-ranking power on the true order) + the `sample-3s` exact-reconstruction result, not a hard-fixture wall-clock win.

**2026-09-07 update - beam-limited search + iterative broadening tried; does not crack the hard fixtures either. Confirms the blowup is genuine exponential search, not a fixed structural wall.** `main.cpp` now has: DFS instrumentation (`nodes`/`elapsed` progress, `RP_MAX_NODES` cap, final summary), a beam width `g_beamK` (recurse into at most K reservoir-valid candidates per node, in `global_gain`-rank order; `RP_BEAM=K`), and - automatically for graphs with >120 supernodes - iterative broadening over K ∈ {1,2,3,4,6,9,unlimited} bounded by a wall-clock budget (`RP_TIME_BUDGET`, default 45s; `RP_BROADEN=0` opts out). Small/medium graphs that finish an exhaustive DFS are unchanged (`sample-3s` still recovers the exact order; CI sanity unaffected). Findings on the hard fixtures:
- **`beam=1` (pure greedy) dead-ends almost immediately** - 17 placements on `test7_shorter` (202 supernodes), 56 on `CBR` (658), 124 on `test7_short` (512). The `global_gain` heuristic (rank-1 only ~60-74% at branch points) cannot greedily thread ~120-330 branch points; the first time its top pick hits a structural/reservoir dead end, beam-1 has no fallback.
- **Wider beams (2 → unlimited) all plateau at the same depth band** and the plateau depth is **seed-dependent**: `test7_shorter` ~100-130/202 (seeds 1/2/3 → ~111/128/102), `test7_short` ~260-330/512. Seed-dependence = the DFS is wandering an exponential number of ~half-complete orderings, not hitting one specific unreachable supernode. No bug, no graph gap (the true order passes `canFollow` + the reservoir check end to end).
- **Net**: `rp` now always terminates on large inputs with a bounded best-effort search (previously it hung forever), and honestly reports "no complete ordering found" when the budget is spent. **No accuracy win on the hard fixtures** - beam search needs a stronger per-edge signal to be useful here, same conclusion as the ranking heuristic itself.

**Decision this leaves open** (PLAN M1.5 step 7): near-exhaustive DFS over the current supernode graph is capped for large VBR *regardless of candidate ordering or beam width*. The two remaining directions, both non-trivial: (a) Milestone 2's partial spectral decode for a much stronger junction signal; (b) redesign the search as true beam/A* over *partial* orderings that emits the best partial path as `output.mp3` (the current beam-limited DFS still requires a *complete* 202/512/658-supernode ordering or it outputs nothing). A cheaper orthogonal experiment first: constraint propagation (AC-3) / stronger forced-chain collapse to shrink the ~120-330 branch points before any search. See the recommended order at the top of the TODO section.

**2026-09-10 update - Milestone 2 (partial spectral decode) landed and it is the first fixture-level accuracy win from the ranking work.** `main_data_decoder.{hpp,cpp}`: an MPEG1 Layer III big_values + count1 Huffman decoder over `huffman_tables`, scalefactor-bit skipping, region0/1 mapping, and a 12-band per-granule occupancy/energy profile - no IMDCT/requantise/synthesis. Validated **bit-exact** (`bitsConsumed == part2_3_length`) on ~46k long-block granules across all 5 fixtures + ci_sanity (silent granules, `part2_3_length == 0`, handled as a valid zero spectrum); ~8% of blocks are window-switching and skipped. `sideinfo_probe` Measurement D: the 12-band-energy L2 distance at a junction has AUC 0.77-0.83 on sample-3s/CBR/ci_sanity (beating `global_gain`) but only 0.60-0.65 on the test7 family (one dense pop song). `z(global_gain) + 0.5*z(spectralL2)` is a strong net win in the probe.

**Wired into `dfs()`**: candidate sort key gains `g_specScale * L2(trailingGranuleSpectrum(cur), leadingGranuleSpectrum(candidate))`, using `decodeChunkFrame` for the cross-chunk main-data assembly (chunk-local-only assembly is a bust - 1-8% coverage). Memoised by (predecessor, chunk), so total decodes ~= edge count and per-edge cost is ~10% wall-clock on `test7_shorter`. Auto-calibrated weight (median random-junction L2 -> ~3 contribution). **`sample-3s` now reconstructs byte-exact on all 7 seeds tried** (was 100/76.5/100/76.5/100/76.5/76.5 with `global_gain`+bv alone; A2 didn't move it). `ci_sanity` still 100%. The hard VBR fixtures still don't complete (spectral ~neutral there, per the probe) but calibrate and terminate cleanly. Caveat: the raw-L2 + raw-`junctionCost` mix has a sharp scale cliff (works ~8-15, catastrophic past ~18); auto-calibration keeps the shipped scale inside it, but a proper impl would z-score both terms.

**2026-10-08 update - full PCM decode at the junction (branch `full-pcm-decode`, `PLAN_full_pcm_decode.md`). The strongest per-edge signal so far, but the hard fixtures still don't complete.** Each candidate edge is decoded all the way to PCM with vendored minimp3 over `[PP, P, cur, cand]`, including the straddling frame and the reservoir-reaching frames after it. The windowed decode is sample-exact vs a full-file decode on every fixture. Score: the peak spectral flux (1024-pt FFT, hop 576) across the junction divided by the median flux in the pre-junction audio. `pcm_probe`, like-for-like (reservoir-decodable candidates, flux and M2 ranked on identical sets): rank-1 sample-3s 87.5% vs M2 37.5%, CBR 67.6% vs 16.5%, test7_shorter 95.8% vs 51.4%, test7_short 89.4% vs 50.9%, test7 88.9% vs 32.0%. Flux AUC 0.77-0.94; M2 AUC 0.45-0.60 on these sets. RMS/peak/HF-burst metrics are noise; not a safe hard filter (long true-junction tail). Wired into the candidate ranking (`RP_PCM`, `RP_PCM_W`, calibrated z-style scale). sample-3s is still exact on seeds 1-7 with 3.5x fewer DFS nodes (1.25M vs 4.39M). regression.sh / ci_sanity pass. Hard fixtures: still no complete ordering, but `rp` now reports the deepest partial path's accuracy. With PCM, greedy test7_shorter reaches 119/164 supernodes with 229/234 true junctions (98%; without PCM: 39/164, 91%), test7_short 180/446 with 97% (without: 88/446, 81%), CBR 30 s broadening 51% vs 32%. The first wrong splice still comes early (~26 chunks in), and chronological DFS backtracking can't repair it. **The bottleneck is now the search, not the signal**: global min-cost matching or a beam over partial orderings on the flux cost is the next step.

**2026-10-12 update - bitrate x chunk-size benchmark (`BENCHMARK_pcm_vs_main.md`/`.xlsx`).** 64 cells (CBR 32-320 kbps, VBR q2/q5, 10 s / 30 s / 6 min; 1-8 KB chunks) x 3 seeds: this branch is exact on 103/192 runs vs 65/192 for `sideinfo-mainbits` (+ the shared final-fragment fix), never complete-but-wrong, never worse on any cell. Held-out segment 33/56 vs 25/56. 6-min VBR and 6-min 32 kbps CBR are now exact at 8 KB. Long CBR at 1-2 KB and 6-min files below 8 KB remain search-limited.

**2026-10-09 update 2 - block-move repair + lock-threshold fallback: `test7_short` now reconstructs byte-exact on seeds 1-7 (first time).** `PLAN_full_pcm_decode.md` step 6. After the DFS, `repairOrdering` (`main.cpp`) does a best-improvement local search with block moves and block swaps. Moves must use full-graph structural edges (taken before AC-3/locking, so wrong locks can be undone), pass the reservoir check, and lower the lexicographic cost (unmeasurable junctions, summed log1p(flux)). `RP_LOCK_TAU` is now a schedule (default 1.5,1.25,1.1). Each non-final attempt is time-budgeted, and the next one retries from the unlocked graph. test7_short: seeds 3/5/6/7 finish at tau 1.25. Seeds 1/2/4 reach tau 1.1, whose A-X-Y-B ordering (606/609 true junctions) is fixed by one block swap. 60-100 s total, mostly failed-attempt budgets. sample-3s / test7_shorter / ci_sanity are unchanged and exact (repair makes 0 moves). CBR and test7 are unchanged. Caveats: the true order isn't the pure flux minimum (60 lower-flux moves on test7_short are reservoir-invalid), so flux + reservoir together pin it. The repair only runs if the DFS completes; at tau 1.0 test7_short* get too many wrong locks to complete. regression baseline: test7_short FAILED -> 100. Trade-off: the default can now reach tau 1.1, where the true-order probe saw wrong locks (sample-3s 1, test7 8). A file that fails at 1.5 may now get a complete-but-wrong ordering instead of an honest "no complete ordering". `RP_LOCK_TAU=1.5` restores the conservative behaviour. Every lock attempt is time-budgeted when a schedule is active, so `rp` still always terminates.

**2026-10-09 update - confident-edge locking from PCM flux: `test7_shorter` now reconstructs byte-exact (first time), on seeds 1-7.** `edge_lock.{hpp,cpp}` (`PLAN_full_pcm_decode.md` step 5): each edge a->b is decoded as `[p, a, b]` for every structural predecessor p of a (sample of 8 if more), and the median flux is taken. An edge is forced when it beats every other successor of a and every other predecessor of b by a factor >= `RP_LOCK_TAU` (1.5). Locking is iterated with AC-3 until a fixpoint, is gated like AC-3 (0 pre-shuffle breaks), and has a decode-cost cap that skips CBR/test7. At tau 1.5 the true-order probe shows 0 wrong locks on every completable fixture (test7 has 1). Every rp run's locks were correct against the source. Results: test7_shorter 164 -> 49 supernodes, exact in 55 DFS nodes. sample-3s exact in 1,792 nodes (was 1.25M). ci_sanity collapses to 1 supernode. test7_short 446 -> 230 supernodes, still no complete ordering (deepest path 594/598 true junctions). At tau 1.1 it completes, but on a valid ordering with one block moved (606/609 true junctions). The true order has the lower summed flux there, so a flux-minimising block-move repair is the next step. 2-opt test: the true pair has the lower summed flux in 97-100% of structural swaps, so flux, unlike `global_gain`, is a viable global objective.

**2026-09-09 update - A3 (bitrate), A4 (arc-consistency), A2 (multi-feature score) landed; none crack this, A4 is the one structural win.** A3: VBR junction bitrate continuity is measured noise (AUC 0.55), not wired; CBR-only bitrate header filter removes ~0.3% of edges. A4 (`propagateArcConsistency`): `test7_shorter` 202→164 supernodes (−19%), `test7_short` 512→446 (−13%), safely gated on the pre-shuffle diagnostic - still no complete ordering (164 is still exponential) but composes with a future per-edge signal. A2 (`sideinfo_probe` Measurement C): full logistic fit over 11 junction features overfits (secondary weights sign-flip across fixtures); shipped only the robust part - `junctionCost` is now `|Δglobal_gain| + |Δbig_values|/32`, which lifts probe rank-1 ~1-3pp on every fixture but doesn't move DFS accuracy on the observable ones (sample-3s seeds still 76.5%/100%). **Per-edge signal work is now exhausted for the near term - the large-VBR case needs Milestone 2's spectral decode, or a different search (beam/A*/Hungarian), not another metadata feature.**

**2026-09-07 update 2 - "best-of" search (keep exploring, minimise summed junction cost) tried; it is *worse*, and the reason is informative.** `main.cpp` `RP_BESTOF=1`: don't stop at the first complete ordering, keep the lowest-scoring one where score = summed `junctionCost` (= `|Δ global_gain|`, unknown junctions cost a flat 384) over every supernode boundary, with branch-and-bound on the partial score. Result on `sample-3s` (5 seeds): best-of converges to one score-194 ordering with **only 25% of chunks in place**, vs **76-100% for first-valid** (and 100% for seeds 1/3/777). The summed-junction-cost *global minimum is not the true order* - a real stream genuinely contains large `global_gain` steps at transients / section edges, so a "smoother than reality" reorder wins. **`global_gain` is a good local rank signal, not a good global objective.** first-valid + rank-ordered candidates stays the default; best-of stays behind the flag for trying a better objective (reservoir headroom, or a multi-feature score first *verified* to have the true order as its argmin - which needs a probe extension: "is `trueOrderScore < scoreOf(random valid orderings)` for objective X?").

---

## TODO / Future Improvements

### Recommended implementation order (2026-09-07)

Distilled from the item-7 findings. Rationale: `global_gain` ranking + first-valid
already recovers the *exact* order on tractable files (`sample-3s` 76-100%,
`ci_sanity` 100%); the open problem is (i) the ~25% of tractable-file cases where
first-valid takes a wrong turn, and (ii) large-VBR files where no complete ordering
is found at all. Cheapest, most-decisive levers first:

1. **[done, negative] A1 - best-of search.** Implemented (`RP_BESTOF`), measured worse
   than first-valid: summed-junction-cost is a misaligned objective (see item 7's
   2026-09-07 update 2). Kept behind the flag.
2. **[done] C1/C2/C3/C4 - consistency & verification** (deterministic seed,
   reconstruction-vs-source report, `measurements.sh`, `regression.sh`). See below.
3. **[done 2026-09-09] A3 - bitrate discriminator.** Mostly negative. VBR junction
   bitrate continuity measured too weak to wire (AUC 0.55). CBR bitrate filter
   implemented but removes only 0.3% of edges - not a lever for item 1/7. See the
   Medium-priority item below.
4. **[done 2026-09-09] AC-3 constraint propagation before the DFS.** Implemented,
   safely gated. Shrinks the graph 13-19% on the medium VBR fixtures but does not
   crack them alone. See the Alternative-approaches item below.
5. **[investigated 2026-09-09] A5 - embedded non-audio.** No repro on any fixture
   (0 orphaned tails everywhere). `test7`'s 2 breaks are a `computeChunkMeta`
   false-sync-tail bug instead (item 5, updated). Deprioritised - no test case.
   The real remaining `computeChunkMeta` fix is the cross-candidate "offset X is
   interior to complete frame Y" check (item 5), but it carries its own aliasing
   risk and isn't attempted. **CBR padding-bit cadence** (Medium-priority item)
   is still open and untried.
6. **[done 2026-09-09] A2 - fitted multi-feature junction score.** `sideinfo_probe`
   Measurement C. Full logistic fit overfits (secondary weights sign-flip across
   fixtures, only +1-6pp). Shipped the robust distillation: `junctionCost` is now
   `|Δglobal_gain| + |Δbig_values|/32` - improves probe rank-1 ~1-3pp on every
   fixture but does not move DFS reconstruction accuracy on the observable ones.
7. **[done 2026-09-10] Milestone 2 - partial spectral decode.** Landed. First
   fixture-level accuracy win: `sample-3s` now byte-exact on every seed. Decoder
   is bit-exact on ~46k granules. But spectral is ~neutral on the test7 family
   and the hard VBR fixtures still don't complete. See item 7's 2026-09-10 update.
8. **The large-VBR case now needs the *search* to change, not the signal.** All
   per-edge signals (global_gain, A2, A3, A4, reservoir model, M2 spectral) are
   in. `test7_shorter`/`test7_short`/`CBR` still find no complete ordering.
   Options: true beam/A* over *partial* orderings that emits a best partial path
   (`rp` currently needs a complete ordering or outputs nothing); Hungarian /
   min-cost perfect matching (sidesteps the exponential DFS entirely); or
   requantised spectral energy + short-block support to strengthen M2 on dense
   content.

### Consistency & verification (done 2026-09-07)

- [x] **C1 - deterministic shuffle seed.** `main.cpp`: default seed is fixed (1) so a
  given input always reconstructs identically; `RP_SEED=<n>` pins another,
  `RP_SEED=random` draws from the clock. Seed is printed.
- [x] **C2 - reconstruction-vs-source report.** `origIndexOf[]` is shuffled in
  lock-step with the chunks; after a reconstruction `rp` prints "X/N chunks in
  original position" and "EXACT (byte-identical to the source stream)" when fully
  correct. No more running `evaluate_order` by hand for a spot check.
- [x] **C3 - `test/measurements.sh`.** Regenerates the quantitative claims in this
  file (pre-shuffle / reservoir breaks, chunk & supernode counts, one-valid-successor
  ratio, `global_gain` AUC + rank-1 rate) in one command, so a stale claim is caught.
- [x] **C4 - `test/regression.sh` + `test/regression_baseline.tsv`.** Local accuracy
  regression check (fails if breaks increase or exact-position % drops >3 points).
  Not in CI - needs the fixtures.

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
  **2026-09-09: no repro.** `sideinfo_probe` finds 0 chunks with the orphaned-tail signature
  and no `ID3`/`APE` magic mid-stream on any of the 5 fixtures (see Known Limitations item 5).
  Deprioritised - nothing to build against. If a fixture with a mid-stream ID3v2 tag turns up:
  before computing `ChunkMeta`, scan each chunk for `ID3` magic, skip `10 + syncsafe(size)`
  bytes, resume frame parsing, and record the skip so `tailRemaining` accounts for it. The
  *reproducible* `computeChunkMeta` bug (`test7.mp3`'s 2 breaks) is the false-sync-tail
  selection described in item 5 - needs the cross-candidate "offset interior to a complete
  frame" check, not non-audio handling.

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

- [x] **Bitrate as a discriminator (A3, 2026-09-09).** Measured with `sideinfo_probe`, then
  the safe half implemented:
  - **VBR bitrate *continuity* across a junction is too weak to use.** AUC of `|Δ bitrate|`
    (true consecutive vs random pairs) is 0.55 on all three VBR fixtures; Measurement-B rank-1
    45-54% (vs ~40-50% random baseline). Same tier as `big_values` / `part2_3_length` -
    documented as noise. **Not wired into `canFollow`** - a hard reject on a 0.55 signal is
    exactly the "plausible idea that makes things worse" trap. The `frameStarts`
    `vector<int>` -> `vector<pair<int,int>>` extension is therefore *not* done.
  - **CBR bitrate *filtering* implemented** (`StreamProfile::isCBR`/`bitrateKbps`, set by
    `detectCBR()` when the first ~6 chunks show >=8 frames all at one bitrate and no Xing
    tag; `matches()` then rejects other-bitrate syncs; `RP_CBR=0` disables). Zero-risk (0
    true-order breaks on any fixture) but a tiny effect: on `CBR.mp3` it removes 66 of 21652
    adjacency edges (0.3%), no change to forced-chain or supernode counts. **Not a lever for
    item 1** - CBR aliasing is among *real* same-bitrate frames, which this can't touch.
    `sample-3s` is correctly left as VBR (it has a Xing tag / non-constant first frames).

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

- [x] **Constraint propagation (arc consistency) before DFS (A4, 2026-09-09).**
  `propagateArcConsistency()` in `adjacency.cpp`, run in `main()` between `buildAdjacency` and
  `buildSupernodes`. Forces an edge whenever a chunk's in- or out-neighbourhood is already a
  singleton (plus: nothing precedes chunk 0), deletes the competing edges, cascades to a
  fixpoint; self-reverts on contradiction.
  **Safety**: the inference is unsound when `canFollow` has a false negative (a real
  predecessor missing from the graph makes the surviving one look "unique"). Gated on the
  pre-shuffle diagnostic - runs only when it reported 0 breaks (`RP_AC3=1` forces, `RP_AC3=0`
  disables). `test7` (2 known `canFollow` false negatives, item 5) correctly skips;
  `sample-3s`/`ci_sanity` reconstruct byte-identically with it on and off.
  **Effect**: first thing to actually shrink the large-VBR search space - `test7_shorter`
  202→164 supernodes (−19%), `test7_short` 512→446 (−13%), `ci_sanity` 17→15. Not enough on
  its own (`test7_shorter` still finds no complete ordering; tractable-file accuracy
  unchanged - `sample-3s` seeds still 76.5%/100%), but it composes with a future stronger
  per-edge signal.

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

Output is written to `output.mp3` in the current directory. Chunk 0 of the input file is always treated as pinned (first in the output). `rp` also prints, after a successful reconstruction, how many chunks landed in their original position and whether the output is byte-identical to the source (C2).

**Environment variables** (all optional; defaults give deterministic, first-valid behaviour):

| var | default | effect |
|---|---|---|
| `RP_SEED` | `1` (fixed) | shuffle RNG seed. `RP_SEED=random` draws from the clock. Before 2026-09-07 the default *was* the clock; it is now fixed so runs are reproducible (C1). |
| `RP_RANK` | on | `RP_RANK=0` disables the `global_gain` continuity ranking of DFS candidates. |
| `RP_CBR` | on | `RP_CBR=0` disables the constant-bitrate frame-header filter (A3a) even when the stream is detected as CBR. |
| `RP_AC3` | auto | arc-consistency prune (A4). Auto = on when the pre-shuffle check found 0 breaks. `RP_AC3=1` forces on, `=0` off. |
| `RP_SPEC` | on | Milestone 2 spectral-continuity term in the candidate ranking. `RP_SPEC=0` disables. |
| `RP_SPEC_SCALE` | auto | overrides the per-file auto-calibrated spectral weight (calibration targets a gentle ~3 per median junction; safe window is roughly 8-15). |
| `RP_PCM` | on | full-PCM-decode junction flux term in the candidate ranking (`pcm_junction`). `RP_PCM=0` disables. |
| `RP_PCM_W` | 2 | weight of the PCM term, relative to the spread of junctionCost + spectral (per-file calibration). Insensitive in 0.5-100. |
| `RP_LOCK` | auto | confident-edge locking from PCM flux (`edge_lock`). Auto = on for Layer III when the pre-shuffle check found 0 breaks and the cost cap allows. `=0` off, `=1` forces past the gate and cap. |
| `RP_LOCK_TAU` | `1.5,1.25,1.1` | lock-margin schedule (comma list): if no complete ordering at one tau, retry locking + search at the next. Non-final attempts get a search budget (RP_TIME_BUDGET, else 45 s broadening / 30 s). A single value = one attempt. Lower = more locks + more risk. |
| `RP_REPAIR` | on | block-move/block-swap repair of the found ordering on summed PCM flux (needs RP_PCM). `=0` off. |
| `RP_REPAIR_PASSES` | 50 | max repair moves applied. |
| `RP_LOCK_ROUNDS` | 8 | max lock/AC-3 iterations on the pruned graph. |
| `RP_LOCK_MAX_DECODES` | 640000 | skip locking when one round would need more window decodes than this (fixed, so the result doesn't depend on the core count). Window fluxes are memoised across rounds and tau attempts. |
| `RP_THREADS` | all cores | threads for edge-lock scoring (results identical to 1 thread). |
| `RP_REPAIR_SUSPECTS` | max(16, n/20) | repair only evaluates moves that cut one of the K worst-flux junctions. `0` = no filter (slow on dense CBR graphs). |
| `RP_REPAIR_BUDGET` | 60 | repair wall-clock budget (s). When spent, the best move found so far is still applied. |
| `RP_CHUNK_SIZE` | 1024 | chunk size in bytes (rp and pcm_probe). The PCM junction window is byte-capped (>= 3 KB / 8 frames before, >= 1 KB / 4 frames after), so its cost doesn't grow with it; frames near the junction stay sample-exact at 4-8 KB (pcm_probe gate 1). |
| `RP_MATCH` | 1 | global min-cost successor matching (`globalMatch`: assignment on log1p(median flux) + cycle splicing + breaks-first repair). `1` = after a lock attempt's DFS fails, `2` = before the DFS, `0` = off. Its result is labelled `Result provenance: best-effort (global matching + repair)` - reservoir-valid but not proven original (a wrong order can be cheaper than the true one). |
| `RP_MATCH_MAX_DECODES` | 100000 | matching decode cap: predecessor contexts per single-chunk supernode are halved (8→1) until the not-yet-memoised windows fit; skipped if even 1 context doesn't. ~1.8 ms/decode/thread at 32 kbps. |
| `RP_MATCH_ITERS` | 0 | iterated matching: up to N re-solves, each forbidding the supernode arc at (or just before) every reservoir break of the previous assignment; the result with the fewest breaks (then lowest cost) is kept. |
| `RP_MATCH_REPAIR` | `valid` | matching repair policy. `valid`: a reservoir-valid matching is taken as is; an invalid one is repaired only until it is valid. `full`: always repair to a flux local optimum (pre-2026-10-18 behaviour). |
| `RP_RESV_FILTER` | auto | reservoir arc pre-filter: drop arcs that fail the reservoir check even from a full reservoir (sound: the check is monotone in the entry level). Auto = on when the reservoir model passed the true order (it feeds AC-3). `=0` off, `=1` forces. |
| `RP_SCORE_ORDER` | off | diagnostic: when the found ordering isn't exact, print its summed PCM flux vs the source order's. |
| `RP_BESTOF` | off | `RP_BESTOF=1` keeps searching past the first complete ordering for the min summed-junction-cost one. Measured *worse* - see Known Limitations item 7. |
| `RP_BEAM` | unset | `RP_BEAM=K` caps each DFS node to its K best-ranked reservoir-valid candidates (single fixed-width attempt). |
| `RP_BROADEN` | auto (>120 supernodes) | `=1`/`=0` force/disable iterative broadening over K ∈ {1,2,3,4,6,9,unlimited}. |
| `RP_TIME_BUDGET` | 45s broaden / 30s best-of / none otherwise | wall-clock stop for the whole search (seconds). |
| `RP_MAX_NODES` | 0 (unlimited) | hard cap on supernode placements tried. |
| `RP_PROGRESS` | ~10M | print a `[nodes=… depth=… elapsed=…]` line every N placements. |

**Verification scripts** (in `sound/`, run from there):
- `test/ci_sanity.sh build` - end-to-end pipeline check on the committed synthetic fixture (also run by CI).
- `test/measurements.sh build` - regenerate this doc's quantitative claims (C3).
- `test/regression.sh [--update] build` - local accuracy regression vs `test/regression_baseline.tsv` (C4; needs the fixtures).
