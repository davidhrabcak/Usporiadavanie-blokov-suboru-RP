#!/usr/bin/env python3
"""Tests for rp. Python 3 standard library only. Run from sound/.

  test/corpus.py fixtures               the files in test/ (whole files, 1 KB chunks, 20 s budget)
  test/corpus.py musiclib               the musiclib/ corpus (whole files, 1 and 4 KB chunks)
  test/corpus.py score REPORT.json      the metrics of one rp report (CI: --expect-exact)

A suite (test/corpus_suites.json) runs rp on every input, resumes where it stopped (results are
kept per suite and binary), and writes RESULTS.csv + RESULTS.md (the summary).
Comparing two runs is done by hand on those files. RP_* variables set when calling are passed to
rp (an A/B run gets its own results file). Options: --match SUBSTRING..., --jobs N.

The metrics depend only on the order rp outputs, not on how it found it or on the file format:
rp's RP_REPORT JSON carries `status`, `n` (chunks in the stream) and `order` (the source index of
each output chunk; for a failed run the deepest partial path), and everything is computed here.
For a partial path the denominators stay n, so missing chunks count as wrong.

  exact             the order is the source order (output byte-identical to the source)
  neighbor_acc      output neighbours (k-1, k) that are true neighbours, / (n-1)
  position_acc      chunks at their source position, / n
  kendall_tau       rank correlation of the placed chunks with the source order (+1 = in order)
  longest_run_frac  longest stretch of true neighbours, in chunks, / n
  start_correct     the first output chunk is the stream's first chunk
  coverage          placed chunks / n (1 for a complete order)
  budget_exhausted  the search stopped on its time or node budget
  wall_s            rp's wall-clock time (load to exit; not CPU time, so threads don't add up)

Derived files (manifest, cut inputs, logs, full reports with the order, results) go to the
gitignored musiclib/.derived/.
"""
import argparse, concurrent.futures as cf, csv, gzip, hashlib, json, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SOUND = os.path.dirname(HERE)
MUSICLIB = os.path.join(SOUND, "musiclib")
SUITES = os.path.join(HERE, "corpus_suites.json")
THREADS = 3   # RP_THREADS per run
TIMEOUT = 900 # seconds per run

# STRUCTURAL = a complete order whose blocks are right but placed wrong: neighbor_acc >= this.
# Set 2026-11-09 from the 143 complete-but-wrong runs (musiclib whole files at 1 and 4 KB +
# fixtures): neighbor_acc rises smoothly from 0.06 to 0.999 with one small break between 0.969 and
# 0.980; position_acc separates nothing (0-0.9 among the >= 0.98 runs, set by where the misplaced
# block sits), so it is not used. Changing this reclassifies existing results (the class is
# recomputed from the metric columns).
STRUCTURAL_NEIGHBOR_MIN = 0.98


def derived_dir(corpus):
    d = os.path.join(MUSICLIB, ".derived")
    return d if corpus == MUSICLIB else os.path.join(d, "corpus-" + os.path.basename(corpus))

# --------------------------------------------------------------------------------------------
# Metrics


def inversions(a):
    """Number of pairs i < j with a[i] > a[j] (merge sort, O(n log n))."""
    if len(a) < 2:
        return a, 0
    m = len(a) // 2
    l, x = inversions(a[:m])
    r, y = inversions(a[m:])
    out, inv, i, j = [], x + y, 0, 0
    while i < len(l) and j < len(r):
        if l[i] <= r[j]:
            out.append(l[i]); i += 1
        else:
            out.append(r[j]); j += 1; inv += len(l) - i
    return out + l[i:] + r[j:], inv


def metrics(n, order, complete):
    """All metrics of one run from its order (source indices, possibly a partial path)."""
    if not n or not order:
        return {}
    pairs = sum(1 for k in range(1, len(order)) if order[k] == order[k - 1] + 1)
    run = best = 1
    for k in range(1, len(order)):
        run = run + 1 if order[k] == order[k - 1] + 1 else 1
        best = max(best, run)
    m = len(order)
    tau = None if m < 2 else 1 - 4 * inversions(list(order))[1] / (m * (m - 1))
    return dict(
        exact=complete and list(order) == list(range(n)),
        neighbor_acc=pairs / max(1, n - 1),
        position_acc=sum(1 for k, c in enumerate(order) if c == k) / n,
        kendall_tau=tau,
        longest_run_frac=best / n,
        start_correct=order[0] == 0,
        coverage=m / n)


METRIC_COLS = ["exact", "neighbor_acc", "position_acc", "kendall_tau", "longest_run_frac",
               "start_correct", "coverage", "budget_exhausted", "wall_s"]


def classify(r):
    """Outcome class from a row's columns (strings, as read from a CSV)."""
    if r.get("timed_out") == "1":
        return "TIMEOUT"
    if r.get("status", "") == "":
        return "CRASH"          # rp wrote no report
    if r["status"] == "error":
        return "ERROR"
    if r["status"] != "complete":
        return "SEARCH_FAIL"    # its metrics describe the deepest partial path
    if r["exact"] == "true":
        return "EXACT"
    if float(r["neighbor_acc"]) >= STRUCTURAL_NEIGHBOR_MIN:
        return "STRUCTURAL"
    return "WRONG"


def cell(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return f"{v:.4f}"
    return "" if v is None else v

# --------------------------------------------------------------------------------------------
# MPEG audio frames: the manifest's format columns and the secN cut. Other formats need their
# own describe() and cut().

BITRATES = {  # (mpeg1?, layer) -> kbps by index
    (True, 1): [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
    (True, 2): [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384],
    (True, 3): [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    (False, 1): [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256],
    (False, 2): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
    (False, 3): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
}
SAMPLE_RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}


def parse_header(d, i):
    """Frame header at d[i:i+4] -> dict, or None when it isn't a valid header."""
    if i + 4 > len(d) or d[i] != 0xFF or (d[i + 1] & 0xE0) != 0xE0:
        return None
    ver, lay = (d[i + 1] >> 3) & 3, (d[i + 1] >> 1) & 3
    bi, si = d[i + 2] >> 4, (d[i + 2] >> 2) & 3
    if ver == 1 or lay == 0 or bi in (0, 15) or si == 3:
        return None
    layer = 4 - lay
    kbps, sr, pad = BITRATES[(ver == 3, layer)][bi], SAMPLE_RATES[ver][si], (d[i + 2] >> 1) & 1
    if layer == 1:
        length = (12000 * kbps // sr + pad) * 4
    elif layer == 3 and ver != 3:
        length = 72000 * kbps // sr + pad
    else:
        length = 144000 * kbps // sr + pad
    spf = 384 if layer == 1 else (1152 if layer == 2 or ver == 3 else 576)
    return dict(ver=ver, layer=layer, kbps=kbps, sr=sr, len=length, spf=spf)


def same_stream(a, b):
    return a["ver"] == b["ver"] and a["layer"] == b["layer"] and a["sr"] == b["sr"]


def frames_of(d):
    """[(offset, header)...] of the main frame run (after any ID3v2 tag)."""
    i = 0
    if d[:3] == b"ID3" and len(d) >= 10:
        i = 10 + ((d[6] & 0x7F) << 21 | (d[7] & 0x7F) << 14 | (d[8] & 0x7F) << 7 | (d[9] & 0x7F))
    while i + 4 <= len(d):          # first header followed by a second matching one
        h = parse_header(d, i)
        if h:
            h2 = parse_header(d, i + h["len"])
            if h2 and same_stream(h, h2):
                break
        i += 1
    else:
        return []
    frames = []
    while True:
        h = parse_header(d, i)
        if not h or (frames and not same_stream(h, frames[0][1])) or i + h["len"] > len(d):
            return frames
        frames.append((i, h))
        i += h["len"]


def describe(d):
    frames = frames_of(d)
    if not frames:
        return dict(format="mp3", frames=0)
    off, h0 = frames[0]
    tag = any(t in d[off + 4: off + 46] for t in (b"Xing", b"Info", b"VBRI"))
    audio = frames[1:] if tag and len(frames) > 1 else frames
    rates = {h["kbps"] for _, h in audio}
    return dict(format="mp3", frames=len(frames), mode="CBR" if len(rates) == 1 else "VBR",
                kbps=round(sum(h["kbps"] for _, h in audio) / max(1, len(audio))), sample_rate=h0["sr"],
                duration_s=round(sum(h["spf"] for _, h in frames) / h0["sr"], 1))


def cut_seconds(d, secs):
    """The first `secs` seconds of whole frames, from the first frame (empty bit reservoir).
    Mid-track stream-copy cuts are not offered: their first frames' reservoir points before
    the cut."""
    frames = frames_of(d)
    first, end, samples = frames[0][0], frames[0][0], 0
    for o, h in frames:
        if samples >= secs * h["sr"]:
            break
        end, samples = o + h["len"], samples + h["spf"]
    return d[first:end]

# --------------------------------------------------------------------------------------------
# Inputs


MANIFEST_COLS = ["id", "path", "size", "mtime", "format", "frames", "mode", "kbps", "sample_rate", "duration_s"]


def manifest(corpus, derived):
    """One row per distinct file content under `corpus` (cached by size + mtime)."""
    path = os.path.join(derived, "manifest.tsv")
    old = {}
    if os.path.exists(path):
        with open(path) as f:
            rd = csv.DictReader(f, delimiter="\t")
            if rd.fieldnames == MANIFEST_COLS:   # a manifest of another version is rebuilt
                old = {r["path"]: r for r in rd}
    rows, seen = [], set()
    for root, dirs, files in os.walk(corpus):
        dirs[:] = sorted(x for x in dirs if not x.startswith(".") and os.path.join(root, x) != MUSICLIB)
        for name in sorted(files):
            if not name.lower().endswith(".mp3"):
                continue
            p = os.path.join(root, name)
            rel, st = os.path.relpath(p, corpus), os.stat(p)
            o = old.get(rel)
            if o and o["size"] == str(st.st_size) and o["mtime"] == str(int(st.st_mtime)):
                row = o
            else:
                d = open(p, "rb").read()
                row = dict(id=hashlib.sha1(d).hexdigest()[:12], path=rel, size=st.st_size,
                           mtime=int(st.st_mtime), **describe(d))
            if row["id"] not in seen:
                seen.add(row["id"])
                rows.append(row)
    os.makedirs(derived, exist_ok=True)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, MANIFEST_COLS, delimiter="\t", extrasaction="ignore")
        w.writeheader()
        w.writerows(rows)
    return [r for r in rows if str(r.get("frames")) != "0"]


def materialize(corpus, derived, row, recipe):
    """recipe: `orig` (the file as it is) or `secN` (its first N seconds)."""
    src = os.path.join(corpus, row["path"])
    if recipe == "orig":
        return src
    if not recipe.startswith("sec"):
        sys.exit("unknown recipe " + recipe)
    out = os.path.join(derived, "inputs", f"{row['id']}_{recipe}.mp3")
    if not os.path.exists(out):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        with open(out + ".tmp", "wb") as f:
            f.write(cut_seconds(open(src, "rb").read(), int(recipe[3:])))
        os.replace(out + ".tmp", out)
    return out

# --------------------------------------------------------------------------------------------
# Running a suite


RESULT_COLS = ["run_id", "class", "binary", "suite", "chunk_size", "id", "recipe", "name", "format", "mode",
               "kbps", "sample_rate", "status", "error", "rc", "timed_out", "n"] + METRIC_COLS + ["log"]


def run_rp(rp, inp, env):
    """Runs rp on `inp` in a scratch directory -> (rc, timed_out, log bytes, report dict)."""
    work = tempfile.mkdtemp()
    try:
        env = dict({k: v for k, v in os.environ.items() if not k.startswith("RP_")}, **env,
                   RP_REPORT=os.path.join(work, "report.json"))
        try:
            p = subprocess.run([rp, inp], cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               timeout=TIMEOUT)
            rc, out, timed_out = p.returncode, p.stdout, False
        except subprocess.TimeoutExpired as e:
            rc, out, timed_out = -9, e.stdout or b"", True
        try:
            rep = json.load(open(env["RP_REPORT"]))
        except (OSError, ValueError):
            rep = {}
        return rc, timed_out, out, rep
    finally:
        shutil.rmtree(work, ignore_errors=True)


def row_of(rep):
    """The status and metric columns of one rp report."""
    r = dict(status=rep.get("status", ""), error=rep.get("error", ""), n=rep.get("n", ""),
             budget_exhausted=rep.get("budget_exhausted"), wall_s=rep.get("wall_s"))
    r.update(metrics(rep.get("n"), rep.get("order"), rep.get("status") == "complete"))
    return {k: cell(v) for k, v in r.items()}


def one_run(job):
    rp, binary, suite, env, chunk, inp, row, recipe, derived, run_id = job
    t0 = time.time()
    rc, timed_out, out, rep = run_rp(rp, inp, dict(env, RP_SEED="1", RP_CHUNK_SIZE=str(chunk), RP_THREADS=str(THREADS)))
    log = os.path.join(derived, "logs", run_id + ".log.gz")   # the run's stdout
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with gzip.open(log, "wb") as f:
        f.write(out)
    if rep:   # the full report (with the order), to recompute or extend the metrics later
        os.makedirs(os.path.join(derived, "reports"), exist_ok=True)
        with gzip.open(os.path.join(derived, "reports", run_id + ".json.gz"), "wt") as f:
            json.dump(rep, f)
    r = {c: "" for c in RESULT_COLS}
    r.update(run_id=run_id, binary=binary, suite=suite, chunk_size=chunk, id=row["id"], recipe=recipe,
             name=row["path"], rc=rc, timed_out=int(timed_out), log=os.path.relpath(log, derived),
             **{k: row.get(k, "") for k in ("format", "mode", "kbps", "sample_rate")})
    r.update(row_of(rep))
    if not r["wall_s"]:   # no report: time the process from outside
        r["wall_s"] = f"{time.time() - t0:.3f}"
    r["class"] = classify({k: str(v) for k, v in r.items()})
    return r


def cmd_suite(args):
    suite = json.load(open(SUITES))[args.suite]
    corpus = os.path.join(SOUND, suite["corpus"])
    derived = derived_dir(corpus)
    rows = all_rows = manifest(corpus, derived)
    if args.match:
        rows = [r for r in rows if any(m.lower() in r["path"].lower() for m in args.match)]
    rp = os.path.join(SOUND, "build", "rp")
    binary = hashlib.sha1(open(rp, "rb").read()).hexdigest()[:12]
    extra = {k: v for k, v in os.environ.items() if k.startswith("RP_")}   # an A/B run
    env = dict(suite.get("env", {}), **extra)
    tag = f"{args.suite}-{binary}" + ("-" + hashlib.sha1(json.dumps(extra, sort_keys=True).encode()).hexdigest()[:6]
                                      if extra else "")
    results = os.path.join(derived, "results", tag + ".csv")
    os.makedirs(os.path.dirname(results), exist_ok=True)
    have = {}
    if os.path.exists(results):
        with open(results) as f:
            have = {r["run_id"]: r for r in csv.DictReader(f)}
    jobs = []
    for chunk in suite["chunk"]:
        for row in rows:
            run_id = hashlib.sha1(f"{tag}|{json.dumps(env, sort_keys=True)}|{chunk}|{row['id']}|{suite['recipe']}"
                                  .encode()).hexdigest()[:16]
            if run_id not in have:
                jobs.append((rp, binary, args.suite, env, chunk, materialize(corpus, derived, row, suite["recipe"]),
                             row, suite["recipe"], derived, run_id))
    print(f"{args.suite}: {len(rows)} inputs x {len(suite['chunk'])} chunk sizes, {len(jobs)} runs to do, "
          f"binary {binary}" + (f", extra env {extra}" if extra else ""), flush=True)
    new = not os.path.exists(results)
    with open(results, "a", newline="") as f, cf.ThreadPoolExecutor(args.jobs) as ex:
        w = csv.DictWriter(f, RESULT_COLS)
        if new:
            w.writeheader()
        for k, fut in enumerate(cf.as_completed([ex.submit(one_run, j) for j in jobs]), 1):
            r = fut.result()
            w.writerow(r)
            f.flush()
            na = f" nb={float(r['neighbor_acc']):.3f}" if r["neighbor_acc"] else ""
            print(f"[{k}/{len(jobs)}] {r['class']:<11}{na:<10} {r['chunk_size']:>5} {r['name'][-60:]} ({r['wall_s']}s)",
                  flush=True)
    live = {r["id"] for r in all_rows}   # inputs since removed or split from the corpus drop out
    with open(results) as f:
        R = [r for r in csv.DictReader(f) if r["id"] in live]
    for r in R:
        r["class"] = classify(r)   # the thresholds may have changed since the run
    summary(R, results, results[:-4] + ".md")
    print("results:", results, "\nsummary:", results[:-4] + ".md")
    print("  ".join(f"{k} {sum(r['class'] == k for r in R)}" for k in CLASSES))
    return 0

# --------------------------------------------------------------------------------------------
# Summary


CLASSES = ["EXACT", "STRUCTURAL", "WRONG", "SEARCH_FAIL", "TIMEOUT", "ERROR", "CRASH"]


def table(head, rows):
    return "\n".join(["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] +
                     ["| " + " | ".join(str(c) for c in r) + " |" for r in rows])


def mean(rows, k):
    v = [float(r[k]) for r in rows if r[k] not in ("", None)]
    return f"{sum(v) / len(v):.3f}" if v else ""


def rate(rows, k):
    v = [r[k] for r in rows if r[k] in ("true", "false")]
    return f"{sum(x == 'true' for x in v)}/{len(v)}" if v else ""


def summary(R, results, out):
    chunks = sorted({int(r["chunk_size"]) for r in R})
    by_chunk = {c: [r for r in R if int(r["chunk_size"]) == c] for c in chunks}
    md = [f"# Test summary\n\n`{os.path.basename(results)}` - {len(R)} runs, {time.strftime('%Y-%m-%d %H:%M')}. "
          "Metrics are over all n chunks of the stream; for SEARCH_FAIL they describe the deepest partial "
          "path (missing chunks count as wrong; `kendall_tau` covers only the placed chunks). STRUCTURAL = "
          f"complete and neighbor_acc >= {STRUCTURAL_NEIGHBOR_MIN} (the blocks are right, their placement isn't).\n"]
    md.append("## Outcome by chunk size\n")
    md.append(table(["chunk"] + CLASSES + ["runs"],
                    [[c] + [sum(r["class"] == k for r in by_chunk[c]) for k in CLASSES] + [len(by_chunk[c])]
                     for c in chunks]))
    md.append("\n## Metrics by chunk size and outcome (means; start_correct / budget_exhausted as counts)\n")
    rows = []
    for c in chunks:
        for k in CLASSES:
            g = [r for r in by_chunk[c] if r["class"] == k]
            if g and k not in ("TIMEOUT", "ERROR", "CRASH"):
                wall = sorted(float(r["wall_s"]) for r in g if r["wall_s"])
                rows.append([c, k, len(g)] + [mean(g, m) for m in ("neighbor_acc", "position_acc", "kendall_tau",
                                                                    "longest_run_frac", "coverage")]
                            + [rate(g, "start_correct"), rate(g, "budget_exhausted"),
                               f"{wall[len(wall) // 2]:.1f}" if wall else "", f"{sum(wall):.0f}"])
    md.append(table(["chunk", "class", "runs", "neighbor", "position", "tau", "longest run", "coverage",
                     "start ok", "budget out", "wall median s", "wall total s"], rows))

    def fmt(r):
        rate = r["kbps"] + " kbps" if r["mode"] == "CBR" else "avg"
        return f"{r['format']} {r['mode']} {rate} @ {int(r['sample_rate'] or 0) / 1000:g} kHz"
    md.append("\n## Exact runs by format and chunk size (exact / runs)\n")
    md.append(table(["format"] + [f"{c // 1024} KB" for c in chunks],
                    [[g] + [f"{sum(r['class'] == 'EXACT' for r in by_chunk[c] if fmt(r) == g)} / "
                            f"{sum(fmt(r) == g for r in by_chunk[c])}" for c in chunks]
                     for g in sorted({fmt(r) for r in R})]))
    bins = [0, 0.25, 0.5, 0.75, 0.9, 1.0001]   # where the complete-but-wrong orders fall
    wrong = [r for r in R if r["status"] == "complete" and r["exact"] != "true"]
    if wrong:
        def b(v):
            return next(i for i in range(len(bins) - 1) if float(v) < bins[i + 1])
        lab = [f"{bins[i]:g}-{min(bins[i + 1], 1):g}" for i in range(len(bins) - 1)]
        md.append("\n## Complete but not exact: neighbor_acc (rows) x position_acc (columns), runs\n")
        md.append(table(["neighbor \\ position"] + lab,
                        [[lab[i]] + [sum(1 for r in wrong if b(r["neighbor_acc"]) == i and b(r["position_acc"]) == j)
                                     for j in range(len(lab))] for i in reversed(range(len(lab)))]))
    md.append("\n## Per input (class, neighbor_acc)\n")
    at = {(r["name"], int(r["chunk_size"])): r for r in R}

    def show(x):
        if x is None:
            return ""
        return x["class"] + (f" {float(x['neighbor_acc']):.2f}" if x["neighbor_acc"] and x["class"] != "EXACT" else "")
    md.append(table(["input"] + [f"{c // 1024} KB" for c in chunks],
                    [[n[-60:]] + [show(at.get((n, c))) for c in chunks] for n in sorted({r["name"] for r in R})]))
    open(out, "w").write("\n".join(md) + "\n")

# --------------------------------------------------------------------------------------------


def cmd_score(args):
    rep = json.load((gzip.open if args.report.endswith(".gz") else open)(args.report, "rt"))
    r = row_of(rep)
    r["class"] = classify(r)
    for k in ["class", "status", "n"] + METRIC_COLS:
        print(f"{k:<17} {r.get(k, '')}")
    if args.expect_exact and r["class"] != "EXACT":
        print("FAIL: expected an exact reconstruction")
        return 1
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in json.load(open(SUITES)):
        p = sub.add_parser(name)
        p.add_argument("--match", nargs="+", help="only inputs whose path contains one of these")
        p.add_argument("--jobs", type=int, default=8)   # rp is mostly single-threaded; 10 OOM-killed a 6 GB 1 KB run on 14 GB
        p.set_defaults(func=cmd_suite, suite=name)
    s = sub.add_parser("score")
    s.add_argument("report")
    s.add_argument("--expect-exact", action="store_true")
    s.set_defaults(func=cmd_score)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main() or 0)
