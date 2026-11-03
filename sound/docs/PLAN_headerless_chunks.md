# PLAN - frames larger than the chunk ("headerless" chunks) (branch `global-matching`)

## Problem

A frame longer than the chunk size can contain a whole chunk. That chunk has no frame header
inside, so `computeChunkMeta` marks it invalid, `canFollow` can't connect it to anything, and
the true order is not representable (CLAUDE.md Known Limitations item 9).

Which streams can have such chunks (largest frame, padding included, vs 1024 B):

| format | largest frame |
|---|---|
| MPEG1 L3, 320 kbps / 44.1 kHz | 1045 B |
| MPEG1 L3, 256-320 kbps / 32 kHz | up to 1441 B |
| MPEG1 L3 / 48 kHz | 961 B (never) |
| MPEG1 L2, any rate | up to 1729 B |
| MPEG2 L3 | at most 721 B (never) |
| MPEG2.5 L3, 8-11 kHz | 1045-1441 B |

`RP_CHUNK_SIZE` is at least 1024 and a Layer III frame at most 1441 B, so a frame can contain
at most **one** whole chunk: there are never two headerless chunks in a row.

Measured on `musiclib` (103 unique tracks, true order, 1 KB):

- 62 tracks have frames > 1 KB; 61 have headerless chunks, 5162 in total (~2% of a
  320 kbps / 44.1 kHz track, one every ~50 chunks).
- 21 of those 5162 contained a profile-matching false sync (before the protection-bit
  check, see item 9), which hides them from the "no header" test.
- At 1 KB only 8 of 103 tracks pass both true-order checks; at 4 KB 91 do (the other 12 are a
  trailing tag at the last junction).

## Key observation: the slot is decided by its predecessor alone

### Where F's header sits (measured with rp's chunking, 2026-10-26)

`F` starts at most `L - C` bytes (~21 at 1045) before the end of `a`. Across the 61 tracks with
headerless chunks (6459 slots):

| bytes of `F` in `a` | slots | |
|---|---|---|
| 1-3 (header split, Case 2) | 3795 (59%) | `tailRemaining(a) = -1` |
| >= 4 (whole header in `a`, Case 1) | 2664 | `tailRemaining(a) = rem` |

**35 of the 61 tracks have only Case-2 slots.** In CBR the frame grid drifts ~20.5 B per chunk
against the chunk grid, so every slot of a file lands at about the same offset. A design that
only handles Case 1 would mark no chunk at all on more than half of the affected files.

Let `a` be the chunk holding the start of a frame `F` that contains the whole next chunk `m`.

- **Case 1** (whole header in `a`): `a` demands `rem = tailRemaining(a) >= C` bytes of `F`. No
  chunk with a header can follow `a` directly (that would need a frame start at offset
  `rem >= C`), so **`a`'s successor is always a headerless chunk**, and the chunk after that,
  `b`, must have a frame start at `rem - C`.
- **Case 2** (`tov` = 1-3 header bytes in `a`): `F`'s length `L` comes from completing the
  header with the successor's first `4 - tov` bytes. `a` is a demander when every completion
  that matches the profile gives `L - tov >= C`, i.e. a frame start the successor can't have.
  For CBR (bitrate fixed) or `tov = 3` (bitrate byte in `a`) `L` is pinned up to the padding
  bit. The completions are taken from the pool chunks' first bytes (the slot chunk is one of
  them): `R(a) = { L(a, m) - tov - C : m in pool, header(a.tail ++ m.head) valid }`, and `b` must
  have a frame start in `R(a)`. If `a` can complete to both a short frame (ordinary Case-2
  successor) and a long one (slot), it is ambiguous; ambiguous chunks are counted and left as
  ordinary chunks in the first version (possible only in VBR with `tov <= 2`).

So:

1. **Which chunks are followed by a slot is known before any search**: exactly the chunks with
   `tailRemaining >= C` ("demanders"). Every ordering uses the same number of slots.
2. **Count check, free:** the number of demanders must equal the number of headerless chunks
   ("pool"). A mismatch means the model is incomplete for this file (most likely a false sync
   inside a headerless chunk) and is reported.
3. **The search doesn't need to know what goes into a slot.** It orders the chunks that have a
   header, with a demander's edge `a => b` standing for `a, (slot), b`. The pool is assigned to
   the slots afterwards.

## Design

### 1. Detection and gate (before the shuffle, shuffle-invariant)

- Compute `ChunkMeta` for every chunk.
- Pool `H` = chunks with no frame start at all (`frameStarts` empty, not the file's last short
  chunk / `finalFragment`).
- Demanders `D` = valid chunks with `tailRemaining >= C` (Case 1) or a Case-2 tail whose
  completions all give `L - tov >= C` (see above), each with its allowed offset set `R(a)`.
- Active only when both are non-empty. Otherwise nothing below runs and the output is
  byte-identical to before.
- Print `|H|`, `|D|`, and the count-check result.

### 2. Reduce the problem to the chunks that have a header

- `ChunkMeta::gapChunks` (0 or 1) and the offset set `R(a)`. Gap edges are added in a
  post-pass after `buildAdjacency`: `a => b` for every `b` with a frame start in `R(a)`
  (`byFrameStart` lookup), replacing `a`'s (empty) ordinary successor list. The pre-shuffle
  check uses the same rule (`canFollow` is only called there and inside `buildAdjacency`).
- The pipeline runs on the reduced chunk list (all chunks minus the pool; chunk 0 stays pinned
  first). The pool chunks are kept aside, shuffled like the rest.
- Diagnostics are computed on the reduced true order: `origIndexOf` becomes the rank in the
  reduced true sequence, so "true successor", lock correctness and exact-position reports
  keep their meaning.

### 3. What a gap edge does to each signal

- **Reservoir.** `F`'s side info lies partly in the unknown slot chunk, so it can't be decoded
  at the edge. Across a gap edge the level is set to full. Sound: the check is monotone in the
  entry level, so this can only accept more, never reject the true order. Pruning is weaker only
  at gap edges (~2% of edges).
- **PCM flux, spectral.** A window whose left side contains a demander spans a slot whose bytes
  are unknown, so the decode would be garbage. Such windows get the **neutral** value (the
  DFS's `g_pcmNeutral`; the matching already maps NaN to its median cost), not "unmeasured":
  the repair's `OrderCost` compares the unmeasured count first, and the number of windows that
  span a slot depends on the order (demanders placed close together share windows), so
  counting them as unmeasured would make the repair bunch slots together.
- **Edge locking.** A gap edge is never locked. Also **no edge into `b` may be locked when `b`
  has a gap-edge predecessor**: in median mode that predecessor's score is `{INF, INF}`, which
  never lowers the predecessor-side margin, so a wrong `x -> b` could lock past the invisible
  true gap edge, and the DFS can't undo a lock.
- **Global matching.** Unmeasured edges already get the median cost and stay in the graph.
- **Repair.** Moves can't create or remove slots (they belong to the demanders).

### 4. Fill the slots once the reduced order is complete

- Slots = positions right after each demander in the found order (excluding the last chunk).
- Cost of pool chunk `m` in slot `(a, b)`: `log1p(flux)` of the window with the path tail up to
  and including `a` on the left and **`m ++ b` (two chunks) on the right**, using
  `PcmJunction::LeftContext`. With two chunks on the right the junction is well measured even at
  320 kbps (AUC 0.90-0.97 vs 0.29 with one chunk; item 9).
- An unmeasurable or failing decode (invalid window, frames with no output after the junction)
  costs a large constant.
- Case-2 slots: `m`'s first `4 - tov` bytes must complete `a`'s header to a length `L` with
  `L - tov - C` equal to `b`'s frame offset (hard constraint, else excluded). In CBR almost every
  slot chunk satisfies it, so flux decides.
- CRC streams: if the CRC-covered bytes of `F` (header bytes 2-3 + side info) mix bytes of `a`
  and `m`, a wrong `m` fails the frame CRC with probability ~1 - 2^-16 (measured 1 / 14640).
  Mismatch -> excluded.
- Solve the `|slots| x |pool|` min-cost assignment exactly (Hungarian, O(n^3), n <= ~500).
  Decodes are `|slots| x |pool|` (~200 000 at 450 slots), run on `RP_THREADS` threads and capped
  by `RP_SLOT_MAX_DECODES`.
- Measured with the true `a`, `b` given (scratch probes `hdrless_probe` / `hung_probe`):
  joint assignment 100% on 2 of 3 tracks (per-slot rank-1 88-90%), 32.5% on Chainsaw (weak flux
  contrast); CRC-protected track 100%.

### 5. Output and reporting

- Insert the assigned pool chunks, write `output.mp3`.
- Re-run the reservoir check on the full order with exact split decodes (diagnostic).
- Accuracy over the full order (original indices), plus `slots: X/S filled with the true
  chunk`.
- Provenance gets `+ S slot(s) assigned by flux`. The slot assignment is a best-effort step:
  nothing proves it is the original.

## Not covered / risks

- **A false sync inside a headerless chunk** makes it a valid node that can't connect, so the
  reduced true order breaks and the count check fails by one. First recount after the
  protection-bit check; if it still matters, let the count check pick the `|D| - |H|` valid
  chunks that fit no position as extra pool members.
- **The right-side problem stays for ordinary edges.** At 320 kbps / 1 KB one candidate chunk
  holds less than one frame, so flux barely ranks ordinary successors (item 9). This plan makes
  the true order representable and fills the slots well. It does not by itself make 320 kbps /
  1 KB files reconstructable; that needs a two-chunk lookahead.
- **Gap edges are blind for flux.** About 2% of edges at 320 kbps / 44.1 kHz, plus the windows
  within `g_pcmLeft` chunks after each slot (~10% of junctions at 5 left chunks).

## Stage A results (2026-10-26)

### A third kind of chunk: split tails

Every split-header slot is really **two** headerless chunks in a row: a **split tail** (the end
of frame `F` and the first `t` = 1-3 bytes of the next header `G`), then the true interior chunk
inside `G`. The split tail's predecessor demands `rem = C - t`, which no ordinary chunk can
satisfy (frame starts are scanned only up to `C - 4`), so the split tail is fully determined by
its predecessor too.

Split tails become **valid nodes** (`promoteSplitTails`, `adjacency.cpp`): frame start `C - t`,
Case-2 tail of `t` bytes. The existing Case-2 gap logic then makes them demanders, and only
interior chunks go to the pool. The alternative - split tails in the pool, two-chunk slots -
was dropped: every such slot would need a split tail and an interior chunk chosen together,
a much harder assignment.

Classification rule (content only, same before and after the shuffle):

- `E_t > 0` (some chunk's tail leaves exactly `C - t` bytes);
- the last `t` bytes complete to a profile-matching header with a real header's rest;
- the first bytes do **not** complete a header for any `t'` in {1, 2} with `E_t' > 0` - that is
  what the interior chunk after a split tail looks like. `t' = 3` can't be tested: the chunk
  would supply only header byte 3, which is almost always valid.

Measured (musiclib, 1 KB, 101 tracks, 10 854 headerless chunks): 4 false positives, 16 false
negatives; split-tail count equals `E_t` on 87 files. Split tails are 3868 of the slots, almost all
`t = 1`; with the bitrate fixed (CBR) the head test is ~25x more selective.

### Two fixes this needed elsewhere

- **CBR detection at 1 KB.** `detectCBR` read 6 chunks, ~5.8 frames of 1045 B, below its 8-frame
  minimum, so the bitrate filter was off on exactly these files. It now reads enough chunks for
  10 maximum-size frames (15 chunks at 1 KB, 8 at 2 KB, 6 from 4 KB up as before).
- **False syncs win ties in `computeChunkMeta`.** At 320 kbps / 1 KB a chunk holds at most one
  real frame start; a false sync before it tied on (tail quality, frame count) and won as the
  earlier offset, giving the chunk a wrong tail. Third key, ties only: side info that is impossible
  for a real frame (`sideInfoPlausible`: big_values > 288, window switching with block_type 0,
  tables 4 / 14, part2_3_length over capacity) loses.

### True-order checks at 1 KB (101 tracks with headerless chunks)

| | both checks pass | count check OK |
|---|---|---|
| Case 1 only (first try) | 3 / 63 | 13 / 63 |
| + split tails, CBR detection | 40 / 101 | 77 / 101 |
| + side-info tie-breaker | **59 / 101** | 77 / 101 |

Remaining 42: 15 have an ID3v1 tag after the last frame (break at the final junction - not this
problem; the `strip` recipe avoids it, a fix would let the final chunk end in tag bytes);
24 fail the count check (FMA CBR off by 1-2 from the residual split-tail errors; Super Mario 64
VBR tracks off by more - they mix `t` values including `t = 3`); 3 other.

## Stage B results (2026-10-26)

`slot_fill.{hpp,cpp}`: per slot, the left side is the path tail up to `g_pcmLeft` chunks ending
at the demander (never across another slot); the right side is the candidate, `b` and the chunk
after `b` when known. Cost `log1p(flux)`; a window whose frames fail after the junction costs 30
(wrong chunks mostly do); silent windows get the median. Hard constraints: split-tail header
completion must land on a frame start of `b`; CRC must match. Exact rectangular min-cost
assignment (Hungarian), slots on `RP_THREADS` threads. `RP_SLOT_FILL=0` keeps pool order.

Oracle (`RP_SLOT_ORACLE=1`: true order of the chunks with a header, search skipped), whole tracks,
1 KB:

| track | slots right | output |
|---|---|---|
| Chainsaw (CBR 320, split-tail slots) | 80 / 80 | exact (was 32.5% in the scratch probe) |
| Ov Moi Omm (CRC) | 245 / 245 | exact - the CRC leaves one candidate per slot |
| Irama Gema - Grasp (CRC, whole-header slots) | 240 / 241 (one false demander, slot left empty) | exact |
| Circus Marcus | 204 / 209 | not exact |
| Super Mario 64 Title Theme (VBR, count mismatch 57 / 65) | 52 / 57 | not exact |

On Chainsaw 5485 of 6400 (slot, chunk) windows fail to decode cleanly - nearly all wrong
candidates - which is what makes it work where flux alone (32.5%) didn't.

## Stages

- **A.** Detection (Case 1 + Case 2), reduction, gap edges, reservoir set to full across gap
  edges, the count check, true-order diagnostics on the reduced order. First result: reduced
  pre-shuffle and reservoir checks pass at 1 KB on the 320 kbps / 44.1 kHz tracks.
- **B.** Slot assignment, with an oracle mode that forces the true reduced order.
- **C.** Guards: neutral PCM/spectral values for windows spanning a slot, the locking rule,
  the matching/repair checks.

## Validation

1. Inactive path: every existing fixture and every bench cell without frames > chunk is
   byte-identical (`regression.sh`, `ci_sanity.sh`, diff against the previous binary).
2. True order: reduced pre-shuffle and reservoir checks pass at 1 KB on `gen/cbr320_*` and on
   the 320 kbps / 44.1 kHz `musiclib` tracks; the count check holds.
3. Slot filling alone: with the true reduced order forced, fraction of slots filled correctly
   (diagnostic env switch).
4. End to end: `gen/cbr320_10s` at 1 KB (bench: FAILED, 6% of junctions recovered) and
   first-N-second clips of 320 kbps `musiclib` tracks at 1 KB, against the 4 KB result.
