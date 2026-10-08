# PLAN - full PCM decode at the junction (branch `full-pcm-decode`)

## Motivation

Milestone 2 (`main_data_decoder`) stops at the Huffman-decoded *quantised* coefficients and
compares a coarse 12-band |q| profile of two granules about one frame apart. It ignores
requantisation (`global_gain`, scalefactors), channel 1, short blocks, and it never looks at
the frame that actually straddles the chunk boundary. The `PLAN_huffman_junction_filter.md`
NO-GO showed that a *bit-count* check on the straddling frame cannot discriminate (the
Huffman decoder consumes exactly `part2_3_length` by construction). What it did not test is
what the straddling frame **sounds like** once decoded all the way to PCM.

Hypothesis: when chunk X is spliced after `cur`, the frame that straddles the boundary and
the first frames of X whose bit-reservoir pointer reaches back into `cur` are decoded from a
mix of bytes. For the true successor they are just the next ~26 ms of the song. For a wrong
successor they are a "hybrid" frame: Huffman data from one place in the file, scalefactors
and side info from another. That should come out as a broadband burst, an energy jump, or a
spectral discontinuity in the PCM. In addition, X's later frames come from a different moment
in the song than `cur`, which a fine-resolution spectrogram should see even when the coarse
envelope does not.

## Decoder

**Vendored [minimp3](https://github.com/lieff/minimp3) (CC0)** - `third_party/minimp3.h`,
compiled once in `pcm_decoder_impl.cpp`. Full Layer I/II/III decode: requantisation,
MS/intensity stereo, reordering, alias reduction, IMDCT (long + short blocks), polyphase
synthesis. The project already uses minimp3 as its reference decoder and as the source of
`huffman_tables`. Writing a Layer III decoder from scratch is a separate project with its own
correctness risk, and it is not what is being tested here. Float output
(`MINIMP3_FLOAT_OUTPUT`), so hybrid-frame overshoot is visible rather than clipped.

## Mechanism (`pcm_junction.{hpp,cpp}`)

For a candidate edge `cur -> X` with path predecessor `P`:

1. Concatenate the raw chunk bytes `[P, cur, X]` and feed them, frame by frame from `P`'s
   first frame header (`headOffset`), to a **fresh** minimp3 decoder. `P` is warm-up: it
   fills the bit reservoir, the IMDCT overlap and the synthesis filterbank delay line, so that
   `cur`'s frames decode exactly as they do in a full-file decode.
2. Track every frame's byte span and output-sample span. The **junction frame F** is the
   first frame that ends past the `cur|X` byte boundary. The junction sample J is where F's
   output starts. "pre" is everything before J (genuine audio, already on the path). "post"
   is F onward, which is what changes with the candidate.
3. Mix down to mono, compute a log-magnitude spectrogram (Hann, 1024-point FFT, hop 576
   = one granule), and score the transition across J.

Inefficient by design: one full decode of ~3 KB (≈7 frames) plus FFTs per (P, cur, X) triple.
It is memoised per triple in the DFS.

## Junction metrics (all "lower = more continuous")

- `flux`: the largest spectral flux (mean |Δ log-mag| between consecutive windows) among
  windows that overlap the post region, divided by the median flux inside the pre region.
  A splice spike relative to the local baseline.
- `specDist`: the distance between the mean log-spectrum of the last 2 pre windows and the
  first 2 post windows.
- `rmsJump`: |Δ log RMS| between the last pre granule and the first post granules.
- `hfBurst`: post/pre ratio of energy above ~11 kHz (hybrid frames are broadband).
- `peak`: the peak |sample| in post divided by the peak in pre.

## Steps

1. **[gate: correctness]** On the true order, the windowed decode of `[i-1, i, i+1]` must
   match a full-file minimp3 decode sample-for-sample on the frames of `cur` and `X`. If it
   does not, add more left context.
2. **[gate: signal]** `test/pcm_probe.cpp`: for every chunk i with structural out-degree >= 2
   (`buildAdjacency`, true order), score every candidate in `adj[i]` with each metric. Report
   AUC (true vs wrong *structural* candidates, never random pairs, which break frame sync
   and inflate the result), rank-1 %, and mean rank vs the (deg+1)/2 random baseline. Run
   Milestone 2's L2 on the **same** candidate sets as the baseline. Silent junctions are
   counted separately. CBR is the decisive fixture: M2 has AUC 0.83 there but only 18%
   rank-1.
3. **Wire into `dfs()`** behind `RP_PCM` (default on if step 2 is a win). Compute each
   candidate's cost once before sorting, not inside the comparator. Combine with the
   existing terms using a per-file calibrated scale (z-score style), so this does not
   inherit the raw-L2 scale cliff. It is a ranking term only, unless step 2 shows zero
   true-junction violations at some threshold, in which case it can also become a
   hard filter.
4. **Test.** `test/regression.sh`, `test/ci_sanity.sh`, the sample-3s seeds 1..7,
   `RP_PCM=0` vs `=1` A/B, and wall-clock (a slower per-edge cost may blow the 20 s regression
   budget: report signal and cost separately).

## Outcome (2026-10-08)

**Step 1 - correctness gate: PASS.** With 2 warm-up chunks (`[i-2, i-1, i, i+1]`) the windowed
decode is sample-exact against a full-file minimp3 decode on every compared frame of every
fixture (test7: 13,403 frames, max |diff| = 0). With only 1 warm-up chunk, sample-3s drops
to 78.5% exact on the pre side (the first decodable frame of P lacks synthesis/IMDCT
history), so the DFS uses up to 3 path chunks plus the candidate.

**Step 2 - signal gate: PASS, by a wide margin, but only for `flux`.** `pcm_probe`, true
successor among the structural candidates `adj[i]` (deg >= 2, silent junctions excluded):

| fixture | junctions | mean deg | flux AUC | flux rank-1 | flux mean rank (random) | M2-L2 rank-1 (same sets) |
|---|---|---|---|---|---|---|
| ci_sanity     | 14   | 2   | 1.000 | 100%  | 1.00 (1.50)  | 85.7% |
| sample-3s     | 49   | 2.6 | 0.810 | 89.8% | 1.12 (1.82)  | 40.5% |
| CBR           | 650  | 33  | 0.795 | 46.6% | 5.29 (16.98) | 11.1% |
| test7_shorter | 184  | 2.4 | 0.868 | 91.8% | 1.08 (1.70)  | 51.1% |
| test7_short   | 485  | 3   | 0.888 | 89.7% | 1.10 (1.99)  | 45.9% |
| test7         | 3319 | 9.6 | 0.929 | 80.5% | 1.29 (5.30)  | 20.0% |

`specDist` (envelope distance) is mediocre (AUC 0.56-0.68), and `rmsJump`/`hfBurst`/`peak`
are noise (~0.5): a wrong splice is not louder or brighter. Its spectrum is just
discontinuous relative to the local frame-to-frame variation. A z-summed combo of all five
dilutes the AUC. **Not a hard filter**: true-junction flux has a long tail (CBR p99 7.1,
test7_short max 28.9) that overlaps the wrong candidates at every threshold.

**Step 3 - wired** (`main.cpp`, `pcmRaw`/`pcmJunctionCost`, `RP_PCM`, `RP_PCM_W`). log1p(flux)
is scaled so its spread is W (= 2) times that of junctionCost + spectral (calibrated per file
over ~600 random structural edges with a plausible path context), and is added to the
candidate sort key. Costs are precomputed once per candidate list. Memoised per
(PP, P, cur, cand).

**Step 4 - test results.**
- `test/ci_sanity.sh`: PASS (exact). `test/regression.sh`: PASS, no change on any fixture.
- sample-3s, seeds 1-7: still 100% exact on every seed. The DFS explores **1.25M nodes vs
  4.39M** without the PCM term (3.5x fewer), ~1.5 s vs ~3 s.
- Hard fixtures (30 s budget): **still no complete ordering**, but the search gets
  substantially further. Greedy (beam=1) depth before dead end: test7_shorter 38 -> 118/164,
  test7_short 87 -> 179/446. Beam=2 plateau: 92 -> 111/164 and 230 -> 326/446. CBR beam=1
  goes 115 -> 77 (worse). CBR's degree is ~33, so 47% rank-1 still leaves the greedy path
  wrong about every other branch point.
- The weight barely matters (W 0.5 to 100 gives the same greedy depth to within ~20%).
- Cost: ~2 s of calibration + decodes on CBR, ~0.2-0.4 s on test7_*. Negligible next to the
  search budget.

**Why it still doesn't complete.** test7_shorter has ~120 branch points. At ~90% rank-1, a
greedy pass is wrong somewhere with near certainty (0.9^120 ~ 3e-6), and chronological
DFS backtracking only revisits the *deepest* choices, never the early wrong turn that caused
the dead end. The per-edge signal is now strong. The bottleneck has moved to the search. The
natural next steps, which this signal finally makes worth doing:
1. Global assignment: min-cost successor matching (Hungarian) on the flux costs, then
   repair. With 80-92% rank-1 on VBR, an assignment should get most edges right at once,
   instead of compounding errors along a path.
2. Beam search over partial orderings scored by *summed* flux (not first-valid DFS), with
   an emit-best-partial output.
3. Making the CBR case sharper. The window currently ends at the candidate's first chunk.
   Including the candidate supernode's 2nd chunk would give more clean post-junction audio.
