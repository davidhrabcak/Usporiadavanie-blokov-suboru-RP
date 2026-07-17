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
- [ ] 2. `main_data_decoder.hpp`/`.cpp`: `GranuleSideInfo` parser (extends
      Milestone 1's bit-source helper to capture `bigValues`, `globalGain`,
      `scalefacCompress`, block-type/window flags, `tableSelect[3]`,
      region0/1 counts, `scalefacScale`, `count1TableSelect`).
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
