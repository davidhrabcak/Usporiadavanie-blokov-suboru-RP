# MP3 chunk reconstruction - working notes

Research prototype for **file carving** (Bachelor's thesis): an MP3 file is cut into equal
chunks (`RP_CHUNK_SIZE`, default 4 KB) and shuffled; `rp` restores the order from the bytes
alone - no filename, no metadata, no external state. Model situation: the file type is known and
every block is present. Real fragmentation is milder (rarely more than 4 fragments), but the
goal is the best algorithm for the full shuffle, not one that is merely good enough for that.

**Scope rule (2026-11-15):** no code for cases that can't occur with real clusters. Frames
larger than the chunk (headerless chunks, < 1.44 KB) were supported and removed; a chunk must
hold a frame header. 1 KB remains usable as a stress test for streams with short frames.

- `README.md` - quick start and layout.
- `docs/HOW_IT_WORKS.md` - the algorithm stage by stage, results, references.
- This file - current state, options, tests, open problems and the measured findings behind
  the design. Older investigation narratives are in git history.

**Rule from experience:** every "should help" idea here has needed measuring against the true
order before it was trusted, and several made things worse. Undated claims in older notes were
wrong more than once (e.g. "`CBR.mp3` in 0.17 s" never reproduced). Date measurements and name
the binary/commit.

---

## Code

`src/` compiles into the `rpcore` library that `rp` (`src/main.cpp`) and `tools/` link.
Includes are relative to `src/` (`#include "mp3/chunk_meta.hpp"`).

| File | Role |
|---|---|
| `src/main.cpp` | The pipeline in five calls: `loadProblem` → `JunctionScorer` → `buildGraph` → `reconstruct` → `writeOutput`. |
| `src/config.*` | `Config::fromEnv`: every `RP_*` option, parsed once. `RP_REPORT` is read by `run_report.hpp`. |
| `src/run_report.hpp` | `RP_REPORT`: one JSON object per run (for `test/corpus.py`). |
| `src/pipeline/input.*` | `loadProblem`: chunks, CBR detection, shuffle, start chunk, profile, metadata, source-order diagnostics. |
| `src/pipeline/output.*` | `writeOutput`: `output.mp3`, provenance line, score vs source; `reportFailure`. |
| `src/mp3/Header.*` | Decodes a 32-bit frame header (length, bitrate, rate, version, layer, mode, CRC). |
| `src/mp3/frame_scanner.*` | Scans a whole file for frames; `chunksFromFirstFrame` cuts the audio (first frame to the trailing tags) into chunks. |
| `src/mp3/chunk_meta.*` | `computeChunkMeta` (per-chunk alignment and tail), `deriveProfile`, side-info parsing incl. split frames (`decodeSplitFrameSideInfo`). |
| `src/mp3/reservoir.*` | Bit-reservoir model: `advanceReservoirFrame` / `advanceReservoirChunk`, `reservoirValid` / `reservoirBreaks`; `frameSlack` / `junctionSlack` (exact main-data continuity). |
| `src/mp3/main_data_decoder.*`, `huffman_tables.*` | Partial Layer III decode (MPEG1, long blocks): Huffman → 12-band per-granule profile for the spectral ranking term. Bit-exact on ~46k granules. Tables from minimp3. |
| `src/graph/adjacency.*` | `canFollow`, `buildAdjacency` (indexed), `propagateArcConsistency` (AC-3). |
| `src/graph/chunk_graph.*` | The search graph in stages: structural graph, `filterReservoirArcs`, AC-3. `adjFull` (before AC-3) is the repair's move space. |
| `src/graph/supernodes.*` | Forced-chain collapse; `chunksOf`. |
| `src/scoring/junction_scorer.*` | The ranking terms (`gainCost` = \|Δglobal_gain\| + \|Δbig_values\|/32, `spectralCost`, `pcmCost`), memos, PCM window size, per-file calibration. |
| `src/scoring/pcm_junction.*`, `pcm_decoder_impl.cpp` | `LeftContext`: full decode of a chunk window with minimp3; spectral-flux spike at the junction. |
| `src/scoring/continuity.*` | Classes every arc tight / at cap / hole / unknown by `junctionSlack` (encoder cap and full-reservoir rule read from in-chunk frame pairs); a fixed cost per class on top of the flux. |
| `src/scoring/edge_lock.*` | Confident-edge locking on median-context flux. |
| `src/search/dfs.*` | Ranked, reservoir-pruned first-valid DFS over supernodes; beam, deadline, iterative broadening. |
| `src/search/global_match.*` | Min-cost successor assignment (SSP) + cycle splicing. |
| `src/search/repair.*` | Block-move / block-swap repair on (breaks, unmeasured, summed flux). |
| `src/search/reconstruct.*` | The lock-tau schedule around locking, collapse, DFS, matching and repair. |
| `src/search/chain_merge.*` | `RP_CHAIN=1` proof of concept for CBR (segment growing on flux x continuity). Not a default; see Findings, CBR. |
| `tools/` | `graph_check` (CI), `pcm_probe` (flux AUC / rank, lock precision), `sideinfo_probe` (side-info and partial-decode signals). |

### Data structures

- `StreamProfile`: `versionID`, `layerID`, `sampleRate`, `channelIdx` (ignored by `matches()`),
  `hasCrc` (protection bit; a mismatch is a false sync), `isCBR` / `bitrateKbps` (CBR filter).
- `ChunkMeta`: `headOffset`, `frameStarts` (sorted valid frame offsets), `frames` (complete frames),
  `tailOverflow` (bytes of the cut tail frame), `tailPartialLen` (its length, -1 if the header
  is cut), `tailBytes`, `chunkHead[4]`, `finalFragment` (the file's short last chunk).
- `tailRemaining(m)`: `tailPartialLen - tailOverflow` if known, 0 if no tail, -1 if the header
  is split (Case 2).

### Pipeline details not in HOW_IT_WORKS

- **Tags.** The leading ID3v2 tag and the trailing ID3v1 / Lyrics3v2 / APEv2 tags are found by
  signature and left out of the chunks (`audioEnd`). A trailing tag used to break the last
  junction (19 musiclib tracks at 4 KB, all SEARCH_FAIL).
- **Profile.** `voteProfile` (majority of `deriveProfile` over all chunks) is used for start
  finding and CBR detection, so neither depends on the true chunk 0.
  Then `deriveProfile` reads the first non-Xing frame of the found start; if the chunk holds only
  a Xing/Info/VBRI frame (a 320 kbps Info frame is 1045 B), that frame's header is used.
- **CBR detection** (`detectCBRUnordered`): >= 99% of the chunks' first headers at one bitrate
  (CBR files >= 99.75%, VBR <= 80.4% on musiclib), no Xing frame at a chunk start, no Info/VBRI
  frame at another bitrate. The CBR filter then rejects other-bitrate syncs (0.3% of edges).
- **Source-order diagnostics** (`checkSourceOrder`): `canFollow` and the reservoir model on the
  true order, printed and reported (`pre_breaks`, `resv_breaks`). Diagnostic only: until
  2026-11-15 they gated the reservoir filter, AC-3 and locking, which a real carver can't do.
  In the 2026-11-09 musiclib run at 4 KB all 20 gated-off files failed anyway.
- **Alignment** (`computeChunkMeta`): candidate offsets are ranked by tail quality (ends on a
  frame boundary > readable tail > 1-3-byte split header > orphaned), then frame count, then
  side-info plausibility (`sideInfoPlausible`, ties only). A Xing frame does not count as a frame.
  A chunk with a frame start but no complete frame is still valid.
- **Start finding** (`findStartCandidates`): header at offset 0 with `main_data_begin == 0`; a
  Xing/Info/VBRI frame breaks ties. On musiclib the true start is always a candidate and is
  decided on 205/215 files (4 KB); the rest log `AMBIGUOUS` and take the lowest index.
  `RP_START=pinned` keeps chunk 0 in place.

---

## Build, run, test

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/rp [input.mp3]          # default input: test/ci_sanity.mp3
```

Output: `output.mp3` in the current directory, plus the provenance line and (because the
source is known) chunks in place, true junctions and EXACT when byte-identical.

**Fixtures.** The MP3s in `test/` (`CBR.mp3`, `sample-3s.mp3`, `test7*.mp3`, `bohemoan-*`,
Queen VBR, `gen/`, `gen2/`) and `musiclib/` are third-party audio: never commit them (`.gitignore`
blocks `*.mp3`). Local source of the old fixtures: `/home/davidhrabcak/Desktop/python/sound/test/`.
Only the synthetic `test/ci_sanity.mp3` is committed. `test/make_bitrate_variants.sh` builds
`gen*/` (re-encoded clips at 32-320 kbps / VBR).

**Tests** (from `sound/`):
- `test/ci_sanity.sh [build]` - CI: `graph_check` (indexed graph == `canFollow` on every pair),
  then `rp` on `ci_sanity.mp3`, which must be EXACT; both at 1 and 4 KB.
- `test/corpus.py fixtures` (files in `test/`, `RP_TIME_BUDGET=20`) and
  `test/corpus.py musiclib` (whole files), both at 4 KB; suites in `test/corpus_suites.json`.
  `RP_CHUNK_SIZE=1024 test/corpus.py ...` runs another chunk size.
  Resumable; results in `musiclib/.derived[/corpus-test]/results/SUITE-BINARY.csv` + `.md`.
  `--jobs N` (default 8), `--match S...`; `RP_*` variables are passed through (own results file).
  No baseline: compare two results files by hand.
- `test/split_long_tracks.sh [-n] [-m MIN] [DIR]`: splits musiclib files longer than MIN (8) min
  into 2-5 min pieces; pieces after the first are re-encoded so each starts with an empty
  reservoir. Originals go to `musiclib/.long_originals/`.

**Metrics** (`corpus.py`, from the `order` in `rp`'s JSON report, so independent of the search):
`exact`, `neighbor_acc` (true neighbour pairs / (n-1)), `position_acc`, `kendall_tau`,
`longest_run_frac`, `start_correct`, `coverage`, `budget_exhausted`, `wall_s`. A failed search
is scored on its deepest partial path (denominators stay n).

**Classes:** EXACT / STRUCTURAL (complete, `neighbor_acc >= 0.98`: blocks right, placement
wrong) / WRONG / SEARCH_FAIL / TIMEOUT / ERROR / CRASH. The 0.98 cut is the only break in the
neighbor_acc distribution of complete-but-wrong runs (0.969 → 0.980); position_acc separates
nothing.

### Options

All optional; defaults are deterministic.

| var | default | effect |
|---|---|---|
| `RP_SEED` | 1 | shuffle seed; `random` = clock. |
| `RP_START` | found | `pinned`: chunk 0 stays first and is not searched for. |
| `RP_CHUNK_SIZE` | 4096 | chunk size in bytes, >= 1024 (rp and the tools). PCM windows are byte-capped, so cost doesn't grow with it. |
| `RP_THREADS` | all cores | scoring threads (results identical to 1 thread). |
| `RP_CBR` | on | `0` disables the CBR bitrate filter. |
| `RP_RESV_FILTER` | on | drop arcs that fail the reservoir check from a full reservoir. |
| `RP_AC3` | on | arc consistency (undone by itself on a contradiction). |
| `RP_RANK` | on | junction-cost ranking in the DFS. |
| `RP_SPEC` | on | partial-decode spectral term (weight calibrated per file). |
| `RP_PCM` | on | PCM flux term; off also disables matching and repair. |
| `RP_CONT` | on | main-data continuity cost (tight 0, at cap / unknown 0.5, hole 2) in the DFS ranking, repair and matching; the matching doesn't decode hole arcs. Layer III. |
| `RP_PCM_LEFT` | auto | left side of a PCM window in chunks: max(3, ceil(4 x p90 frame / chunk)). Lock/matching windows use one less. |
| `RP_LOCK` | on | edge locking (Layer III). |
| `RP_LOCK_TAU` | 1.5,1.25,1.1 | lock-margin schedule; each value is one attempt from the unlocked graph. |
| `RP_LOCK_MAX_DECODES` | 640000 | skip locking above this many decodes per round (a count, so results don't depend on cores); `0` = no cap. |
| `RP_MATCH` | on | global matching after a failed DFS. |
| `RP_MATCH_MAX_DECODES` | 100000 | matching decode cap; contexts are halved 8->1 until it fits, else skipped. |
| `RP_REPAIR` | on | block-move/swap repair on summed flux. |
| `RP_REPAIR_MIN_GAIN` | 1.0 | min summed-log1p-flux gain for a flux-only move on a valid order. |
| `RP_REPAIR_BUDGET` | 60 | repair seconds; the best move found so far is still applied. |
| `RP_TIME_BUDGET` | 45 broaden / 30-45 per lock attempt | DFS wall-clock limit (s). |
| `RP_CHAIN` | off | `1`: CBR proof of concept (`chain_merge`), replaces locking. `RP_CHAIN_DIAG=1` prints its statistics. |
| `RP_SCORE_ORDER` | off | print found vs true summed flux when not exact. |
| `RP_REPORT` | unset | path for the JSON run report. |

Removed 2026-11-15 (measured worse, no effect, or outside the scope rule): `RP_GAP`,
`RP_SLOT_FILL`, `RP_SLOT_ORACLE` (headerless chunks), `RP_BESTOF`, `RP_MATCH_ITERS`, `RP_MATCH=2`,
`RP_BEAM`, `RP_BROADEN`, `RP_MAX_NODES`, `RP_PROGRESS`, `RP_SPEC_SCALE`, `RP_PCM_W` (fixed at 2),
`RP_LOCK_ROUNDS` (8), `RP_REPAIR_PASSES` (50), `RP_REPAIR_SUSPECTS` (max(16, n/20)), and the
"force" value `1` of the former auto switches.

---

## Current results

**Whole-file musiclib run (2026-11-15, binary `9ee1d2804558` = commit `5017813`, 4 KB, 367 inputs):**
201 EXACT, 38 STRUCTURAL, 79 WRONG, 49 SEARCH_FAIL, no timeout / error / crash. All 201 exact
are VBR; CBR (117): 0 EXACT, 6 STRUCTURAL, 76 WRONG, 35 SEARCH_FAIL.

Before the cleanup (2026-11-09, binary `4c3f912dd056`, with headerless support):

| | EXACT | STRUCTURAL | WRONG | SEARCH_FAIL | median / max time |
|---|---|---|---|---|---|
| 4 KB | 192 | 42 | 76 | 57 | 2.6 s / 294 s |
| 1 KB | 22 | 13 | 6 | 326 | 60 s / 533 s |
| fixtures (1 KB) | 20 | 4 | 2 | 10 | |

No timeouts. One 1 KB run (`Siddhartha - God Is Love`, 7505 chunks) was OOM-killed: matching
built every window for every context count before finding none fit the cap. Fixed the same day
(count misses with early exit; 6.2 GB → 1.1 GB, matching 86 s → 0.16 s, all fixture orders
unchanged). The remaining ~0.9 GB is the graph (12.5M edges).

The 2026-10-18 development benchmark (64 + 56 held-out clips, 32-320 kbps CBR and VBR, 1-8 KB)
is in HOW_IT_WORKS ch. 6.

---

## Open problems

- **CBR (2026-11-15).** At 4 KB no CBR track of musiclib is exact (0/117; all 192 exact are
  VBR). ~67 structural successors per chunk on a 2500-chunk 320 kbps track; flux ranks the true
  one first for 52% only (perfect context too), and the decode caps switch locking and matching
  off. Flux-only pairing strategies fail (uncapped matching: 51% true junctions; margin joining:
  < 2% joined). Promising signal: exact main-data continuity (`junctionSlack`): wrong successors
  pass 0.2-3%, most true ones do (1869/1874 on a 256 kbps track, but 256 of 2499 true
  junctions on pURE and 83 of 1125 on Rock On Elm leave a hole; the rest are untestable). As a hard filter it fails (0-10% true
  arcs have a hole). Next step: use it as a cost in the matching and the DFS ranking.
  `RP_CHAIN=1` PoC (segment growing, flux / 8 on tight edges), paired A/B on 32 CBR tracks
  stratified by bitrate (2026-11-15, same binary and settings, arms run one after the other):
  default 0 EXACT / 1 STRUCTURAL / 25 WRONG / 6 FAIL, mean neighbor_acc 0.653 (median 0.701);
  chain 0 / 3 / 22 / 7, mean 0.726 (median 0.853); 15 better, 7 worse, 10 same (+-0.01).
  6 FAIL -> complete (to 0.89-0.99), but 7 WRONG -> FAIL: the joins are only 88-97% correct
  (locking ~99%), and on 320 kbps tracks with a full reservoir the wrong joins leave no
  perfect successor matching. Not a default. Next: continuity as a cost inside the matching
  (no forced joins), or joins with a fallback to the unjoined graph like the lock schedule.
- **`RP_CONT` (continuity as a cost, never a filter) - DEFAULT since 2026-11-15** (fixed
  costs; the per-file calibration was dropped as it changed nothing). A/B 2026-11-15, commit `9174916`, one
  binary, arms run one after the other, 4 KB:

  | | default | `RP_CONT=cal` | `RP_CONT=fixed` |
  |---|---|---|---|
  | 32 CBR tracks: EXACT/STRUCT/WRONG/FAIL | 0/1/25/6 | 0/3/28/1 | 0/3/28/1 |
  | 32 CBR tracks: mean (median) neighbor_acc | 0.653 (0.701) | 0.785 (0.866) | 0.785 (0.865) |
  | 20 VBR musiclib tracks: EXACT | 10 | 13 | 13 |
  | 38 fixtures: EXACT | 29 | 34 | 34 |

  cal vs default: CBR 17 better / 2 worse, VBR 4 / 0, fixtures 6 / 2 (both worse ones are
  CBR: the DFS now finds a complete order first, slightly wrong, where before it failed and
  the matching found the exact one). VBR gains: Queen VBR and 3 musiclib tracks STRUCTURAL/
  WRONG -> EXACT, 2 FAIL -> complete; no VBR loss. The matching runs on 5 more CBR tracks
  (hole arcs aren't decoded, so it fits under its cap). cal and fixed give identical
  outcomes (74/90 identical orders, no neighbor_acc change > 0.01): the per-file strength
  doesn't matter, the classes do.
- **Cleanup check (2026-11-15):** fixtures at 4 KB: all 38 orders identical before and after (29 EXACT, 4
  STRUCTURAL, 5 WRONG); at 1 KB the same class counts (20 EXACT), orders differ only on failing
  files with frames > 1 KB. The 19 musiclib tracks with a trailing tag (4 KB, 2026-11-15): 0
  source-order breaks on all, 19 SEARCH_FAIL -> 6 EXACT, 7 WRONG, 6 SEARCH_FAIL.

- **Wrong outputs.** The pipeline can return a complete, reservoir-valid, wrong order, and no
  label tells it apart (provenance and stability gap don't separate right from wrong). Two kinds:
  *search failures* (the true order is cheaper but more than one repair move away, or below the
  min-gain threshold) and *objective failures* (the wrong order has lower summed flux - Queen
  VBR 2/4 KB, bohemoan 2/4 KB, `CBR.mp3`). The second kind needs a better objective; edge probabilities with an exact
  most-probable-path solver (plan `cb53497`) were tried and abandoned.
- **1 KB chunks.** 326/367 SEARCH_FAIL (2026-11-09, with headerless support). At 256-320 kbps
  a frame doesn't fit in a chunk; that case is now out of scope (scope rule above).
- **Ambiguous start** on ~5% of musiclib (no Xing frame, many `main_data_begin == 0`). Possible
  tie-break: the candidate with no good-sounding structural predecessor.
- **False-sync alignment inside a real frame.** A cross-candidate check ("reject offset X
  inside complete frame Y") is the known fix, not attempted (own aliasing risk). The protection
  bit check removed the known cases (`test7.mp3`).
- **Unbounded DFS (fixed 2026-11-15):** with a single lock attempt and <= 120 supernodes the
  exhaustive DFS had no deadline (hit the 900 s test timeout on 4 PoC runs); now 45 s.
- **Silent runs** of identical frames are ambiguous in principle (harmless to the ear).
- Real fragmentation is milder (few multi-cluster fragments) but adds foreign and missing
  clusters, which the tests don't model.

### Ideas not tried

- CBR padding-bit cadence as a junction check.
- Iterated local search in repair (kicks + re-repair) for the search failures.
- Particle filter / learned edge classifier as alternatives to flux.
- Skipping mid-stream ID3v2 / APE blocks (no fixture has one).

---

## Findings

Measured results the design rests on. AUC = true vs wrong junction; rank-1 = how often the true
successor ranks first among structural candidates.

Old plan tags still used in code comments: A1 best-of search, A2 fitted junction score, A3 / A3a
bitrate signal / CBR filter, A4 AC-3, A5 non-audio check, C1 fixed seed, C2 score vs source,
Milestone 1 exact split-frame reservoir, Milestone 2 partial spectral decoder, Measurement A-E
and Stage A/B = sections of `sideinfo_probe` and of the headerless-chunk work.

### Structure and reservoir

- **CBR aliasing.** All frames have one length, so frame starts repeat with a short period:
  `CBR.mp3` (658 chunks) has ~33 successors/chunk, growing ~n/209 per demanded offset. VBR is
  better but short quiet frames (104 B) repeat dozens of times.
- **Tail-first alignment** fixed most false alignments; frame-count-first had fed wrong tails
  into every edge.
- **Rejected:** requiring an extra complete frame after the demanded offset (true-order breaks
  2 → 11 / 130); "has a complete frame" as the top alignment key (2 breaks fixed, 6 created);
  comparing `a`'s tail bytes with `b`'s head bytes (they are different parts of one frame, not
  copies - the same objection applies to byte-hash fingerprinting).
- **Reservoir update rule** (checked frame by frame against minimp3):
  `level_after = main_data_begin·8 + produced − part2_3_length`, not `level_before + ...` -
  surplus a frame doesn't point back to is discarded. The wrong form rejected the true order by
  chunk 7. `main_data_begin` is a pointer, not a consumption amount. MPEG2/2.5 use 8 bits for it.
- **Split frames** are checked exactly at a candidate edge by rebuilding their side info from
  both chunks (`sample-3s` in place 55% → 74%).
- **Monotone in entry level:** a step that fails from a full reservoir fails from any level -
  the basis of the reservoir arc filter (removes 40-85% of CBR arcs, 0 true arcs).
- **AC-3 end-of-stream bug:** forcing the last chunk's single arc deleted a true arc once graphs
  got sparse; the successor-side rule now waits until the end is known.
- **CRC streams:** a 16-bit CRC follows the header when the protection bit is 0; ignoring it
  shifted every side-info field by 2 bytes (true-order reservoir breaks on almost every chunk of
  the 17 protected musiclib tracks). The protection bit is constant per stream, so a mismatch is
  a false sync.
- **Clips must start at the track's start.** A mid-track stream copy starts with frames pointing
  into missing reservoir bytes. Use `ffmpeg -t N -c copy`, or re-encode.

### Junction signals

| signal | AUC | rank-1 | status |
|---|---|---|---|
| \|Δglobal_gain\| | 0.58-0.77 | 27-74% VBR, 9% CBR | ranking term |
| \|Δbig_values\|, part2_3_length | 0.56-0.65 | | big_values/32 in ranking |
| other side-info fields, VBR bitrate | 0.48-0.57 | | noise |
| partial-decode 12-band spectrum | 0.45-0.83 | 16-51% | ranking term |
| **PCM flux spike / median** | **0.77-0.94** | **68-96%** | ranking, locking, matching, repair |
| RMS, peak, HF burst | | | noise |

- **Logistic fit over 11 features** overfits (weights flip sign across fixtures); only the
  big_values/32 term was kept.
- **Huffman bit-count check on the straddling frame: no discrimination.** Wrong successors pass
  93-100%: the decoder consumes exactly `part2_3_length` by construction.
- **Flux is a ranking signal, not a filter:** some true junctions spike (a drum hit at the cut).
  It is a usable global objective (the true pair wins 97-100% of structural swaps), unlike
  `global_gain`, whose min-sum order put only 25% in place (`RP_BESTOF`).
- **Lock precision** (116 clips, round 1): mutual locks 98.8% correct at margin >= 1.1, 83% at
  1.05-1.1, 58% below.
- **High bitrates at 1 KB:** the left side needs ~4 frame lengths (fixed by `RP_PCM_LEFT`); the
  right side with one chunk gives AUC 0.64-0.85 at 256-320 kbps, 0.85-0.97 with a two-chunk
  lookahead. The lookahead was built and dropped (2026-10-27): only active below ~2.4 KB, and all
  320 kbps 1 KB clips still failed.

### Search

- **Exhaustive DFS** is exponential on large graphs; greedy dies within tens of placements,
  wider beams plateau at a seed-dependent depth. With PCM ranking the first wrong splice still
  came ~26 chunks in and chronological backtracking couldn't undo it: the bottleneck was the
  search, not the signal.
- **Global matching** gets 92-98% of junctions right in ~0.02 s with 0-3 cycles; matching
  first (`RP_MATCH=2`) was slightly worse than DFS first.
- **Min-gain 1.0:** flux-only moves below 0.25 gain were harmful 30:4 in the move study; the
  held-out set helped choose 1.0, so it isn't clean for this setting.
- **Repair cost order** breaks > unmeasured > flux: with unmeasured junctions neutral, repair
  "won" by making junctions unscorable.
- **Iterated matching** changed no outcome; off.
