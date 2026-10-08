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

## Outcome

(filled in below as steps land)
