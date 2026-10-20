# PLAN - edge probabilities + most-probable Hamiltonian path (branch `prob-hampath`, from `global-matching`)

## Proposal (as asked)

Turn everything we already measure at a chunk junction - header/structure information and
the PCM "smoothness" of the decoded signal - into one **probability** `p(a→b) ∈ (0,1)` that
chunk `b` really follows chunk `a`. Then:

1. evaluate it on every candidate junction, settling easy pairs/runs first so the expensive
   part runs on a smaller problem;
2. build a directed weighted graph from those probabilities;
3. find the **most probable Hamiltonian path** from chunk 0, the original order, with a
   proper (ideally exact) solver instead of DFS + heuristics.

This document fixes the formulation, says what is reused, and lists the measurements that
decide whether each step is worth building.

## What we already know (evidence this plan is built on)

- **Signal.** PCM junction flux is the strongest per-edge signal: rank-1 is 68-96% of junctions,
  AUC 0.77-0.94 (`pcm_probe`). Header-only signals are weak: `global_gain` has AUC 0.68 and
  M2 spectral has AUC 0.45-0.83.
- **The global objective already beats DFS.** `globalMatch` (commit `9d705fe`) solves the
  exact min-cost successor assignment on `log1p(median flux)`. That assignment is the
  classic relaxation of this exact path problem.
  - It gets 92-98% of junctions right in ~0.02 s with only 0-3 cycles to patch.
  - After patching and repair it is exact on cases DFS never solved: cbr128_30s 2 KB,
    held-out vbrq2_30s 1 KB, and test7_short at the strictest lock attempt.
- **The objective, not the search, causes the remaining wrong outputs.**
  `RP_SCORE_ORDER=1` shows the wrong order is *cheaper* than the true one in three cases.
  An exact solver on today's costs would return the same wrong orders.

  | case | summed log1p(flux): found (wrong) | true order |
  |---|---|---|
  | cbr192_10s 2 KB | 95.494 | 95.531 |
  | Queen VBR 4 KB (all 3 seeds) | 1051.096 | 1051.677 |
  | bohemoan 32 kbps 4 KB | 296.556 | 297.465 |

- **The local stability gap doesn't separate right from wrong.** The gap is how much cost
  rises for the cheapest valid one-move alternative (commit `eac8255`).
  - Wrong outputs: 0.013 and 0.037.
  - Exact outputs: 0.046 (cbr128_30s 2 KB), 0.76, 0.81, 1.42.

  A sum of uncalibrated `log1p(flux)` values has no meaningful scale. A probability would.
- **The bit reservoir is a hard constraint, but it isn't a per-edge one.**
  - Whether chunk `k` decodes depends on the reservoir level built up along the path.
    It re-syncs within a frame or two, so it is "almost local", but it can't be folded into
    the edge weights.
  - Today the assignment is reservoir-invalid in most cases, and the breaks-first repair has
    to fix that.
- **Cost is dominated by window decodes.** One decode takes 0.3-0.8 ms at ≥128 kbps and
  ~1.8 ms per thread at 32 kbps. Graph sizes (from the benchmark, after chain collapse):

  | file × chunk | nodes | arcs | arcs/node |
  |---|---|---|---|
  | Queen VBR 1 KB | 5 322 (5 266 supernodes) | 76 714 | 14 |
  | bohemoan 32 kbps 1 KB | 1 405 | 394 244 | 281 |
  | bohemoan 32 kbps 2 KB | 703 | 98 562 | 140 |
  | cbr320_30s 1 KB | 1 174 | 27 025 | 23 |
  | vbrq2_30s 1 KB | 602 (425) | 1 242 | 2.1 |

  VBR graphs are sparse. Long CBR graphs are dense, and density grows with file length.
  Scoring every arc even once can be the bottleneck: the bohemoan 1 KB matching is skipped
  by the 100k-decode cap today.

## Formulation

### What "most probable path" means

Let `x_e ∈ {0,1}` select arcs that form a Hamiltonian path from chunk 0. If edge events are
treated as independent, the likelihood of a path is

```
L(x) = Π_{e chosen} p_e · Π_{e not chosen} (1 - p_e)
log L(x) = const + Σ_{e chosen} log( p_e / (1 - p_e) )
```

The MAP path therefore **maximises the summed log-odds**. Equivalently, it is a
**shortest Hamiltonian path with arc cost `w_e = -log(p_e / (1-p_e))`**. Two things follow.

- It is **not** "maximise Σ p_e", which is the literal reading of "most expensive Hamiltonian
  path". Summing raw probabilities lets several mediocre arcs outweigh one near-certain arc,
  and it isn't a likelihood. The weights must be logs.
- Because every path has exactly n-1 arcs, using `-log p_e` gives the same argmax as
  log-odds only if `log(1-p_e)` is roughly constant. It isn't for dense rows. **Use
  log-odds.** Offset them so they are non-negative (`w_e = max_logodds - logodds_e`) when a
  solver needs that; adding a constant per arc doesn't change the optimum.

### Independent per-edge p versus a per-row distribution

Each chunk has exactly one true successor and one true predecessor, so the truth is a
permutation matrix. Two ways to get probabilities:

- **(a) Edge classifier.** `p_e = σ(f(features_e))`, trained per arc. Simple and calibratable.
  It ignores the competition between arcs, so relative features have to inject that (below).
- **(b) Doubly-stochastic normalisation.** Start from per-arc likelihood ratios and apply
  Sinkhorn row/column scaling. You get the approximate marginal `P(a→b | everything)` under
  the permutation constraint. This is a better input for "lock the easy pairs" and for the
  confidence report than raw (a). It costs a few sparse matrix passes.

**Plan:** (a) produces the weights the solver optimises. (b) is computed from (a) and used
only for locking decisions and the confidence report. Measure whether (b)'s marginals are
better calibrated than (a) before relying on them.

### Context dependence

The flux of `a→b` depends on the two chunks before `a` (decode warm-up). Arc weights are
therefore an approximation:
- within a supernode, the internal context is exact;
- for single-chunk supernodes, the median over sampled predecessor contexts is used (as
  `globalMatch` and `edge_lock` do today).

The context spread (min/median/max over contexts) becomes a feature.

After solving, re-score the chosen path's arcs in their exact contexts, update those weights,
and re-solve. Stop at a fixed point, at most 3 iterations. Cycling here means the model is
uncertain, which goes into the confidence report.

## The probability function

### Features per arc a→b (all already computable)

**Header/structure (cheap, no decode)**
- `canFollow` case: 1 (known tail length) or 2 (split header).
- Reservoir slack: the first frames of `b` give their `mainDataBegin`; compare it with the
  bytes `a`'s tail can supply. Negative slack means impossible; small slack is suspicious.
- `global_gain` continuity: the `junctionCost`/`boundaryGain` term.
- M2 spectral distance: `spectralJunctionCost`.
- Bitrate and padding continuity: an exact feature for CBR, a weak one for VBR.
- `part2_3_length == 0` runs (silence). Their order is ambiguous in principle; mark them, so
  the model reports p≈uniform rather than guessing.

**Signal (PCM decode, `pcm_junction`)**
- `flux`: min, median and max over contexts.
- `specDist`, `rmsJump`, `hfBurst`, `peak`. Each was noise alone, but they may add to flux in
  a joint model.
- Unmeasurable/silent flag, for the windows `scoreJunction` can't score.

**Relative (the competition; this is what locking's margin uses today)**
- Rank of `b` among `a`'s successors and rank of `a` among `b`'s predecessors, both by flux.
- `log(flux_e / best competing flux)` on the row and on the column. This is the lock
  margin, made continuous.
- Out-degree of `a` and in-degree of `b`.

### Model and training

- **Data.** A new `pcm_probe --dump-arcs` mode writes one CSV row per structural arc with all
  features and label = (b is the true successor of a). It runs on the true order, so labels
  are free.
  - Training: `test/gen/` (14 files) × {1, 2, 4, 8} KB. Queen and bohemoan are added only as
    separate rows, not mixed with the rest.
  - Held-out evaluation: `test/gen2/` (a different start offset in the source, so different
    audio) plus the regression fixtures.
- **Model.** Start with logistic regression on standardised features, with a few `log`
  transforms. It has around 20 weights, is trivially reproducible in C++, and is
  interpretable. Escalate to a small gradient-boosted model, exported as a flat tree array,
  only if held-out log-loss improves clearly.
- **Calibration.** Report a reliability curve, the Brier score and log-loss on held-out data.
  Fit isotonic or Platt rescaling on a training split if needed.
- **Class balance.** Positives are about 1/(arcs/node). Weight the classes, then re-calibrate.

## "Easy pairs first": cascade and locking

The user's step 1 maps onto machinery that already exists. The changes are to drive it by
calibrated probabilities and to make it decode-budgeted.

1. **Structural filter.** `canFollow`, CBR header filter, AC-3. This already exists and
   produces n chunks and m arcs.
2. **Cheap prescore.** Header/structure features only, with no decode: a logistic model with
   only the cheap features. Keep an arc if it is in the top-k of its row **or** its column by
   cheap score. Choose k per file so that measured recall of true arcs on training data is
   ≥ 99.9%. This is what makes dense CBR affordable, because 281 arcs/node can't all be
   decoded. **Gate:** if the cheap model can't reach that recall with a meaningful k (say
   k ≤ 30 of 281), the cascade gives nothing and dense CBR stays out of scope (CLAUDE.md
   Known Limitation 1).
3. **One-context PCM score.** Score surviving arcs with one context, the supernode's own
   chunk or the best-scoring predecessor.
4. **Multi-context PCM score, contested arcs only.** Add contexts only for arcs where the
   row or column decision is still open: the top-2 log-odds are within δ.
5. **Lock.** Lock arcs with Sinkhorn marginal ≥ `p_lock` on both sides, then run AC-3 and
   chain collapse. Supernodes are the "runs".
   - `p_lock` is set from held-out precision: a wrong lock is unrecoverable for an exact
     solver on the locked graph, so target ≥ 0.999.
   - Locking replaces the tau schedule `1.5,1.25,1.1` with a single probability threshold.
     If a solve is infeasible, a lower threshold is a fallback, as today.
6. Every stage counts decodes against a fixed cap (as `RP_LOCK_MAX_DECODES` and
   `RP_MATCH_MAX_DECODES` do). Results then don't depend on the core count.

## Solving the most-probable Hamiltonian path

### Reduction

Shortest Hamiltonian path from a fixed start `s` (chunk 0's supernode) = **asymmetric TSP
(ATSP)** on n+1 nodes:
- add a dummy node D;
- add arc D→s with cost 0;
- add arcs v→D with cost 0 for every allowed end `v` (only the final-fragment chunk's
  supernode, if there is one);
- missing arcs are forbidden (no edge, or cost ∞).

### Solver options

| option | exact? | fit here | license / dependency |
|---|---|---|---|
| **AP-based branch & bound** (Carpaneto-Dell'Amico-Toth style), in-house | yes (with time limit → best found) | **First candidate, not yet justified.** The assignment relaxation is already implemented (`globalMatch` SSP). On our data it leaves only 0-3 subtours, so the subtour part of the tree is shallow. But the same AP solutions had 6-12 reservoir breaks and needed 1-22 repair moves (22 on vbrq2_30s 1 KB, 8 on test7_short). **The reservoir cuts, not the subtours, will set the tree depth.** That has to be measured (step 2) before choosing. To branch on a subtour or a no-good, forbid each of its arcs in turn. Re-solve incrementally from the previous potentials. | none |
| **ILP**, DFJ formulation, lazy subtour cuts (HiGHS) | yes | Second check on the B&B and an upper bound on what exact solving costs. The LP bound is ≥ the AP bound. Reservoir no-goods (below) are linear cuts. | HiGHS: MIT, C++ library, CMake-friendly |
| **CP-SAT** `AddCircuit` with optional arcs (OR-Tools) | yes | Native Hamiltonian-circuit propagation, strong LNS for good solutions on 5k nodes. Prototype it in Python first, against dumped instances. | Apache-2.0, but a heavy C++ dependency |
| **LKH-3** (ATSP / SOP) | no (state-of-the-art heuristic) | Seconds on 5k nodes. Use it as an upper bound and a cross-check: if LKH finds a cheaper tour than the exact solver, there's a bug. Called as an external binary with TSPLIB files. | Academic/non-commercial use only. Don't vendor it into the repo. |
| pure SAT / MaxSAT | in principle | **Not recommended.** Hamiltonicity encodings need O(n² log n) or more clauses with weak propagation, and weighted MaxSAT at n≈5k is out of reach. CP-SAT is the "SAT-solver" route that works, because its circuit constraint does the propagation SAT encodings lack. | - |
| Concorde | yes, but symmetric only | Poor fit: ATSP→STSP doubles the nodes and loses the sparsity structure. | academic |

**Recommendation, pending the step-2 measurements** (AP bound vs the optimum, and reservoir
cuts per instance). Implement AP-B&B in `main.cpp`'s neighbourhood (new file
`hampath.{hpp,cpp}`), reusing the SSP code from `globalMatch`. Validate it against HiGHS and
CP-SAT, in a Python harness over dumped instances, on every bench instance it can finish.
Keep LKH-3 as an external sanity check only. Today's repair (`repairOrdering`) remains the
local-search polish when the exact solve hits its time limit.

### The bit reservoir: lazy no-good cuts

The reservoir check can't be written as arc costs. Two properties of
`advanceReservoirFrame` (main.cpp) make it usable as lazy cuts anyway:

- **Monotone in the entry level.** The only level-dependent test is
  `mainDataBegin > level/8`. The `headOffset` fallback credit only adds. So a fuller
  reservoir never turns a pass into a fail, and "fails from every entry level" ⇔ "fails from
  a full reservoir".
- **Exact re-sync after one validated frame.** The level after a frame is
  `mainDataBegin*8 + produced - part23Bits` (capped), which does not depend on the entry
  level. A failure at chunk k therefore depends only on the chunks back to the last frame
  validated before it. That's 2-3 chunks at most bitrates/chunk sizes. It can be more when
  chunks hold no complete frame (320 kbps at 1 KB, where frames are ~1044 bytes), via the
  split-frame side-info path.

So the cut windows below are sound and short. Use them the way lazy subtour constraints are
used:

1. Solve. Run `reservoirBreaks` on the result.
2. For each break at position k, walk back to the shortest window `[i..k]` whose check fails
   when entered with a **full** reservoir (sound by monotonicity). By the re-sync property
   this window ends no earlier than the last validated frame before k, so it is typically 2-4
   chunks. Weak case: a window that starts at chunk 0, because only the pinned start enters
   with level 0. Its cut covers a long prefix and barely prunes. The fallback there is the
   existing breaks-first repair from the best solution found.
3. Add the no-good "not all arcs `seq[i]→…→seq[k]` together":
   - ILP: `Σ x_e ≤ len - 1`;
   - B&B: branch on those arcs;
   - CP-SAT: a `BoolOr` of their negations.
4. Re-solve and repeat.

A window of a single arc `a→b` is one that fails even when `a` is entered with a full
reservoir. Such an arc is infeasible in every context, so delete it from the graph up front.
That's a pre-pass over all arcs (cheap, no decode) worth adding even without the rest of
this plan. Measure how many arcs it removes on CBR.

### Output and confidence

- An exact solve gives a **certificate**: the path is optimal for the model. If it isn't the
  original order, the model is wrong, not the search.
- For the confidence report, give the log-likelihood margin to the second-best path: re-solve
  with the found path forbidden (one no-good over all its arcs). Also report the per-junction
  Sinkhorn marginals. A probability ratio between the best and second-best path is
  interpretable in a way the current `stability gap` is not.
- Keep the provenance line (`Result provenance: ...`). Add `exact-optimal (model)` /
  `best-found (time limit)` and the log-likelihood margin.

## Steps and gates

Each step ends with a measurement that decides whether the next one is worth doing.

0. **Finish the `global-matching` benchmark.** It's running; see Status. It is the baseline
   this plan has to beat: **gm1b/gm2b** = `rp_gm2` = `eac8255`.
1. **Arc dump + model (offline, Python).**
   - Build `pcm_probe --dump-arcs`, then `scripts/fit_edge_model.py` (logistic regression +
     calibration report).
   - **Gate A (signal):** held-out (gen2) log-loss and AUC beat `log1p(median flux)` alone,
     and the reliability curve stays within ±5 points.
   - **No leakage into Gate B.** The three failure cells come from `gen/` (cbr192_10s) and
     from Queen and bohemoan. Score them with **leave-one-file-out** models: the model that
     scores a file was never fit on any chunk size of that file.
2. **Objective check: is the truth the optimum under the new weights?**
   - Add the instance dump (`RP_DUMP_ATSP=path`: TSPLIB ATSP + forbidden arcs + the
     true-order and chunk-0 annotations) here rather than in step 4.
   - Solve every dumped instance with an off-the-shelf optimiser: LKH-3 for a strong upper
     bound and CP-SAT where it proves optimality.
   - **Gate B:** under the new weights, cost(truth) ≤ cost(best path found by the optimiser)
     on the three cells where truth loses today (cbr192_10s 2 KB, Queen 4 KB, bohemoan 4 KB).
     No cell where truth wins today may flip. Beating the old wrong order is not enough, so
     compare against the optimiser.
   - Run this for both the new weights and today's `log1p(flux)` costs. The second run tells
     how many of today's failures are signal failures rather than search failures.
   - Also log, per instance:
     - AP lower bound vs cost(truth): a tight gap means B&B is cheap;
     - the number of lazy reservoir cuts a cut loop needs to reach a reservoir-valid optimum
       (CP-SAT/ILP in the Python harness).

     These two numbers decide step 4's solver choice.
   - **If Gate B fails, stop.** A better solver cannot help, and the next step is more or
     better features, not step 4.
3. **Cascade + probability locking in `rp`** (`RP_PROB=1`).
   - Port the model as a weight table in C++.
   - Measure decodes vs the current lock + match pipeline at equal or better true-arc recall.
4. **Exact solver.**
   - Build AP-B&B with lazy reservoir cuts, behind `RP_SOLVER=bb` (default `match` = today's
     pipeline).
   - Cross-check it against HiGHS / CP-SAT / LKH-3 in `test/solver_check.py`, on the
     instances dumped in step 2.
   - **Gate C:** on every instance both solve, B&B's optimum equals HiGHS's, and LKH-3 never
     finds anything cheaper.
5. **Context fixed-point iteration + second-best margin.**
6. **Full benchmark.**
   - Configs `prob1` (cascade + match) and `prob2` (cascade + B&B), on the same matrix and
     with the same harness (`tmp/scripts/bench_one6.sh` + `parse.py`).
   - Compare against final and gm1b: exact count, wrong-output count, time.
   - **Success criterion:** the wrong-output count goes down without losing exact cells. One
     wrong output is worse than one failure: in a carving setting it is silently wrong.
7. **Docs.** Update CLAUDE.md (pipeline phase 6 and the env table) and add an Outcome section
   to this file.

## Files

- `test/pcm_probe.cpp`: `--dump-arcs`.
- `scripts/fit_edge_model.py` (new; Python, numpy + scikit-learn): fit, calibrate, emit
  `edge_model.inc`.
- `edge_model.{hpp,cpp}` (new): features → log-odds; Sinkhorn marginals.
- `hampath.{hpp,cpp}` (new): ATSP reduction, AP-B&B, lazy reservoir cuts, TSPLIB dump. The
  SSP assignment moves here out of `globalMatch`.
- `main.cpp`: `RP_PROB`, `RP_SOLVER`, `RP_SCORE_ORDER=2`, provenance values.
- `test/solver_check.py` (new): HiGHS (`highspy`) / OR-Tools CP-SAT / LKH-3 on dumped instances.

## Risks

- **Edge independence is false.** Arcs in one row compete. The relative features and
  Sinkhorn address this only partly, so the probabilities may be over-confident on dense
  rows. The held-out reliability curve checks this, separately for CBR and VBR.
- **Distribution shift.** The training audio is one source track (gen/ and gen2/ are the
  same song at different offsets), plus Queen and bohemoan. A model fit on it may not
  transfer. Add at least one more source track before trusting `p_lock`.
- **Silence and repeated audio** make several orders genuinely equally probable. The
  certificate then proves optimality of an arbitrary one of them. The confidence report has
  to show near-zero margin there instead of claiming success.
- **Dense long CBR.** If Gate 2's cheap recall fails, the problem stays out of reach,
  whatever the solver.
- **Exact-solve time.** AP-B&B is exponential in the worst case. Reservoir cuts can add
  depth. There is a hard time limit, after which it falls back to best-found + repair, and
  the label says so.

## Status

- 2026-10-17: plan written. `global-matching` finished at `eac8255` (decode cap, stability
  gap, provenance label); regression.sh and ci_sanity pass.
- 2026-10-18: baseline for this plan is now `844fe4b`: reservoir arc filter + LeftContext +
  min-gain repair. Exact on 49/64 main and 44/56 held-out cells, 8 + 8 wrong outputs, 4-5x
  faster per cell than global matching v2 (paired median; CLAUDE.md Known Limitations item 8).
  - Of the 16 wrong outputs, 7 are objective failures (the wrong order is cheaper). These are
    Gate B's targets.
  - The other 9 are search failures under the exact-context summed flux: the true order is
    cheaper, but more than one repair move away. It is a hypothesis, not a result, that an
    exact solver would fix them, because the solver in this plan optimises median-context arc
    weights, not exact-context flux. Step 2's truth-vs-optimum check answers it.
  - Arc pre-filter: done (it is the "single-arc window" pre-pass of the reservoir section).
