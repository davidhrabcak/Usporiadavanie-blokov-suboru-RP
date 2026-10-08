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

**Step 2 - signal gate: PASS, by a wide margin, but only for `flux`.** `pcm_probe`, the rank of the
true successor among chunk i's candidates (deg >= 2, silent junctions excluded). This is the
**like-for-like** table. Candidates are restricted to those whose post side decodes without a
reservoir underflow (`postFailed == 0`). The DFS reservoir check already rejects the rest,
and their zero-filled gap would hand flux a free spike. Candidates must also be scorable
by Milestone 2. Flux and M2 are ranked on exactly the same sets:

| fixture | junctions | random mean rank | flux AUC | flux rank-1 | flux mean rank | M2 AUC | M2 rank-1 | M2 mean rank |
|---|---|---|---|---|---|---|---|---|
| ci_sanity     | 5    | 1.50 | 1.000 | 100%  | 1.00 | 0.840 | 80.0% | 1.20 |
| sample-3s     | 32   | 1.67 | 0.770 | 87.5% | 1.12 | 0.454 | 37.5% | 1.69 |
| CBR           | 364  | 8.19 | 0.820 | 67.6% | 2.81 | 0.599 | 16.5% | 7.63 |
| test7_shorter | 72   | 1.62 | 0.896 | 95.8% | 1.03 | 0.565 | 51.4% | 1.60 |
| test7_short   | 226  | 1.79 | 0.879 | 89.4% | 1.08 | 0.564 | 50.9% | 1.67 |
| test7         | 2326 | 3.19 | 0.938 | 88.9% | 1.10 | 0.574 | 32.0% | 2.85 |

(On all structural candidates `adj[i]`, unfiltered, flux rank-1 is 89.8 / 46.6 / 91.8 / 89.7
/ 80.5% on sample-3s / CBR / test7_shorter / test7_short / test7. That set is less
DFS-relevant: it includes reservoir-invalid candidates and is larger for CBR.)

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
  further *and* the partial path it builds is mostly correct. `rp` now prints the deepest
  partial path's accuracy against the source order (chunk-level junctions whose successor is
  the true next chunk):

  | fixture | search | PCM off: depth, true junctions | PCM on: depth, true junctions |
  |---|---|---|---|
  | test7_shorter | greedy (beam 1) | 39/164 sn, 70/77 (91%)    | 119/164 sn, 229/234 (98%) |
  | test7_shorter | broadening, 30 s | 137/164 sn, 249/264 (94%) | 147/164 sn, 279/286 (98%) |
  | test7_short   | greedy (beam 1) | 88/446 sn, 92/114 (81%)   | 180/446 sn, 230/238 (97%) |
  | test7_short   | broadening, 30 s | 264/446 sn, 307/367 (84%) | 377/446 sn, 486/506 (96%) |
  | CBR           | greedy (beam 1) | 116/658 sn, 40/115 (35%)  | 78/658 sn, 49/77 (64%) |
  | CBR           | broadening, 30 s | 278/658 sn, 89/277 (32%)  | 337/658 sn, 172/336 (51%) |

  The first wrong junction still comes early (the correct prefix is 26 chunks on
  test7_shorter / test7_short, 3 on CBR). Later junctions are overwhelmingly right, so the
  path is a few wrong splices in a mostly correct sequence, and those few eventually
  strand it.
- The weight barely matters (W 0.5 to 100 gives the same greedy depth to within ~20%).
- Cost: ~2 s of calibration + decodes on CBR, ~0.2-0.4 s on test7_*. Negligible next to the
  search budget.

**Why it still doesn't complete.** test7_shorter has ~120 branch points. At ~90-96% rank-1,
a greedy pass is wrong somewhere with near certainty, and the measurement above shows exactly
that: the first wrong splice comes within the first ~26 chunks. Chronological DFS backtracking
only revisits the *deepest* choices, never that early wrong turn. On the VBR fixtures, 96-98%
of the junctions on the deepest path are correct, so the bottleneck has moved to the search.
Next steps this signal makes worth doing:
1. Global assignment: min-cost successor matching (Hungarian) on the flux costs, then
   repair. With 80-92% rank-1 on VBR, an assignment should get most edges right at once,
   instead of compounding errors along a path.
2. Beam search over partial orderings scored by *summed* flux (not first-valid DFS), with
   an emit-best-partial output.
3. Making the CBR case sharper. The window currently ends at the candidate's first chunk.
   Including the candidate supernode's 2nd chunk would give more clean post-junction audio.

## Step 5 - confident-edge locking (2026-10-09)

The search stalled because a few early wrong turns stranded it, even though 96-98% of the
junctions on its deepest path were right. So: before the search, **force** the edges flux is
very sure about. Then the DFS doesn't have to choose at those branch points at all.

**Mechanism** (`edge_lock.{hpp,cpp}`). Flux depends on the path context, which isn't known
pre-search. So each edge a->b is decoded as `[p, a, b]` for every structural predecessor p
of a (an evenly spaced sample of 8 when there are more), and summarised by the median.
a->b is locked when its median flux beats every other successor of a **and** every other
predecessor of b by a factor >= tau. Locked edges are forced (competitors removed), AC-3
re-runs, and the whole thing iterates on the pruned graph until no new lock appears
(`RP_LOCK_ROUNDS`, default 8). It has the same soundness gate as AC-3 (only when the
pre-shuffle check found 0 breaks). A decode-cost cap (`RP_LOCK_MAX_DECODES`, default 40k
per round) skips dense graphs. CBR would cost ~170k decodes (~2 min) per round, for <5%
coverage.

**Objective alignment (2-opt swap test, pcm_probe).** For two true edges a->b, c->d whose
crossed pair a->d, c->b is also structural, the true pair has the lower summed flux in
100% (ci_sanity, sample-3s), 97.4% (CBR), 98.2% (test7_shorter), 99.0% (test7_short), and
99.6% (test7) of swaps. Unlike `global_gain` (the failed best-of experiment), summed flux
points at the true order, so global/objective searches on it are viable.

**Lock precision on the true order** (pcm_probe, median mode; "wrong" = locks that are not a
true successor pair; coverage = correct locks / true edges at a branch point; single round,
no iteration):

| tau | sample-3s | CBR | test7_shorter | test7_short | test7 |
|---|---|---|---|---|---|
| 1.00 | 1 wrong / 62% | 29 wrong / 9% | 5 wrong / 74% | 17 wrong / 66% | 202 wrong / 34% |
| 1.10 | 1 wrong / 48% | 0 / 4.3% | 0 / 68% | 0 / 58% | 8 wrong / 16% |
| 1.25 | 1 wrong / 36% | 0 / 2.1% | 0 / 59% | 0 / 49% | 2 wrong / 7% |
| **1.50** | 0 / 22% | 0 / 0.8% | 0 / 47% | 0 / 33% | 1 wrong / 2.6% |
| 2.00 | 0 / 10% | 0 / 0% | 0 / 28% | 0 / 19% | 0 / 0.7% |

The robust mode (worst context must beat every competitor's best) is 100% precise at every
tau, but it covers far less (test7_shorter 34%, test7_short 15%, 0 on CBR/test7). **Default
tau = 1.5.** It is the lowest threshold with no wrong lock on any completable fixture.
test7's single wrong lock at 1.5 is moot here (test7 is skipped by the gate and the cost
cap), but it shows the margin isn't infinite: one wrong lock makes the true order
unreachable.

**Results in `rp`** (default tau 1.5, iterated; "locks correct" is checked against the source
order and printed by `rp` as a diagnostic):

| fixture | before (PCM ranking only) | with locking |
|---|---|---|
| ci_sanity | exact, 15 supernodes | exact, 13/13 locks correct, **1 supernode** |
| sample-3s | exact on seeds 1-7, 1.25M DFS nodes | exact on seeds 1-7, 14/14 locks correct, 28 supernodes, **1,792 nodes (0.1 s)** |
| test7_shorter | no complete ordering | **byte-exact on seeds 1-7**, 83/83 locks correct, 164 -> 49 supernodes, 55 DFS nodes (0.07 s) + ~1.7 s locking |
| test7_short | no complete ordering, deepest 377/446 sn | still no complete ordering (seeds 1-3); 159/159 locks correct, 446 -> 230 supernodes, deepest path 594/598 true junctions |
| CBR, test7 | - | unchanged (skipped: cost cap / pre-shuffle breaks) |

Lock counts and outcomes are identical across seeds 1-7. `regression.sh` passes, and the
baseline has been updated: test7_shorter goes from FAILED to 100.

**Lower tau on test7_short.** At tau 1.1 (309/309 locks correct, 63 supernodes) the DFS
*completes*, but on a wrong-but-valid ordering: 606/609 junctions are true, with one block
of the song moved (A-C-B instead of A-B-C, 3 wrong junctions, so position accuracy reads
0.5%). Its summed flux (`RP_SCORE_ORDER=1`) is 506.78 vs **503.40 for the true order**, so a
block-move local search minimising summed flux (moves constrained by `canFollow` + the
reservoir check) would repair it. Not built yet, and tau 1.1 is not the default (the true-order
probe shows wrong locks at 1.1 on sample-3s and test7).

**Next steps:**
1. Block-move repair on summed flux for complete-but-wrong orderings.
2. Use flux as a global objective (beam over partial orderings / min-cost matching). The
   2-opt test says it is aligned.
3. CBR: cheaper locking (e.g. only score chunks with few predecessors) and a sharper
   window (more post-junction audio).

## Step 6 - block-move repair + lock-threshold fallback (2026-10-09)

**Repair** (`repairOrdering` in `main.cpp`, `RP_REPAIR`, `RP_REPAIR_PASSES`). After the DFS
returns a complete ordering, it runs a best-improvement local search over two move types:
- **block move**: take seq[i..j] out and reinsert it after position k (3 new junctions);
- **block swap**: exchange two disjoint blocks (4 new junctions).

A move is only considered when every new junction is an edge of the **full** structural
graph (taken before AC-3 and locking, so a wrong lock can be undone), the whole ordering
passes the bit-reservoir check, and the cost strictly drops. Chunk 0 stays pinned. Moves are
enumerated through structural predecessor/successor lists, ~30k valid candidates per pass on
test7_short, ~4 s.

Two things the first version got wrong, found on the test7_short tau-1.1 near-miss:
1. **Block moves alone can't fix it.** The wrong ordering was A-X-Y-B where the truth is
   A-B-Y-X. Every single block move along the way is structurally invalid or not
   improving. A single block swap (X <-> B) fixes it, so block swaps were added.
2. **The cost could be gamed.** With unmeasurable junctions scored at a neutral value, the
   search took a move (-0.086) that made two high-flux junctions unmeasurable, away from the
   truth it had just reached. The cost is now lexicographic: (number of unmeasurable
   junctions, summed log1p(flux) over the rest).

Note: on the true test7_short order, 60 moves would lower summed flux *further* but fail the
reservoir check. So the true order is not the pure flux minimum. It's flux **plus** the
reservoir constraint that pins it down.

**Lock-threshold fallback** (`RP_LOCK_TAU` is now a schedule, default `1.5,1.25,1.1`). If no
complete ordering is found at one tau, locking + search is redone from the unlocked graph at
the next one. Every attempt except the last gets a search budget (`RP_TIME_BUDGET`, else
45 s broadening / 30 s), so a mid-size graph can't run an unbounded exhaustive DFS. A
too-low tau risks a wrong lock; when the DFS still completes, the repair can undo it.

**Results (default settings):**

| fixture | before step 6 | now |
|---|---|---|
| ci_sanity | exact | exact (seeds 1-3) |
| sample-3s | exact, seeds 1-7 | exact, seeds 1-7, repair 0 moves, ~1.6 s |
| test7_shorter | exact, seeds 1-7 | exact, seeds 1-7, repair 0 moves, ~3 s |
| **test7_short** | no complete ordering | **byte-exact on seeds 1-7**, every lock correct. Seeds 3/5/6/7 finish at tau 1.25 (~61 s). Seeds 1/2/4 reach tau 1.1, which yields the A-X-Y-B ordering (606/609 true junctions), and **one repair swap makes it exact** (~99 s) |
| CBR, test7 | not completed | unchanged (locking skipped: cost cap / pre-shuffle breaks; DFS never completes, so the repair never runs) |

`regression.sh` passes, and the baseline has been updated: test7_short goes from FAILED to
100.0 (supernodes 63). regression.sh now reads the last lock attempt's supernode count.
`ci_sanity.sh` passes.

**Stress test, tau forced to 1.0** (the probe shows wrong locks there), seeds 1-3:
- sample-3s seed 3: 2 wrong locks. The DFS still completes on a wrong ordering, and the
  repair fixes it in 1 move, giving byte-exact output.
- test7_shorter / test7_short: 4-25 wrong locks. The graph has no complete ordering left,
  so the repair never gets to run.

The repair only rescues orderings the search completes, which is why the schedule starts
strict.

**Termination:** with a lock schedule every attempt is time-budgeted, including the last
(30 s with a fallback left, 45 s for the last), so `rp` still always terminates even when
locking shrinks a large graph below the 120-supernode broadening threshold. A single-tau
run (`RP_LOCK_TAU=1.5`) keeps the old rule.

**Risk trade-off of the fallback:** the default can now reach tau 1.1, where the true-order
probe recorded wrong locks (1 on sample-3s, 8 on test7). Before the fallback, a file that
failed at 1.5 honestly reported "no complete ordering". Now it may emit a complete but
wrong ordering, and the repair only sometimes rescues that (stress test above). Real use has
no `[vs source]` line to catch it. `RP_LOCK_TAU=1.5` restores the old conservative
behaviour.

**Repair cost on large inputs:** block-swap enumeration grows roughly as n^2 x degree^2.
That's ~4 s/pass at n=610, but it could be very slow at test7's 3,351 chunks if that file
ever completes (with up to 50 passes). Not an issue on any fixture today.

**Cost:** the wall-clock is dominated by the budgets of failed lock attempts (45 s + 30 s
before the tau-1.1 attempt on test7_short). The repair itself is ~4 s.
