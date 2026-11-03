# Benchmark: `full-pcm-decode` vs `sideinfo-mainbits` (2026-10-12)

Spreadsheet with every run: [`BENCHMARK_pcm_vs_main.xlsx`](BENCHMARK_pcm_vs_main.xlsx). The Notes sheet
has the method details, and the summary sheets are formulas over the Runs sheet.
Tooling: `test/bench/` and `test/make_bitrate_variants.sh`.

## TL;DR

- **The PCM branch is better, with no regression found.** Over 64 file × chunk-size cells × 3
  seeds, the final branch reconstructs **103/192 runs byte-exact, against 65/192** for main (with
  the shared last-chunk fix, see below) and 59/192 for main as-is. It never returns a
  complete-but-wrong ordering (0 runs, against 6 for main). On average it recovers 80% of the
  true junctions, against 59%. **There is no cell where main + fix is exact and the PCM branch is
  not**, in the main matrix or in the held-out set.
- **Held-out segment** (a different 30 s / 10 s cut, not used for any tuning): **33/56 exact
  against 25/56**, with 0 wrong orderings against 3.
- **It is not just more time.** Main + fix was given a 150 s budget (5×) on the 20 cells where the
  PCM branch is exact and main + fix is not. It became exact on 1 of them.
- **VBR:** 45/60 exact against 36/60, with 96% of junctions recovered against 84%. The 6-minute VBR
  Bohemian Rhapsody at 8 KB chunks goes from failing to **byte-exact on all seeds (~40-70 s)**. At
  1-4 KB it still fails, but its best partial path is 78-89% correct, against 32-56% for main.
- **CBR:** 58/132 against 29/132. The dense 32 kbps / 6-minute file (`bohemoan`) goes from
  failing to **byte-exact at 8 KB**.
- **Still unsolved on both branches:** long CBR at 1-2 KB chunks, and the 6-minute files below
  8 KB. The signal is strong there (flux puts the true successor first in 60-92% of cases,
  92% even among ~140 candidates on bohemoan at 2 KB). What gives out is the search: one early wrong turn still
  strands it.

## 1. Status of the branch, and does it fully use the decode?

`full-pcm-decode` decodes every candidate junction all the way to PCM with the vendored minimp3:
requantisation, stereo, IMDCT and synthesis, including the frame that straddles the chunk boundary and
the bit-reservoir frames after it. The decode is sample-exact against a full-file decode. From that
audio it measures one thing, the spectral-flux spike at the junction relative to the local flux. It
uses that measure in three places:

1. **Ranking** of DFS candidates (`RP_PCM`).
2. **Confident-edge locking** (`edge_lock`). An edge is forced when its median-context flux beats
   every competitor on both sides by a factor tau, using a schedule of 1.5, 1.25 and 1.1.
3. **Block-move / block-swap repair** of a complete ordering on summed flux.

What it does *not* do with the decoded audio (and why):
- **No hard filter.** True junctions have a long flux tail (p99 ≈ 4-22), which overlaps wrong
  candidates at every threshold.
- **The other metrics** (RMS jump, HF burst, peak, envelope distance) are noise in the probe
  (AUC ≈ 0.5-0.65), so they are measured but not used.
- **No global objective search** (min-cost matching, or a beam over partial orderings on summed
  flux). This is the main missing step. The 2-opt test says summed flux is aligned with the true
  order: the true pair is cheaper in 92-100% of structural swaps on every file except cbr192 at 1 KB (76-80%).

So the decode is used to the extent the measurements justify. The remaining limit is the search,
not the signal.

### What this round finished (commits `aa9ce7c`..`1c5abe1`)

| commit | change | why it was needed |
|---|---|---|
| aa9ce7c | accept a header-less **final fragment** as the last chunk | When the file's last chunk held only the tail of the final frame, the true order had a break. That also silently disabled AC-3 and locking for the whole file. This is shared code, so main is benchmarked with and without it. |
| 1446fe6 | PCM window **byte-capped** (≥ 3 KB / 8 frames before the junction, ≥ 1 KB / 4 frames after); `RP_CHUNK_SIZE`; deadline polled every 64 nodes instead of 2^18 | At 8 KB chunks every candidate decoded ~32 KB, and the post-side max-flux ran over seconds of music. The search budget was being overshot from 45 s to 280 s. 1 KB behaviour is unchanged. |
| ef55e44 | **threaded, memoised locking**; bounded repair (suspect filter + 60 s budget) | Locking was skipped by its decode cap on every long file. The repair ran 17 min 51 s on a dense CBR graph *after* the search had already found the exact order. |
| bb1100d | repair **skipped after a strict-tau success**; two-phase move scan | On cbr32_10s (1 KB) the repair moved the exact order to a wrong one with lower summed flux. The suspect-only scan missed the fixing swap on cbr320_10s (4 KB). |
| f9ad99c | **fixed** lock cap (640k decodes); probe gate 1 reports near-junction exactness | With a thread-scaled cap, the result depended on the core count. |
| 1c5abe1 | a wrong lock onto chunk 0's chain abandons that lock attempt; the failure report keeps the best partial path over all attempts | Previously `rp` exited with rc=1 and a misleading error. |

Regression (`test/regression.sh`) and `test/ci_sanity.sh` pass after every commit. The decode is
still sample-exact near the junction at 4-8 KB chunks: `pcm_probe` gate 1 reports 100% for the last
3 pre-junction frames plus the post frames. The < 100% overall figure at 4-8 KB comes from the capped
window's warm-up frames.

**Tried and reverted:** the "interior" false-sync rule for `computeChunkMeta`
(CLAUDE.md Known Limitations item 5). It fixed vbrq5_30s at 1 KB but added a break on test7 at
1 KB, so the net is zero. That bug is why vbrq2_30s / vbrq5_30s at 1 KB fail on *both*
branches: the true order isn't structurally reachable there.

## 2. Method

- **Files:** 14 generated fixtures (CBR 32/64/128/192/320 kbps and LAME VBR q2/q5, 10 s and 30 s,
  44.1 kHz, no tags) cut at 120 s from the Queen track. Also `bohemoan-rhapsody` (6 min, 32 kbps CBR;
  the 2k/4k/8k files are byte-identical, so it was run once per chunk size) and
  `Queen – Bohemian Rhapsody VBR` (6 min, ~121 kbps VBR). The held-out set is the same 14 settings cut at 200 s.
- **Chunk sizes:** 1, 2, 4 and 8 KB. **Seeds:** 1-3 (ablations, held-out and the equal-time check use seed 1).
- **Configurations:**
  - **main**: `sideinfo-mainbits` as-is.
  - **main + fix**: main plus the shared last-chunk fix.
  - **PCM v1**: 1446fe6.
  - **PCM final**: 1c5abe1 with `RP_THREADS=4`.
- **Budgets:** `RP_TIME_BUDGET=30` everywhere, 900 s hard timeout. The PCM branch can spend up to
  3 × 30 s (the lock schedule) plus locking plus repair; the equal-time check addresses this.
- **Metrics:**
  - **EXACT**: byte-identical output.
  - **complete-but-wrong**: a valid ordering that isn't the original one.
  - **recovered**: true junctions in the result (or in the deepest partial path) divided by
    (chunks − 1).
- **Caveat:** runs shared the machine (12-14 processes), so wall-clock times are only indicative.

## 3. Results

### Exact reconstructions by bitrate mode and chunk size (seeds 1-3)

| mode | chunk | main exact | main+fix exact | PCM v1 exact | PCM final exact | main recovered | main+fix recovered | PCM v1 recovered | PCM final recovered |
|---|---|---|---|---|---|---|---|---|---|
| CBR | 1 KB | 3/33 | 3/33 | 2/33 (+3 wrong) | 5/33 | 29% | 29% | 41% | 46% |
| CBR | 2 KB | 3/33 | 3/33 | 6/33 | 6/33 | 42% | 42% | 58% | 67% |
| CBR | 4 KB | 5/33 (+3 wrong) | 5/33 (+3 wrong) | 20/33 | 20/33 | 50% | 50% | 83% | 86% |
| CBR | 8 KB | 18/33 | 18/33 | 24/33 | 27/33 | 69% | 69% | 92% | 93% |
| **CBR** | **all** | 29/132 (+3 wrong) | 29/132 (+3 wrong) | 52/132 (+3 wrong) | 58/132 | 47% | 47% | 69% | 73% |
| VBR | 1 KB | 0/15 | 6/15 | 6/15 | 6/15 | 69% | 69% | 87% | 90% |
| VBR | 2 KB | 6/15 (+3 wrong) | 6/15 (+3 wrong) | 12/15 | 12/15 | 84% | 84% | 96% | 96% |
| VBR | 4 KB | 12/15 | 12/15 | 12/15 | 12/15 | 91% | 91% | 98% | 98% |
| VBR | 8 KB | 12/15 | 12/15 | 15/15 | 15/15 | 92% | 92% | 100% | 100% |
| **VBR** | **all** | 30/60 (+3 wrong) | 36/60 (+3 wrong) | 45/60 | 45/60 | 84% | 84% | 95% | 96% |
| **all** | **all** | **59/192** (+6 wrong) | **65/192** (+6 wrong) | **97/192** (+3 wrong) | **103/192** | **59%** | **59%** | **77%** | **80%** |

### Per file and chunk size (exact out of 3 seeds; W = complete-but-wrong runs)

| file | chunk | chunks | main | main+fix | PCM v1 | PCM final |
|---|---|---|---|---|---|---|
| cbr32_10s | 1 KB | 40 | 3/3 | 3/3 | 0/3 W3 (90%) | **3/3** |
| cbr32_10s | 2 KB | 20 | 3/3 | 3/3 | 3/3 | **3/3** |
| cbr32_10s | 4 KB | 10 | 2/3 (81%) | 2/3 (81%) | 2/3 (81%) | 2/3 (81%) |
| cbr32_10s | 8 KB | 5 | 3/3 | 3/3 | 3/3 | **3/3** |
| cbr32_30s | 1 KB | 118 | 0/3 (32%) | 0/3 (32%) | 0/3 (1%) | 0/3 (55%) |
| cbr32_30s | 2 KB | 59 | 0/3 (47%) | 0/3 (47%) | 0/3 (7%) | 0/3 (75%) |
| cbr32_30s | 4 KB | 30 | 0/3 W3 (86%) | 0/3 W3 (86%) | 3/3 | **3/3** |
| cbr32_30s | 8 KB | 15 | 3/3 | 3/3 | 3/3 | **3/3** |
| cbr64_10s | 1 KB | 79 | 0/3 (62%) | 0/3 (62%) | 2/3 (94%) | 2/3 (93%) |
| cbr64_10s | 2 KB | 40 | 0/3 (90%) | 0/3 (90%) | 3/3 | **3/3** |
| cbr64_10s | 4 KB | 20 | 3/3 | 3/3 | 3/3 | **3/3** |
| cbr64_10s | 8 KB | 10 | 2/3 (74%) | 2/3 (74%) | 2/3 (81%) | 2/3 (81%) |
| cbr64_30s | 1 KB | 235 | 0/3 (21%) | 0/3 (21%) | 0/3 (45%) | 0/3 (53%) |
| cbr64_30s | 2 KB | 118 | 0/3 (34%) | 0/3 (34%) | 0/3 (59%) | 0/3 (61%) |
| cbr64_30s | 4 KB | 59 | 0/3 (62%) | 0/3 (62%) | 3/3 | **3/3** |
| cbr64_30s | 8 KB | 30 | 3/3 | 3/3 | 3/3 | **3/3** |
| cbr128_10s | 1 KB | 157 | 0/3 (35%) | 0/3 (35%) | 0/3 (66%) | 0/3 (66%) |
| cbr128_10s | 2 KB | 79 | 0/3 (56%) | 0/3 (56%) | 0/3 (75%) | 0/3 (79%) |
| cbr128_10s | 4 KB | 40 | 0/3 (46%) | 0/3 (49%) | 3/3 | **3/3** |
| cbr128_10s | 8 KB | 20 | 3/3 | 3/3 | 3/3 | **3/3** |
| cbr128_30s | 1 KB | 470 | 0/3 (11%) | 0/3 (11%) | 0/3 (43%) | 0/3 (47%) |
| cbr128_30s | 2 KB | 235 | 0/3 (16%) | 0/3 (16%) | 0/3 (33%) | 0/3 (60%) |
| cbr128_30s | 4 KB | 118 | 0/3 (17%) | 0/3 (17%) | 0/3 (72%) | 0/3 (55%) |
| cbr128_30s | 8 KB | 59 | 0/3 (22%) | 0/3 (22%) | 3/3 | **3/3** |
| cbr192_10s | 1 KB | 236 | 0/3 (26%) | 0/3 (26%) | 0/3 (45%) | 0/3 (45%) |
| cbr192_10s | 2 KB | 118 | 0/3 (51%) | 0/3 (51%) | 0/3 (88%) | 0/3 (88%) |
| cbr192_10s | 4 KB | 59 | 0/3 (57%) | 0/3 (57%) | 3/3 | **3/3** |
| cbr192_10s | 8 KB | 30 | 2/3 (80%) | 2/3 (80%) | 2/3 (83%) | 2/3 (83%) |
| cbr192_30s | 1 KB | 705 | 0/3 (20%) | 0/3 (20%) | 0/3 (36%) | 0/3 (36%) |
| cbr192_30s | 2 KB | 353 | 0/3 (20%) | 0/3 (20%) | 0/3 (66%) | 0/3 (64%) |
| cbr192_30s | 4 KB | 177 | 0/3 (22%) | 0/3 (22%) | 0/3 (66%) | 0/3 (68%) |
| cbr192_30s | 8 KB | 89 | 0/3 (35%) | 0/3 (35%) | 3/3 | **3/3** |
| cbr320_10s | 1 KB | 392 | 0/3 (4%) | 0/3 (4%) | 0/3 (4%) | 0/3 (5%) |
| cbr320_10s | 2 KB | 196 | 0/3 (24%) | 0/3 (24%) | 0/3 (59%) | 0/3 (59%) |
| cbr320_10s | 4 KB | 98 | 0/3 (36%) | 0/3 (36%) | 3/3 | **3/3** |
| cbr320_10s | 8 KB | 49 | 2/3 (82%) | 2/3 (82%) | 2/3 (82%) | 2/3 (82%) |
| cbr320_30s | 1 KB | 1174 | 0/3 (1%) | 0/3 (1%) | 0/3 (1%) | 0/3 (1%) |
| cbr320_30s | 2 KB | 587 | 0/3 (19%) | 0/3 (19%) | 0/3 (40%) | 0/3 (42%) |
| cbr320_30s | 4 KB | 294 | 0/3 (25%) | 0/3 (25%) | 0/3 (67%) | 0/3 (67%) |
| cbr320_30s | 8 KB | 147 | 0/3 (42%) | 0/3 (42%) | 0/3 (79%) | 0/3 (79%) |
| vbrq2_10s | 1 KB | 202 | 0/3 (100%) | 3/3 | 3/3 | **3/3** |
| vbrq2_10s | 2 KB | 101 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq2_10s | 4 KB | 51 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq2_10s | 8 KB | 26 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq2_30s | 1 KB | 602 | 0/3 (53%) | 0/3 (53%) | 0/3 (78%) | 0/3 (78%) |
| vbrq2_30s | 2 KB | 301 | 0/3 (79%) | 0/3 (79%) | 3/3 | **3/3** |
| vbrq2_30s | 4 KB | 151 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq2_30s | 8 KB | 76 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq5_10s | 1 KB | 148 | 0/3 (99%) | 3/3 | 3/3 | **3/3** |
| vbrq5_10s | 2 KB | 74 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq5_10s | 4 KB | 37 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq5_10s | 8 KB | 19 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq5_30s | 1 KB | 440 | 0/3 (60%) | 0/3 (60%) | 0/3 (92%) | 0/3 (92%) |
| vbrq5_30s | 2 KB | 220 | 0/3 W3 (98%) | 0/3 W3 (98%) | 3/3 | **3/3** |
| vbrq5_30s | 4 KB | 110 | 3/3 | 3/3 | 3/3 | **3/3** |
| vbrq5_30s | 8 KB | 55 | 3/3 | 3/3 | 3/3 | **3/3** |
| Queen - Bohemian Rhapsody VBR | 1 KB | 5322 | 0/3 (32%) | 0/3 (32%) | 0/3 (64%) | 0/3 (80%) |
| Queen - Bohemian Rhapsody VBR | 2 KB | 2661 | 0/3 (42%) | 0/3 (42%) | 0/3 (79%) | 0/3 (78%) |
| Queen - Bohemian Rhapsody VBR | 4 KB | 1331 | 0/3 (56%) | 0/3 (56%) | 0/3 (89%) | 0/3 (89%) |
| Queen - Bohemian Rhapsody VBR | 8 KB | 666 | 0/3 (61%) | 0/3 (61%) | 3/3 | **3/3** |
| bohemoan-rhapsody-2k | 1 KB | 1405 | 0/3 (5%) | 0/3 (5%) | 0/3 (3%) | 0/3 (3%) |
| bohemoan-rhapsody-2k | 2 KB | 703 | 0/3 (9%) | 0/3 (9%) | 0/3 (7%) | 0/3 (7%) |
| bohemoan-rhapsody-2k | 4 KB | 352 | 0/3 (16%) | 0/3 (16%) | 0/3 (29%) | 0/3 (70%) |
| bohemoan-rhapsody-2k | 8 KB | 176 | 0/3 (19%) | 0/3 (19%) | 0/3 (81%) | **3/3** |

(percent in brackets: mean recovered true junctions when not all seeds were exact)

### Held-out segment (start 200 s, seed 1)

| configuration | exact | complete-but-wrong | failed | mean recovered |
|---|---|---|---|---|
| main+fix | 25/56 | 3 | 28 | 70% |
| PCM final | 33/56 | 0 | 23 | 83% |

Cells where only PCM final is exact: 8; only main+fix: 0.

### Equal-time check (main+fix with a 150 s budget, seed 1)

| file | chunk | main+fix 30 s | main+fix 150 s | PCM final | PCM final wall s |
|---|---|---|---|---|---|
| cbr32_10s (held-out) | 1 KB | FAILED | COMPLETE_WRONG | EXACT | 12.48 |
| cbr32_30s (held-out) | 4 KB | COMPLETE_WRONG | COMPLETE_WRONG | EXACT | 6.04 |
| cbr64_30s (held-out) | 8 KB | COMPLETE_WRONG | COMPLETE_WRONG | EXACT | 4.78 |
| cbr128_10s (held-out) | 2 KB | FAILED | FAILED | EXACT | 15.19 |
| cbr128_10s (held-out) | 4 KB | COMPLETE_WRONG | COMPLETE_WRONG | EXACT | 1.92 |
| cbr192_30s (held-out) | 8 KB | FAILED | FAILED | EXACT | 23.44 |
| cbr320_10s (held-out) | 4 KB | FAILED | FAILED | EXACT | 1.55 |
| cbr320_30s (held-out) | 8 KB | FAILED | FAILED | EXACT | 79.1 |
| cbr32_30s | 4 KB | COMPLETE_WRONG | COMPLETE_WRONG | EXACT | 9.64 |
| cbr64_10s | 2 KB | FAILED | EXACT | EXACT | 16.99 |
| cbr64_30s | 4 KB | FAILED | FAILED | EXACT | 94.06 |
| cbr128_10s | 4 KB | FAILED | FAILED | EXACT | 5.73 |
| cbr128_30s | 8 KB | FAILED | FAILED | EXACT | 75.72 |
| cbr192_10s | 4 KB | FAILED | FAILED | EXACT | 7.44 |
| cbr192_30s | 8 KB | FAILED | FAILED | EXACT | 38.77 |
| cbr320_10s | 4 KB | FAILED | FAILED | EXACT | 43.69 |
| vbrq2_30s | 2 KB | FAILED | FAILED | EXACT | 1.68 |
| vbrq5_30s | 2 KB | COMPLETE_WRONG | COMPLETE_WRONG | EXACT | 1.78 |
| Queen VBR (6 min) | 8 KB | FAILED | FAILED | EXACT | 39.62 |
| bohemoan-rhapsody-2k | 8 KB | FAILED | FAILED | EXACT | 245.06 |

### Which part does the work? Ablations (seed 1)

| configuration | exact | complete-but-wrong | failed | mean recovered |
|---|---|---|---|---|
| all on | 35/64 | 0 | 29 | 80% |
| RP_PCM=0 | 29/64 | 3 | 32 | 74% |
| RP_LOCK=0 | 27/64 | 1 | 36 | 73% |
| RP_REPAIR=0 | 34/64 | 1 | 29 | 80% |
| RP_LOCK_TAU=1.5 | 25/64 | 1 | 38 | 73% |
| main+fix (reference) | 23/64 | 2 | 39 | 61% |

- **Each component adds something:**
  - Disabling the PCM term costs 6 exact cells (35 → 29), and 3 cells become complete-but-wrong.
  - Disabling locking costs 8.
  - Locking at tau 1.5 only, without the 1.25 / 1.1 fallback, costs 10.
- **The repair rarely changes an outcome now:** 34 against 35 without it. The skip-after-strict-success rule
  removed the one case where it did harm.
- **Exact PCM-final runs by the tau of their last lock attempt:** 75 at 1.5, 12 at 1.25, 16 at 1.1.
- **The fallback carries real risk.** 36 of the 71 PCM-final runs that failed at the tau 1.1 attempt had
  1-12 wrong locks, which makes the true order unreachable. None of the 16 exact tau-1.1 runs had a wrong
  lock, and no run produced a wrong output.

## 4. How it changes variable-bitrate reconstruction

- **Short VBR at ≥ 4 KB is trivial for both branches.** The structure alone leaves 0-36 branch points,
  and both are exact. The real comparison is at 1-2 KB and on the 6-minute file.
- **VBR at 2 KB: 12/15 against 6/15 exact.** vbrq2_30s and vbrq5_30s become exact. Main fails on
  them, or (vbrq5_30s) returns a 98%-correct but wrong ordering.
- **VBR at 1 KB: 6/15 on both branches; recovered 90% against 69%.** The 30 s VBR files at 1 KB fail
  on both branches because of the shared `computeChunkMeta` false-sync break (above), not because of
  the signal.
- **6-minute VBR (5,322 chunks at 1 KB):**
  - 8 KB: **exact on all seeds** (main: fails at 61% recovered).
  - 1-4 KB: still fails, but the best partial path is 78-89% correct against 32-56%.
  - Its true-successor rank-1 is 86-98%, against 13-43% for Milestone 2.
- **The VBR signal is the strongest of all:** flux AUC is 0.85-1.0 and rank-1 86-100%. Locking at
  tau 1.5 covers 17-70% of branch-point edges on the 10/30 s files (1-56% on Queen). It has 0 wrong
  locks except vbrq5_10s at 1 KB (1), and Queen at 4 and 8 KB (2 and 1).

## 5. Signal strength by bitrate and chunk size (`pcm_probe`, true order)

- `branch points`: junctions with ≥ 2 structural candidates.
- `random mean rank`: the expected rank of a random pick, so (number of candidates + 1) / 2.
- `M2`: Milestone 2, the partial-spectrum signal main uses.
- Rows with 0 branch points are omitted.
- The `bohemoan` 1 KB probe did not finish within 90 min, so that row is missing.

### Per-edge signal on the true order (pcm_probe): rank-1 of the true successor among structural candidates

| file | chunk | branch points | candidates/junction (random mean rank) | flux AUC | flux rank-1 | M2 rank-1 | 2-opt true cheaper | tau1.5 locks wrong / coverage |
|---|---|---|---|---|---|---|---|---|
| cbr32_10s | 1 KB | 38 | 4.5 | 0.814 | 57.9% | 37.1% | 99.2% | 0 / 2.6% |
| cbr32_10s | 2 KB | 18 | 2.5 | 0.776 | 61.1% | 29.4% | 100.0% | 0 / 0.0% |
| cbr32_10s | 4 KB | 8 | 1.5 | 0.828 | 75.0% | 71.4% | 100.0% | 0 / 33.3% |
| cbr32_30s | 1 KB | 116 | 12.3 | 0.87 | 59.5% | 23.6% | 99.6% | 0 / 0.0% |
| cbr32_30s | 2 KB | 57 | 6.4 | 0.852 | 61.4% | 29.6% | 99.3% | 0 / 1.7% |
| cbr32_30s | 4 KB | 28 | 3.5 | 0.857 | 67.9% | 48.0% | 100.0% | 0 / 6.9% |
| cbr32_30s | 8 KB | 13 | 2.0 | 0.817 | 84.6% | 66.7% | 100.0% | 0 / 28.6% |
| cbr64_10s | 1 KB | 77 | 4.45 | 0.85 | 76.6% | 19.7% | 98.8% | 0 / 2.6% |
| cbr64_10s | 2 KB | 38 | 4.5 | 0.836 | 65.8% | 28.6% | 100.0% | 0 / 5.1% |
| cbr64_10s | 4 KB | 18 | 2.5 | 0.853 | 83.3% | 35.3% | 100.0% | 0 / 15.8% |
| cbr64_10s | 8 KB | 8 | 1.5 | 0.891 | 100.0% | 57.1% | 100.0% | 0 / 44.4% |
| cbr64_30s | 1 KB | 233 | 12.25 | 0.909 | 70.0% | 9.4% | 99.7% | 0 / 0.9% |
| cbr64_30s | 2 KB | 116 | 12.3 | 0.9 | 65.5% | 16.4% | 99.8% | 0 / 0.9% |
| cbr64_30s | 4 KB | 57 | 6.4 | 0.907 | 82.5% | 25.9% | 100.0% | 0 / 3.4% |
| cbr64_30s | 8 KB | 28 | 3.5 | 0.904 | 92.9% | 28.0% | 100.0% | 0 / 10.3% |
| cbr128_10s | 1 KB | 155 | 4.46 | 0.888 | 80.6% | 25.0% | 99.6% | 0 / 0.6% |
| cbr128_10s | 2 KB | 77 | 4.45 | 0.842 | 77.9% | 33.8% | 98.5% | 0 / 3.8% |
| cbr128_10s | 4 KB | 38 | 4.5 | 0.854 | 81.6% | 37.1% | 99.2% | 0 / 7.7% |
| cbr128_10s | 8 KB | 18 | 2.5 | 0.863 | 88.9% | 47.1% | 100.0% | 0 / 15.8% |
| cbr128_30s | 1 KB | 467 | 12.25 | 0.93 | 81.8% | 13.7% | 99.7% | 0 / 0.0% |
| cbr128_30s | 2 KB | 233 | 12.25 | 0.906 | 82.0% | 20.9% | 99.6% | 0 / 1.3% |
| cbr128_30s | 4 KB | 116 | 12.3 | 0.903 | 81.9% | 27.5% | 99.9% | 0 / 2.6% |
| cbr128_30s | 8 KB | 57 | 6.4 | 0.907 | 87.7% | 29.6% | 100.0% | 0 / 3.4% |
| cbr192_10s | 1 KB | 232 | 4.42 | 0.756 | 58.0% | 33.8% | 76.2% | 0 / 0.0% |
| cbr192_10s | 2 KB | 116 | 4.44 | 0.859 | 75.0% | 36.5% | 98.0% | 0 / 2.6% |
| cbr192_10s | 4 KB | 57 | 2.47 | 0.845 | 77.2% | 41.2% | 97.5% | 0 / 10.3% |
| cbr192_10s | 8 KB | 28 | 1.5 | 0.835 | 92.9% | 63.2% | 92.3% | 0 / 31.0% |
| cbr192_30s | 1 KB | 701 | 12.24 | 0.787 | 60.0% | 20.2% | 79.6% | 0 / 0.0% |
| cbr192_30s | 2 KB | 350 | 12.24 | 0.911 | 80.9% | 27.2% | 99.4% | 0 / 0.3% |
| cbr192_30s | 4 KB | 174 | 6.37 | 0.895 | 81.0% | 31.1% | 98.7% | 0 / 1.1% |
| cbr192_30s | 8 KB | 86 | 3.44 | 0.878 | 84.9% | 43.6% | 97.6% | 0 / 3.4% |
| cbr320_10s | 1 KB | 375 | 0.0 | 0.5 | 0.0% | 0.0% | 0.0% | 0 / 0.0% |
| cbr320_10s | 2 KB | 194 | 4.45 | 0.89 | 84.5% | 45.0% | 99.1% | 0 / 0.5% |
| cbr320_10s | 4 KB | 96 | 2.47 | 0.836 | 80.2% | 64.4% | 100.0% | 0 / 10.3% |
| cbr320_10s | 8 KB | 46 | 1.5 | 0.845 | 91.3% | 81.8% | 100.0% | 0 / 31.9% |
| cbr320_30s | 1 KB | 1124 | 0.0 | 0.5 | 0.0% | 0.0% | 0.0% | 0 / 0.0% |
| cbr320_30s | 2 KB | 585 | 12.26 | 0.926 | 82.4% | 39.6% | 99.7% | 0 / 0.0% |
| cbr320_30s | 4 KB | 292 | 6.39 | 0.901 | 81.5% | 46.8% | 99.9% | 0 / 3.4% |
| cbr320_30s | 8 KB | 145 | 3.45 | 0.911 | 85.5% | 60.4% | 100.0% | 0 / 6.8% |
| vbrq2_10s | 1 KB | 43 | 1.65 | 0.865 | 88.4% | 56.5% | 96.0% | 0 / 17.4% |
| vbrq2_10s | 2 KB | 5 | 1.5 | 1.0 | 100.0% | 40.0% | 100.0% | 0 / 50.0% |
| vbrq2_30s | 1 KB | 403 | 1.79 | 0.899 | 87.1% | 51.5% | 98.6% | 0 / 22.2% |
| vbrq2_30s | 2 KB | 124 | 1.66 | 0.939 | 98.4% | 55.1% | 100.0% | 0 / 59.9% |
| vbrq2_30s | 4 KB | 36 | 1.54 | 0.922 | 100.0% | 53.3% | 100.0% | 0 / 69.0% |
| vbrq2_30s | 8 KB | 6 | 1.5 | 0.917 | 100.0% | 75.0% | 100.0% | 0 / 70.0% |
| vbrq5_10s | 1 KB | 38 | 1.54 | 0.894 | 94.7% | 51.5% | 100.0% | 1 / 41.9% |
| vbrq5_10s | 2 KB | 9 | 1.5 | 1.0 | 100.0% | 66.7% | 100.0% | 0 / 66.7% |
| vbrq5_30s | 1 KB | 301 | 1.78 | 0.947 | 98.0% | 49.1% | 100.0% | 0 / 35.9% |
| vbrq5_30s | 2 KB | 86 | 1.58 | 0.927 | 96.5% | 56.4% | 97.8% | 0 / 55.6% |
| vbrq5_30s | 4 KB | 23 | 1.5 | 0.893 | 91.3% | 47.6% | 100.0% | 0 / 52.2% |
| vbrq5_30s | 8 KB | 10 | 1.5 | 0.9 | 90.0% | 66.7% | 100.0% | 0 / 60.0% |
| CBR | 1 KB | 656 | 16.98 | 0.795 | 46.6% | 11.1% | 97.4% | 0 / 0.8% |
| CBR | 2 KB | 327 | 16.95 | 0.736 | 30.9% | 11.6% | 96.8% | 0 / 6.1% |
| CBR | 4 KB | 163 | 16.9 | 0.744 | 32.1% | 10.3% | 97.3% | 0 / 8.5% |
| CBR | 8 KB | 81 | 8.71 | 0.748 | 41.2% | 19.0% | 96.7% | 0 / 11.0% |
| Queen VBR (6 min) | 1 KB | 5320 | 7.78 | 0.953 | 86.4% | 12.7% | 99.8% | 0 / 1.0% |
| Queen VBR (6 min) | 2 KB | 2656 | 4.38 | 0.953 | 92.1% | 22.3% | 100.0% | 0 / 29.4% |
| Queen VBR (6 min) | 4 KB | 1289 | 2.71 | 0.953 | 95.6% | 36.2% | 99.9% | 2 / 43.6% |
| Queen VBR (6 min) | 8 KB | 553 | 2.03 | 0.952 | 97.5% | 42.9% | 99.8% | 1 / 55.7% |
| bohemoan-rhapsody-2k | 2 KB | 400 | 70.7 | 0.988 | 91.7% | 2.6% | 100.0% | 0 / 10.1% |
| bohemoan-rhapsody-2k | 4 KB | 350 | 35.6 | 0.989 | 93.9% | 5.0% | 99.9% | 1 / 17.4% |
| bohemoan-rhapsody-2k | 8 KB | 174 | 18.0 | 0.992 | 96.5% | 7.0% | 100.0% | 0 / 26.3% |
| sample-3s | 1 KB | 49 | 1.82 | 0.81 | 89.8% | 43.5% | 100.0% | 0 / 22.0% |
| sample-3s | 2 KB | 24 | 1.83 | 0.792 | 83.3% | 39.1% | 100.0% | 0 / 28.0% |
| sample-3s | 4 KB | 11 | 1.82 | 0.823 | 90.9% | 54.5% | 100.0% | 1 / 33.3% |
| sample-3s | 8 KB | 2 | 1.5 | 1.0 | 100.0% | 100.0% | 0.0% | 0 / 33.3% |
| test7_short | 1 KB | 488 | 1.99 | 0.888 | 89.7% | 45.9% | 99.0% | 0 / 33.3% |
| test7_short | 2 KB | 164 | 1.76 | 0.86 | 92.0% | 42.5% | 99.0% | 0 / 53.8% |
| test7_short | 4 KB | 48 | 1.6 | 0.845 | 89.6% | 46.9% | 100.0% | 0 / 49.0% |
| test7_short | 8 KB | 19 | 1.5 | 0.902 | 89.5% | 46.2% | 100.0% | 0 / 50.0% |
| test7_shorter | 1 KB | 187 | 1.7 | 0.868 | 91.8% | 51.1% | 98.2% | 0 / 46.8% |
| test7_shorter | 2 KB | 51 | 1.62 | 0.85 | 90.2% | 55.6% | 100.0% | 0 / 58.9% |
| test7_shorter | 4 KB | 13 | 1.5 | 0.888 | 100.0% | 22.2% | 100.0% | 0 / 71.4% |
| test7_shorter | 8 KB | 1 | 1.5 | 1.0 | 100.0% | 0.0% | 0.0% | 0 / 100.0% |

Flux rank-1 is ≥ M2 in every row (ties only where there are ≤ 2 branch points, or where the
structure is broken, as with cbr320 at 1 KB). The weak
spots are 192 kbps at 1 KB, where 627-byte frames leave the window with only 1-2 post-junction
frames (rank-1 58-60%, 2-opt 76-80%), and the old CBR.mp3 fixture (rank-1 31-47%).

## 6. Negative or neutral results

- **Wall-clock time:** where neither branch completes, the PCM branch uses its whole schedule
  (3 × 30 s plus locking plus repair, ~90-120 s against 30 s). On the 6-minute files at 1-2 KB,
  locking is also expensive (~0.6 M decodes per round).
- **Partial accuracy on the densest graphs is not better:** on bohemoan (32 kbps CBR, ~140
  candidates per junction) at 1-2 KB, both branches recover only 3-9% of the junctions.
  Locking covers ≤ 10% there, and the search strands early.
- **Long CBR at 1-2 KB still fails, as does cbr320_30s at every chunk size** (79% recovered at 8 KB).
- **The tau 1.1 fallback** makes wrong locks routine on CBR. They didn't lead to wrong output in
  this benchmark (the search then fails instead), but the margin is thin. Use `RP_LOCK_TAU=1.5`
  where an honest failure is preferable.
- **320 kbps at 1 KB is out of scope** for both branches: the frames are longer than a chunk.

## 7. Recommendation and next steps

Merge `full-pcm-decode` into `sideinfo-mainbits`. It is better or equal on every cell tested,
including the held-out set, and never worse. Next:
1. **Global search on summed flux** (min-cost successor matching, or a beam over partial orderings).
   This is the remaining bottleneck on long CBR and on the 6-minute files at 1-4 KB.
2. **A sharper or wider window for high-bitrate small chunks** (192-320 kbps at 1-2 KB). For example,
   include the candidate's second chunk.
3. **Fix the `computeChunkMeta` false-sync tail properly** (item 5). It blocks vbrq2/vbrq5_30s at
   1 KB on both branches.
4. **Make locking cheaper on 6-minute files**, for example by scoring only chunks with few predecessors first.
