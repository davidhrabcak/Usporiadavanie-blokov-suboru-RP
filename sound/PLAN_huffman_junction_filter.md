# Plan: split-frame Huffman-decode consistency as a structural junction filter

Branch: `huffman-junction-filter` (off `sideinfo-mainbits` @ 1a81545). Companion:
`CLAUDE.md`, `PLAN_sideinfo_mainbits.md`. This is the "the search itself must
change" step from `CLAUDE.md` item 7 (2026-09-10) - it moves the Milestone 2
decoder from a *ranking nudge* to a *hard structural prune*, which is a bigger
change to the algorithm's inner workings than anything on `sideinfo-mainbits`,
hence the separate branch.

## Why

Every per-edge *signal* is now in (global_gain, A2, A3, A4, reservoir model, M2
spectral). The large VBR fixtures (`test7_shorter` 164 supernodes, `test7_short`
446, `test7` 3351) and CBR still find no complete ordering: the adjacency graph
is too dense. Root cause (`CLAUDE.md` item 7): Case-1 `canFollow` accepts *any*
decodable, profile-matching header at the demanded byte offset `rem` in the
candidate successor - and lots of chunks have one there by chance.

The decisive check we have not applied: at a Case-1 junction a->b, the split
frame F (header + side info in `a`, `a.tailOverflow >= 4`) has a known exact
`part2_3_length`. Its main data is [reservoir from frames before `a`] + [F's
payload = last bytes of `a` + first `rem` bytes of `b`]. **Huffman-decoding F's
big_values + count1 regions must consume exactly `part2_3_length` bits** (this is
bit-exact on ~46k real granules - see Milestone 2). For a *wrong* `b`, the first
`rem` bytes are another frame's content; fed as F's continuation the decoder hits
an invalid leaf or desyncs and lands far from `part2_3_length`. Estimated pass
rate for a wrong `b`: ~1-5%.

Expected effect: CBR out-degree ~33 (n=658) / ~90+ (n~1875) -> ~2-4. Large-VBR
branch points similarly collapse. That makes long CBR and large VBR
**combinatorially plausible** for a bounded search - the open question this plan
answers empirically.

## Known limits going in

- **Silent runs**: consecutive digitally-silent frames (`part2_3_length ~ 0`) are
  byte-identical -> genuinely 1-of-N! orderings. Perceptually harmless (any order
  sounds the same); detect and collapse into a "silent block" rather than trying
  to order it.
- **Window-switching frames (~8%)**: the decoder skips short/start/stop blocks.
  The filter can't run on those junctions -> they fall back to the weak
  structural check. Residual branching depends on how they cluster. Short-block
  decode support is a stretch goal, not required for the go/no-go.
- **Decoder correctness is now load-bearing**: a false negative removes the true
  ordering from the graph entirely. Must validate the filter never rejects a true
  Case-1 junction on any fixture's true order *before* enabling it.
- **Reservoir assembly is per-path** (needs `a`'s predecessors' main data), so
  this is a DFS-time prune, not a `buildAdjacency`-time one - unless we accept a
  weaker "no reservoir, F's payload only" variant for graph pruning.

## RESULT: NO-GO (2026-09-10, step 1)

`sideinfo_probe` Measurement E, all 5 fixtures + ci_sanity:

| fixture       | TRUE pass | WRONG-successor pass | nonzero-count ratio |
|---------------|-----------|----------------------|---------------------|
| sample-3s     | 48/48     | **100.0%**           | 1.00                |
| CBR           | 456/462   | **93.1%**            | 1.00                |
| test7_shorter | 268/268   | **99.6%**            | 1.00                |
| test7_short   | 516/516   | **99.7%**            | 1.00                |
| test7         | 2862/2864 | **99.6%**            | 1.00                |
| ci_sanity     | 34/34     | **100.0%**           | 1.00                |

Wanted wrong-successor pass < 5%. Got 93-100%. **The partial Huffman decoder
cannot tell a correct continuation from garbage**, because:

1. `big_values` decodes a **fixed** number of pairs (from side info), and count1
   is bounded by `part2_3_length` - so `bitsConsumed` lands on `part2_3_length`
   *by construction*, regardless of the bytes.
2. The minimp3 tables are complete: every peek resolves to some leaf, so garbage
   bytes never raise an "invalid code" error.
3. The decoded coefficient *count* is content-independent (nonzero ratio = 1.00);
   only the coefficient *values* differ - and that's exactly the M2 spectral
   profile, already wired and already weak on the hard fixtures.

The bit-exact `part2_3_length` match from Milestone 2 was real but says only "the
decoder terminates where it should", not "this is the right continuation".

## Consequence

There is no cheap decode-based *structural* filter for Case-1 junctions. Long CBR
is **not combinatorially plausible** to reconstruct exactly with structure +
light decode: the graph is dense (~33 out-edges/chunk at n=658, growing with n),
the per-edge signals are weak (gg AUC 0.68 / spectral rank-1 18% on CBR), and
nothing separates true from false continuations. Silent runs are additionally
ambiguous in principle.

Remaining options, none cheap or sure:
- **Global min-cost assignment (Hungarian)** over gg+spectral weights - sidesteps
  the exponential DFS but uses the same weak signals and yields sub-cycles that
  need heuristic patching. Likely still wrong on CBR; *might* help sparse VBR.
- **Requantised spectral + real audio-continuity** (energy decay / spectral
  smoothness) - the one physically-grounded signal left, bigger decoder, fuzzy.
- **Accept best-effort**: bounded beam search that emits the best *partial*
  ordering. Won't be the exact original for large files.

Recommendation: merge `sideinfo-mainbits` (the completed, tested signal work),
document large-file exact reconstruction as a limitation with a best-effort
fallback, and treat Hungarian / requantised-spectral as research spikes only if
the exact large-file case becomes a hard requirement.

## Steps (superseded by the NO-GO above)

- [x] 1. **Experiment (go/no-go), in `sideinfo_probe`.** Done - NO-GO. For every Case-1 junction
      in the true order, and for a sample of *wrong* successors of the same
      predecessor, decode the split frame F (reservoir from the true preceding
      frames + `a` tail + candidate `b` head) and record: leaf-error / bit-count
      delta / pass. Report true-junction pass rate (must be ~100%) and wrong-
      successor pass rate (want < 5%). Also: how many Case-1 vs Case-2 vs
      window-switching junctions per fixture; silent-run lengths.
- [ ] 2. If go: `splitFrameHuffmanOk(prevMainData, aMeta, aBytes, bBytes) -> bool`
      in `main_data_decoder` (reuse `decodeChunkFrame` assembly + a "did it land
      on part2_3_length" predicate).
- [ ] 3. Wire as a **hard prune** at the DFS edge (in `dfs()`, alongside the
      reservoir check - reject the candidate if the split frame fails). Needs the
      path-prefix reservoir; assemble by walking back through `state.order`'s
      chunks. `RP_HUFFCHECK=0` to disable for A/B.
- [ ] 4. Silent-run handling: detect `part2_3_length == 0` frames; collapse
      maximal runs of silent-only chunks into one block (order within = don't
      care) before/after the DFS.
- [ ] 5. Validate: filter never rejects a true Case-1 junction on any fixture's
      true order. Then A/B: out-degree histograms, does `test7_shorter` /
      `test7_short` / CBR now complete, reconstruction accuracy, wall-clock.
- [ ] 6. If it works: consider also applying a payload-only (no reservoir) variant
      inside `buildAdjacency` to prune the *static* graph (cheaper supernode
      collapse, smaller DFS from the start).
- [ ] 7. Stretch: short-block (window-switching) decode so the ~8% of junctions
      the filter currently can't check are covered.
- [ ] 8. `CLAUDE.md` dated entry; decide merge order of the two branches.
