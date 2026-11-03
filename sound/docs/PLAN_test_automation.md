# PLAN - automated corpus tests and test output (2026-10-26)

## Why

Testing on the real corpus (`sound/musiclib/`) has been done by hand: ad-hoc scripts, `grep`
over `rp` logs, tables assembled in chat. The existing tools don't scale to it:

- `test/regression.sh` / `test/measurements.sh` hardcode six fixtures in `test/`.
- `test/bench/` (`bench_one.sh` + `parse.py`) parses ~45 fields out of human-readable log lines
  with regexes, so a reworded log line silently breaks a field.
- CI runs one synthetic file (`test/ci_sanity.sh`); the corpus can't be committed.

## Decisions (agreed with the user)

| question | decision |
|---|---|
| how results leave `rp` | `RP_REPORT=path.json`: `rp` writes one JSON object per run; logs stay for humans |
| result / report format | `results.csv` (one row per run, appendable) + generated `summary.md` |
| where data lives | under `sound/musiclib/`, whole directory gitignored; manifest and derived files in `sound/musiclib/.derived/` |
| pass / fail | diff against a committed baseline of outcome classes |

## Components

### 1. `rp` JSON report (`RP_REPORT=path.json`)

Written on every exit path (success, "no complete ordering", early errors), so a missing file
means the process crashed or was killed. Fields:

- input: `file`, `chunk_size`, `seed`, `env` (all `RP_*` variables); the runner records the binary's hash (a compiled-in commit goes stale between CMake configures)
- stream: `profile` (`mpeg`, `layer`, `sample_rate`, `cbr_kbps` or null, `crc`), `chunks`
- true order: `pre_breaks`, `resv_breaks` (null when not computed)
- graph and search: `edges`, `supernodes`, `locks`, `locks_correct`, `lock_attempts`,
  `matching` (ran / skipped / result), `repair_moves`
- result: `status` (`complete` / `no_ordering` / `error`), `error` text, `exact_pct`,
  `true_junctions`, `junctions`, `exact` (byte-identical), `provenance`
- timing: `wall_s`, per-phase seconds

### 2. `test/corpus.py` (Python 3, standard library only)

Subcommands:

- **`scan`**: recursively finds `*.mp3` under `musiclib/` (any layout, e.g. `Free_Music_Archive/`,
  `Internet_Archive/<item>/`), parses every frame header (MPEG 1/2/2.5, Layers I-III), and writes
  `.derived/manifest.tsv`, one row per unique file (content hash; duplicates listed):
  `id` (short hash), `path`, `source` (top-level folder), `mpeg`, `layer`, `sample_rate`,
  `mode` (CBR/VBR), `kbps` (avg), `crc`, `channels`, `duration_s`, `audio_bytes`, `max_frame`,
  `info_tag`, `trailing_bytes` (after the last frame: ID3v1 / APE / other), `headerless_1k`,
  `headerless_2k`. Cached by size + mtime, so a rescan only reads new files.
- **`derive`**: deterministic test inputs in `.derived/inputs/`, named by source id + recipe:
  - `head<N>`: first N chunks' worth of audio (stream copy; a real start has an empty bit
    reservoir), N chosen per chunk size from a target chunk count (e.g. 100 / 300 / 1000);
  - `strip`: audio frames only (leading and trailing tags removed);
  - `lame-v2-<s>s`: decoded segment re-encoded with LAME V2 (VBR material), optional.
  Mid-track stream-copy cuts are not offered: their first frames' reservoir points before the cut.
- **`run`**: runs a matrix `inputs x chunk sizes x configs x seeds` in parallel (`--jobs`), each
  run in its own working directory with `RP_REPORT` and a timeout. Result row = JSON report +
  manifest properties + `run_id`, `binary` (commit), `config` name, `rc`, `log` path. Resumable:
  a `run_id` (hash of binary, input, chunk size, config, seed) already in `results.csv` is
  skipped. Named configs live in `test/corpus_configs.json` (`default`, `no_pcm`, ...).
- **`report`**: `summary.md` from a results file:
  - outcome class counts by source, format (CBR/VBR x bitrate x sample rate) and chunk size;
  - structural causes (frames > chunk, trailing tag, CRC, other);
  - per-file table of outcome per chunk size.
- **`compare`**: two results files or results vs baseline; lists every (input, chunk, config)
  whose outcome class changed; exit code 1 on a regression; `--update-baseline` rewrites it.

### 3. Outcome classes (assigned by `corpus.py`, not by `rp`)

| class | meaning |
|---|---|
| `EXACT` | output byte-identical to the source |
| `WRONG` | complete ordering, not the source order |
| `SEARCH_FAIL` | no complete ordering, true order structurally valid |
| `STRUCTURAL` | not exact and the true order is broken (`pre_breaks` or `resv_breaks` > 0) |
| `TIMEOUT` | killed by the runner's timeout (no report) |
| `ERROR` | `rp` reported an error itself (`status: error`, e.g. no frames, no profile) |
| `CRASH` | non-zero exit or missing report otherwise |

### 4. Baseline and regression rule

`test/corpus_baseline.tsv` (committed; ids, names and classes only, no audio): one row per
(input id, recipe, chunk size, config, seed) with its class. A **regression** is an `EXACT` that
becomes anything else, a structurally valid run that becomes `STRUCTURAL`, or a new `CRASH` or `ERROR`.
Improvements are listed. The baseline changes only with `--update-baseline`.

### 5. Tiers

| tier | inputs | when |
|---|---|---|
| smoke | `test/ci_sanity.mp3` (committed) | CI, every push (unchanged) |
| fixtures | `test/*.mp3`, `test/gen*/` | locally, replaces `regression.sh` |
| corpus-quick | `head` clips at ~100-300 chunks, 1 and 4 KB | locally, minutes |
| corpus-full | whole tracks, 1-16 KB | locally, hours, resumable |

## Status (2026-10-26)

Stages 1-4 built: `RP_REPORT` (stdout unchanged; JSON on success / no ordering / error),
`corpus.py scan|run|report|compare`, `orig` / `strip` / `headN` recipes. Checks: the manifest
reproduces the hand census of the FMA set (103 unique, 99 CBR / 4 VBR, 16 CRC, 62 with frames
> 1 KB); the fixtures tier gives `regression.sh`'s outcomes; `compare` flags a doctored
EXACT -> SEARCH_FAIL as a regression (exit 1). Not built: the LAME re-encode recipe; retiring
the old scripts (stage 5).

## Stages

1. `.gitignore` the whole `sound/musiclib/`; `rp` JSON report (+ check: identical stdout, report on
   every exit path).
2. `corpus.py scan` + manifest; validate against the hand-made census (103 FMA tracks: 99 CBR /
   4 VBR, 16 CRC, 62 with frames > 1 KB).
3. `run` + `report` on the fixtures tier; compare with `regression.sh`.
4. `derive`, `compare`, baseline; corpus-quick tier.
5. Retire `regression.sh` / `measurements.sh` / `bench/parse.py` once `corpus.py` covers them
   (separate decision).
