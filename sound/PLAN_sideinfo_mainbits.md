# Plan: Decode side info + main data to cut MP3 chunk-reorder DFS branching

Branch: `sideinfo-mainbits`. Companion doc: `CLAUDE.md` (this plan closes gaps
named in that doc's items 3 and 7 — read those first for full background).
This file tracks execution progress; update checkboxes as steps land so the
work can be picked up in a later session without re-deriving context.

## Context

`sound/` reassembles a shuffled, 1024-byte-chunked MP3 file using structural
heuristics + an exhaustive DFS. Per `CLAUDE.md` item 7 (2026-07-11), the DFS
still has a wide branching factor on real VBR content because the only
signals used today are header/frame-alignment structure and an
*accumulate-only* reservoir check that runs post-hoc per-chunk, not per-edge.
Two decode-based signals are named in `CLAUDE.md` as the next levers, both
never attempted — this plan implements both, as two independently-measurable
milestones. Every "should help" idea in this codebase so far has needed
empirical validation against the true unshuffled order before being trusted
inside the DFS (two plausible ideas already turned out to make things worse —
see items 3 and 7); both milestones here must clear that same bar before being
kept.

Confirmed during planning: there is no existing Python prototyping workflow
for `chunk_meta`/`adjacency` (the `.so` binding only wraps `Mp3FrameScanner`) —
all new logic is implemented and validated directly in C++.
`sound/test/evaluate_order.cpp` (`evaluate_order <original.mp3> <output.mp3>`)
is the existing ground-truth comparator (Kendall's tau, exact-position %,
adjacent-run coverage) for correctness verification.

### Test fixtures (NOT in git — rights-restricted)

The MP3 fixtures (`CBR.mp3`, `sample-3s.mp3`, `test7.mp3`, `test7_short.mp3`,
`test7_shorter.mp3`, `test1..6*`) are third-party / rights-restricted and must
never be committed — `.gitignore` blocks `*.mp3` for this reason. They have
never been in git history.

Local source on this machine (2026-09-03): a pre-`7a3103f` copy of the project
at `/home/davidhrabcak/Desktop/python/sound/test/`. Copy the ones a given
measurement needs into `sound/test/` before running `rp`/`evaluate_order`
(`main.cpp`'s `FILENAME` default and the `test/…` paths are relative to
`sound/`). Chunk counts as a sanity check: `CBR.mp3` 658, `sample-3s.mp3` 51,
`test7_shorter.mp3` 320, `test7_short.mp3` 610, `test7.mp3` 3351.

Build note (2026-09-03): `cmake` is not installed on this machine. Direct
build that matches the doc's `-O2` A/B methodology:
`g++ -std=c++14 -O2 -o cmake-build-debug/rp frame_scanner.cpp Header.cpp chunk_meta.cpp adjacency.cpp huffman_tables.cpp main.cpp`
and `g++ -std=c++17 -O2 -o cmake-build-debug/evaluate_order test/evaluate_order.cpp frame_scanner.cpp Header.cpp`.

---

## Milestone 1 — Exact junction-frame reservoir consumption

**Gap**: `advanceReservoirChunk` (`main.cpp:132`) only *credits* a split
frame's byte count at a chunk boundary; it never validates/consumes that
frame's own `mainDataBegin`/`part2_3_length`. By construction in `tryParse`,
whenever `tailPartialLen > 0` (Case 1), the split frame's full 4-byte header
(and `tailOverflow >= 4`) is guaranteed inside the earlier chunk — so once a
DFS candidate successor `b` is chosen, the junction frame's side info can be
fully reconstructed from `a`'s tail bytes + `b`'s head bytes.

- [x] 1. `chunk_meta.hpp`/`.cpp`: add `std::vector<uint8_t> tailBytes` (full
      tail region, not just 3 bytes) to `ChunkMeta`, populated in
      `computeChunkMeta`.
- [x] 2. `chunk_meta.cpp`: refactor bit-reading core into a shared
      `SideInfoBitSource`/`readBitsMsb`/`computeSideInfoFields`, used by both
      the single-chunk path (`readFrameSideInfo`) and the new cross-chunk
      path; add `decodeSplitFrameSideInfo(a, bBytes) -> SideInfoResult`
      (bool-valid out-struct, matching the codebase's existing convention
      rather than `std::optional`, to avoid a C++17 bump).
- [x] 3. `main.cpp`: `advanceReservoirChunk` now takes `prevMeta` + the
      current chunk's raw bytes; for Case-1 split frames it decodes the real
      side info and runs it through `advanceReservoirFrame`, falling back to
      the old byte credit only when the exact decode isn't possible. Wired
      at all 3 call sites (pre-shuffle check, DFS priming, `dfs()` - the
      supernode-boundary case uses `supernodes[cur].chunks.back()` as the
      predecessor for a candidate's first chunk).
- [x] 4. Validate: pre-shuffle/reservoir check passes on 4/5 fixtures
      (`test7_short.mp3`, `test7_shorter.mp3`, `sample-3s.mp3`, `CBR.mp3`).
      `test7.mp3` (full track) shows 2 breaks, confirmed pre-existing via
      A/B against an unmodified baseline build (not a regression) - see
      `CLAUDE.md` item 3's 2026-07-18 update.
- [x] 5. Measured: pinned-seed A/B (`RP_SEED` env var added for this) +
      fixed-call-count benchmark on `test7_shorter.mp3`, and full-completion
      comparison on `sample-3s.mp3`. Result: genuine accuracy improvement
      (not a wash) at a real per-candidate cost - see `CLAUDE.md` item 3's
      2026-07-18 update for numbers.
- [x] 6. `CLAUDE.md` updated (item 3 + item 6 correction + TODO checkbox).

---

## Milestone 2 — Partial main-data decode for spectral continuity

Scoped to MPEG1 Layer III only (matches existing fixtures); MPEG2/2.5 LSF
scalefactor tables are out of scope — document as a known limitation.

- [x] 1. `huffman_tables.hpp`/`.cpp`: static big_values (32 `table_select`
      values) + count1 (quad, tables A/B) Huffman tables, extracted
      programmatically from minimp3's `minimp3.h` (CC0) and diffed
      numerically against the source to confirm zero transcription drift
      (2164 + 32 + 32 + 28 + 16 values, all identical). Data only - the
      leaf-decoding scheme is documented in the header but the actual
      bitstream-integrated decode (against this project's own bit reader,
      not minimp3's) is M2.4's job. Wired into `CMakeLists.txt`'s `rp`
      target. `CLAUDE.md`, root `README.md`, and this file updated to
      reflect the new file and Milestone-1-era usage changes (`argv[1]`
      input override, `RP_SEED` env var) that predated this step but hadn't
      been documented in the top-level `README.md` yet.
- [x] 2. `GranuleSideInfo` / `FrameSideInfo` parser. Landed in
      `chunk_meta.{hpp,cpp}` (not a new `main_data_decoder` file - it shares
      Milestone 1's `SideInfoBitSource`/`readBitsMsb` directly). `parseFrameSideInfo`
      now walks the full side-info block (all granule/channel fields:
      `bigValues`, `globalGain`, `scalefacCompress`, window/block-type flags,
      `tableSelect[3]`, `subblockGain[3]`, region0/1 counts, `preflag`,
      `scalefacScale`, `count1TableSelect`); `computeSideInfoFields` (the M1
      reservoir path) is now a thin wrapper over it, with a
      `bitPos = blockStart + blockBits` guard so the `part2_3_length` sum
      stays bit-identical to the pre-refactor skip loop. Public entry point
      `parseFrameSideInfoAt(buf, off, mpeg1, mono)`. Regression-checked: M1
      reservoir check still PASSES on the true order for all 5 fixtures
      (`test7.mp3`'s 2 pre-existing breaks unchanged).

---

## Milestone 1.5 — Side-info-only continuity heuristic (Step 1 probe + wiring)

Rationale for inserting this ahead of Milestone 2's full spectral decode: M1's
exact reservoir consumption turned out to be "a wash" on hard VBR (CLAUDE.md
item 7). Before building the expensive Huffman+requantize path, measure whether
the *free* side-info fields alone (now that step 2 parses them) discriminate a
chunk's true successor. They do - `global_gain` strongly.

**Probe** (`test/sideinfo_probe.cpp`, built by CMake): runs on each fixture's
true (unshuffled) order; for every chunk computes the trailing granule's side
info and, for candidates, the leading granule's. Two measurements: (A)
Mann-Whitney AUC of the junction |Δfeature| for true consecutive pairs vs
random pairs; (B) rank of the true successor among a chunk's structural
candidates `adj[i]` when sorted by |Δfeature|, vs the random-order baseline
mean rank.

- [x] 1. Measurement A - only `global_gain` is a consistent signal. AUC of
      `|Δ global_gain|`: sample-3s 0.58, CBR 0.68, test7_shorter 0.77,
      test7_short 0.73, test7 0.69. `bigValues` / `part2_3_length` are weak
      secondaries (~0.56-0.65). Everything else (`blockType`,
      `windowSwitching`, `tableSelect`, `scalefacCompress`, region counts) is
      noise (AUC 0.48-0.57).
- [x] 2. Measurement B (`global_gain` scorer, rank of true successor among
      `adj[i]`, degree>=2): test7_shorter meanRank 1.31 vs 1.69 base, rank1
      74% (deg>=3: 60%); test7_short 1.53 vs 1.99, rank1 64% (deg>=3: 55%);
      test7 3.48 vs 5.24, rank1 27% (deg>=3: 27%, ~1.5x baseline); CBR 10.4
      vs 17.0, rank1 9%; sample-3s 1.60 vs 1.82, rank1 52%. A naive z-scored
      combo of global_gain+bigValues+part2_3_length was *not* better than
      global_gain alone (dilutes it) - keep it single-feature.
- [x] 3. Wired into `dfs()` (`main.cpp`): `BoundaryGain` per chunk (leading /
      trailing granule `global_gain`), and each `dfs()` call `stable_sort`s a
      copy of `superAdj[cur]` by `|Δ global_gain|` at the junction (unknown =
      sorts last, stable). **Ranking only - never rejects an edge; reservoir
      check unchanged.** `RP_RANK=0` env var disables it for A/B.
- [x] 4. A/B on `sample-3s.mp3` (completes fast, 5 seeds, RP_RANK 0 vs 1):
      dramatic. rank OFF exact-position 13-63%, tau +0.03..+0.48; rank ON
      exact-position 76-100%, tau +0.69..+1.00, with **4 of 5 seeds a
      perfect reconstruction** (tau exactly +1.0000). Same wall-clock (<1s
      both).
- [x] 5. A/B on `test7_shorter` (intractable VBR): ranking does **not** make
      it complete - `RP_SEED=1 RP_RANK=1` still doesn't finish the exhaustive
      DFS in 95s, same as baseline. Expected (traversal-order change, not a
      graph change; fixture already documented intractable). So M1.5 is an
      *answer-quality* win, not a *tractability* win for the largest files -
      same axis split as M1. Evidence for keeping it = Measurement B +
      sample-3s exact reconstruction, not a hard-fixture wall-clock number.
      (Measuring completion cleanly is hampered by `rp` block-buffering its
      cout to a pipe/file and losing the tail on SIGTERM - a `cout.flush()`
      after the metadata table, or progress to stderr, would help future A/Bs.)
- [x] 6. `CLAUDE.md` updated - item 7 gets a dated 2026-09-03 entry; the
      hard-fixture paragraph reflects step 5's result.
- [x] 7. Beam-limited search + iterative broadening added (`main.cpp`:
      `g_beamK`, `RP_BEAM`, auto-broaden for >120 supernodes over
      K ∈ {1..9,unlimited} under a wall-clock budget `RP_TIME_BUDGET`,
      default 45s; plus DFS instrumentation `RP_MAX_NODES`/`RP_PROGRESS` and
      a nodes/elapsed summary). **Does not crack the hard fixtures.** beam=1
      dead-ends in 17-124 placements; wider beams all plateau at a
      seed-dependent depth (~100-130/202 on `test7_shorter`, ~260-330/512 on
      `test7_short`) - genuine exponential blowup, not a structural wall
      (the true order passes `canFollow` + reservoir end to end). Value
      delivered: `rp` now always terminates on large inputs with a bounded
      best-effort search instead of hanging; small/medium graphs unchanged.
      See `CLAUDE.md` item 7's 2026-09-07 update.
- [x] 8. A1 (best-of search) implemented and measured - **negative**. `RP_BESTOF=1`:
      keep exploring past the first complete ordering, keep the min summed-junctionCost
      one, branch-and-bound on partial score. On `sample-3s` it converges to a single
      score-194 ordering that is only ~25% correct, vs 76-100% for first-valid. The
      objective is misaligned: a real stream's true order contains large `global_gain`
      steps, so the smoothest reorder is not the true one. Off by default, flag kept.
      See `CLAUDE.md` item 7's "2026-09-07 update 2".
- [x] 9. C1-C4 consistency/verification landed (`main.cpp` + `test/measurements.sh` +
      `test/regression.sh` + `test/regression_baseline.tsv`): deterministic default
      seed, reconstruction-vs-source report in `rp` output, one-command regeneration
      of this doc's numbers, and a local accuracy-regression baseline. See `CLAUDE.md`
      "Consistency & verification".
- [x] 10. A3 (bitrate) done - negative: VBR junction bitrate continuity is noise
      (AUC 0.55, not wired); CBR bitrate header filter implemented, 0.3% edge
      reduction. See `CLAUDE.md` item 7's 2026-09-09 update.
- [x] 11. A4 (arc-consistency prune) done - `propagateArcConsistency` in
      `adjacency.cpp`, gated on the pre-shuffle diagnostic. First structural win:
      `test7_shorter` 202→164 supernodes (−19%), `test7_short` 512→446. Still not
      enough alone. See `CLAUDE.md` item 7's 2026-09-09 update + the AC-3 TODO item.
- [x] 12. A5 (embedded non-audio) investigated - **no repro**: 0 orphaned tails / no
      `ID3`/`APE` magic on any of the 5 fixtures (`sideinfo_probe`). `test7`'s 2 breaks
      are a `computeChunkMeta` false-sync-tail selection bug instead (`CLAUDE.md` item 5,
      updated); the obvious `(hasCompleteFrame, ...)` scoring reorder trades those 2 for
      6 elsewhere, reverted.
- [ ] 13. **Decision still open.** Recommended order now (top of `CLAUDE.md` TODO):
      A2 fitted multi-feature junction score → Milestone 2 spectral decode only if that
      doesn't get large-VBR files there. Also open but lower-value: CBR padding cadence,
      and the cross-candidate "offset interior to a complete frame" check for item 5.
      M1.5 (`global_gain` ranking + first-valid) + A4 stand as the wins for small/medium
      files.

**Gate for Milestone 2 steps 3-7 below**: only build the full Huffman +
requantize + spectral-continuity path if M1.5's `global_gain` heuristic proves
insufficient on the hard fixtures (step 5). On the evidence so far it may
already be enough for small/medium files.
- [ ] 3. `decodeScaleFactors(...)` (block-type/`scalefac_compress`-aware).
- [ ] 4. `decodeHuffmanRegions(...)` (big_values + count1, stop at
      `part2_3_length` bit boundary).
- [ ] 5. `requantize(...)` -> frequency-domain magnitudes (no MDCT/IMDCT
      needed).
- [ ] 6. `main.cpp`: cross-chunk main-data byte assembly helper (walks
      backward through already-placed DFS chunks using `mainDataBegin`).
- [ ] 7. `computeGranuleEnergyProfile(coeffs)` + similarity function
      (cosine/L2) between boundary granules.
- [ ] 8. Wire into `dfs()`: sort `superAdj[cur]` candidates by continuity
      score (ranking heuristic only, never a hard reject).
- [ ] 9. Validate: cross-check decode against minimp3 on sample frames;
      confirm true-order adjacent granules score higher than random pairs
      on all 5 fixtures.
- [ ] 10. Measure: pinned-seed A/B, DFS nodes/wall-clock, ranking on/off, on
      `test7_shorter.mp3`. Keep only if it demonstrably helps.
- [ ] 11. Update `CLAUDE.md` with dated findings (either outcome).

---

## Verification (both milestones)

- Build: `cmake -B cmake-build-debug && cmake --build cmake-build-debug`.
- Correctness: `./cmake-build-debug/rp` against each fixture, then
  `./cmake-build-debug/evaluate_order <original.mp3> output.mp3`.
- Performance: pinned-seed A/B (DFS nodes via existing `Depth:` counter +
  wall-clock) per milestone before considering it "done."
- `CLAUDE.md` gets a dated entry either way, following its existing style.

## CI (2026-09-03)

`.github/workflows/ci.yml` builds all three CMake targets and runs
`sound/test/ci_sanity.sh` on every push/PR touching `sound/**`. The sanity
check reconstructs `sound/test/ci_sanity.mp3` - a small synthetic VBR file
(`test/make_ci_sanity.sh`, pure ffmpeg lavfi synthesis, committed so CI is
hermetic; the `*.mp3` gitignore rule has an explicit exception for it) - from
a pinned-seed shuffle and asserts an exact recovery (tau = +1.0, exact
position 100%). It reconstructs perfectly across every seed tried and with the
`global_gain` heuristic both on and off, so it catches pipeline regressions
(build break, crash, hang, DFS no longer finding the correct order) without
being brittle to heuristic tuning.
