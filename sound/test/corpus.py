#!/usr/bin/env python3
"""Corpus tests for rp (PLAN_test_automation.md). Python 3 standard library only.

  test/corpus.py scan                   manifest of every *.mp3 under musiclib/ (cached)
  test/corpus.py run [options]          run rp over a matrix, append to a results CSV
  test/corpus.py report RESULTS.csv     summary.md next to the results
  test/corpus.py compare RESULTS.csv    outcome changes vs test/corpus_baseline.tsv (exit 1 on
                                        a regression); --update-baseline rewrites the baseline

Run from sound/. The corpus (sound/musiclib/) and everything derived from it
(sound/musiclib/.derived/) are gitignored; only test/corpus_baseline.tsv (ids, names and outcome
classes, no audio) and test/corpus_configs.json are committed.
"""
import argparse, concurrent.futures as cf, csv, gzip, hashlib, json, os, shutil, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
SOUND = os.path.dirname(HERE)
# RP_CORPUS points at another folder of mp3s (e.g. test/ for the fixtures tier); its derived
# files still go under the gitignored musiclib/.derived/corpus-<name>/.
CORPUS = os.path.abspath(os.environ.get("RP_CORPUS", os.path.join(SOUND, "musiclib")))
DERIVED = os.path.join(SOUND, "musiclib", ".derived")
if CORPUS != os.path.join(SOUND, "musiclib"):
    DERIVED = os.path.join(DERIVED, "corpus-" + os.path.basename(CORPUS))
MANIFEST = os.path.join(DERIVED, "manifest.tsv")
BASELINE = os.path.join(HERE, "corpus_baseline.tsv")
CONFIGS = os.path.join(HERE, "corpus_configs.json")

# --------------------------------------------------------------------------------------------
# MPEG audio frame headers (MPEG 1 / 2 / 2.5, Layers I-III)

BITRATES = {  # (mpeg1?, layer) -> kbps by index
    (True, 1): [0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448],
    (True, 2): [0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384],
    (True, 3): [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320],
    (False, 1): [0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256],
    (False, 2): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
    (False, 3): [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160],
}
SAMPLE_RATES = {3: [44100, 48000, 32000], 2: [22050, 24000, 16000], 0: [11025, 12000, 8000]}
MPEG_NAME = {3: "1", 2: "2", 0: "2.5"}


def parse_header(d, i):
    """Frame header at d[i:i+4] -> dict, or None when it isn't a valid header."""
    if i + 4 > len(d) or d[i] != 0xFF or (d[i + 1] & 0xE0) != 0xE0:
        return None
    ver, lay = (d[i + 1] >> 3) & 3, (d[i + 1] >> 1) & 3
    bi, si = d[i + 2] >> 4, (d[i + 2] >> 2) & 3
    if ver == 1 or lay == 0 or bi in (0, 15) or si == 3:
        return None
    layer = 4 - lay
    kbps = BITRATES[(ver == 3, layer)][bi]
    sr = SAMPLE_RATES[ver][si]
    pad = (d[i + 2] >> 1) & 1
    if layer == 1:
        length = (12000 * kbps // sr + pad) * 4
    elif layer == 3 and ver != 3:
        length = 72000 * kbps // sr + pad
    else:
        length = 144000 * kbps // sr + pad
    spf = 384 if layer == 1 else (1152 if layer == 2 or ver == 3 else 576)
    return dict(ver=ver, layer=layer, kbps=kbps, sr=sr, len=length, spf=spf,
                crc=not (d[i + 1] & 1), mono=(d[i + 3] >> 6) == 3)


def same_stream(a, b):
    return a["ver"] == b["ver"] and a["layer"] == b["layer"] and a["sr"] == b["sr"]


def frames_of(d):
    """(first frame offset, [(offset, header)...]) of the main frame run (after any ID3v2 tag)."""
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
        return None, []
    first, frames = i, []
    while True:
        h = parse_header(d, i)
        if not h or (frames and not same_stream(h, frames[0][1])) or i + h["len"] > len(d):
            break
        frames.append((i, h))
        i += h["len"]
    return first, frames


def has_tag(d, off, h):
    """Xing/Info/VBRI tag in the frame at `off`."""
    return any(t in d[off + 4: off + 4 + 2 + 32 + 8] for t in (b"Xing", b"Info", b"VBRI"))


def describe(path):
    d = open(path, "rb").read()
    first, frames = frames_of(d)
    if not frames:
        return dict(frames=0)
    h0 = frames[0][1]
    tag = has_tag(d, first, h0)
    audio = frames[1:] if tag and len(frames) > 1 else frames
    rates = {h["kbps"] for _, h in audio}
    end = frames[-1][0] + frames[-1][1]["len"]
    trailing = d[end:]
    kind = ("" if not trailing else "id3v1" if trailing[-128:-125] == b"TAG" else
            "ape" if b"APETAGEX" in trailing else "other")
    row = dict(
        frames=len(frames), mpeg=MPEG_NAME[h0["ver"]], layer=h0["layer"], sample_rate=h0["sr"],
        mode="CBR" if len(rates) == 1 else "VBR",
        kbps=round(sum(h["kbps"] for _, h in audio) / max(1, len(audio))),
        crc=int(h0["crc"]), mono=int(h0["mono"]),
        duration_s=round(sum(h["spf"] for _, h in frames) / h0["sr"], 1),
        audio_bytes=end - first, max_frame=max(h["len"] for _, h in frames),
        info_tag=int(tag), trailing_bytes=len(trailing), trailing_kind=kind)
    for C in (1024, 2048):          # chunks (rp's chunking: from the first frame) holding no whole header
        whole = {(o - first) // C for o, _ in frames if (o - first) % C <= C - 4}
        n = (end - first) // C
        row["noheader_%dk" % (C // 1024)] = sum(1 for c in range(1, n) if c not in whole)
    return row


MANIFEST_COLS = ["id", "path", "source", "duplicates", "size", "mtime", "frames", "mpeg", "layer",
                 "sample_rate", "mode", "kbps", "crc", "mono", "duration_s", "audio_bytes", "max_frame",
                 "info_tag", "trailing_bytes", "trailing_kind", "noheader_1k", "noheader_2k"]


def load_manifest():
    if not os.path.exists(MANIFEST):
        sys.exit("no manifest - run: test/corpus.py scan")
    with open(MANIFEST) as f:
        return list(csv.DictReader(f, delimiter="\t"))


def cmd_scan(args):
    old = {}
    if os.path.exists(MANIFEST):
        with open(MANIFEST) as f:
            for r in csv.DictReader(f, delimiter="\t"):
                old[r["path"]] = r
                for p in filter(None, r["duplicates"].split("|")):
                    old[p] = dict(r, path=p)
    paths = []
    for root, dirs, files in os.walk(CORPUS):
        dirs[:] = sorted(x for x in dirs if not x.startswith(".") and os.path.join(root, x) != os.path.join(SOUND, "musiclib"))
        paths += [os.path.join(root, f) for f in sorted(files) if f.lower().endswith(".mp3")]
    by_id, reused = {}, 0
    for p in paths:
        rel = os.path.relpath(p, CORPUS)
        st = os.stat(p)
        o = old.get(rel)
        if o and o["size"] == str(st.st_size) and o["mtime"] == str(int(st.st_mtime)):
            row, reused = dict(o, path=rel, duplicates=""), reused + 1
        else:
            fid = hashlib.sha1(open(p, "rb").read()).hexdigest()[:12]
            row = dict(id=fid, path=rel, size=st.st_size, mtime=int(st.st_mtime), duplicates="", **describe(p))
        row["source"] = rel.split(os.sep)[0] if os.sep in rel else "."
        if row["id"] in by_id:
            by_id[row["id"]]["duplicates"] += ("|" if by_id[row["id"]]["duplicates"] else "") + rel
        else:
            by_id[row["id"]] = row
    os.makedirs(DERIVED, exist_ok=True)
    with open(MANIFEST, "w", newline="") as f:
        w = csv.DictWriter(f, MANIFEST_COLS, delimiter="\t", extrasaction="ignore")
        w.writeheader()
        for r in sorted(by_id.values(), key=lambda r: r["path"]):
            w.writerow(r)
    rows = list(by_id.values())
    dups = sum(len(filter_dups(r)) for r in rows)
    print(f"{len(paths)} mp3 files, {len(rows)} unique ({dups} duplicates), {reused} reused from cache -> {MANIFEST}")
    for src in sorted({r["source"] for r in rows}):
        rs = [r for r in rows if r["source"] == src]
        modes = {m: sum(1 for r in rs if r.get("mode") == m) for m in ("CBR", "VBR")}
        print(f"  {src}: {len(rs)} unique - CBR {modes['CBR']}, VBR {modes['VBR']}, "
              f"no frames {sum(1 for r in rs if str(r.get('frames')) == '0')}")


def filter_dups(r):
    return [p for p in str(r.get("duplicates", "")).split("|") if p]

# --------------------------------------------------------------------------------------------
# Inputs: a manifest row + a recipe.
#   orig       the file as it is
#   strip      audio frames only (leading/trailing tags removed)
#   headN      the first N chunks' worth of whole frames at the run's chunk size (N = target
#              chunk count): the input differs per chunk size
#   secN       the first N seconds of whole frames: the same input at every chunk size
# Both start at the real start (empty bit reservoir); mid-track stream-copy cuts are not offered,
# their first frames' reservoir points before the cut.


def materialize(row, recipe, chunk):
    src = os.path.join(CORPUS, row["path"])
    if recipe == "orig":
        return src
    out_dir = os.path.join(DERIVED, "inputs")
    os.makedirs(out_dir, exist_ok=True)
    name = f"{row['id']}_{recipe}" + (f"_c{chunk}" if recipe.startswith("head") else "") + ".mp3"
    out = os.path.join(out_dir, name)
    if os.path.exists(out):
        return out
    d = open(src, "rb").read()
    first, frames = frames_of(d)
    if recipe == "strip":
        end = frames[-1][0] + frames[-1][1]["len"]
    elif recipe.startswith("head") or recipe.startswith("sec"):
        bytes_target = int(recipe[4:]) * chunk if recipe.startswith("head") else None
        samples_target = int(recipe[3:]) * frames[0][1]["sr"] if recipe.startswith("sec") else None
        end, samples = first, 0
        for o, h in frames:
            if (bytes_target is not None and end - first >= bytes_target) or \
               (samples_target is not None and samples >= samples_target):
                break
            end, samples = o + h["len"], samples + h["spf"]
    else:
        sys.exit("unknown recipe " + recipe)
    tmp = out + ".tmp"
    with open(tmp, "wb") as f:
        f.write(d[first:end])
    os.replace(tmp, out)
    return out

# --------------------------------------------------------------------------------------------
# Running rp

RESULT_COLS = ["run_id", "class", "binary", "config", "seed", "chunk_size", "input", "id", "recipe",
               "name", "source", "mode", "kbps", "sample_rate", "layer", "crc", "max_frame",
               "trailing_kind", "rc", "timed_out", "status", "error", "chunks", "pre_breaks",
               "resv_breaks", "edges", "supernodes", "lock_attempts", "locks", "locks_correct",
               "matching", "exact", "exact_pct", "true_junctions", "junctions", "provenance",
               "repair_moves", "budget_exhausted", "partial_chunks", "partial_true_junctions",
               "wall_s", "log", "start_candidates", "start_xing", "start_correct"]


def classify(r):
    if r["timed_out"] == "1":
        return "TIMEOUT"
    if r["status"] == "":
        return "CRASH"
    if r["status"] == "error":
        return "ERROR"
    if r["exact"] == "true":
        return "EXACT"
    broken = any(r[k] not in ("", "0") for k in ("pre_breaks", "resv_breaks"))
    if broken:
        return "STRUCTURAL"
    return "WRONG" if r["status"] == "complete" else "SEARCH_FAIL"


def one_run(job):
    rp, binary, cfg_name, cfg_env, seed, chunk, inp, row, recipe, threads, timeout = job
    work = tempfile.mkdtemp(dir=os.path.join(DERIVED, "work"))
    report = os.path.join(work, "report.json")
    env = {k: v for k, v in os.environ.items() if not k.startswith("RP_")}
    env.update(cfg_env)
    env.update(RP_SEED=str(seed), RP_CHUNK_SIZE=str(chunk), RP_THREADS=str(threads), RP_REPORT=report)
    t0, timed_out = time.time(), False
    try:
        p = subprocess.run([rp, inp], cwd=work, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        rc, out = p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        rc, out, timed_out = -9, e.stdout or b"", True
    rep = {}
    if os.path.exists(report):
        try:
            rep = json.load(open(report))
        except ValueError:
            rep = {}
    run_id = job_id(binary, cfg_name, seed, chunk, row["id"], recipe, cfg_env)
    log = os.path.join(DERIVED, "logs", run_id + ".log.gz")
    os.makedirs(os.path.dirname(log), exist_ok=True)
    with gzip.open(log, "wb") as f:
        f.write(out)
    shutil.rmtree(work, ignore_errors=True)
    r = {c: "" for c in RESULT_COLS}
    r.update(run_id=run_id, binary=binary, config=cfg_name, seed=seed, chunk_size=chunk,
             input=f"{row['id']}:{recipe}", id=row["id"], recipe=recipe, name=os.path.basename(row["path"]),
             rc=rc, timed_out=int(timed_out), log=os.path.relpath(log, CORPUS),
             wall_s=rep.get("wall_s", round(time.time() - t0, 2)))
    for k in ("source", "mode", "kbps", "sample_rate", "layer", "crc", "max_frame", "trailing_kind"):
        r[k] = row.get(k, "")
    for k, v in rep.items():
        if k in r and k not in ("wall_s",):
            r[k] = json.dumps(v) if isinstance(v, bool) else ("" if v is None else v)
    r["class"] = classify({k: str(v) for k, v in r.items()})
    return r


def job_id(binary, cfg, seed, chunk, fid, recipe, cfg_env):
    # the config's environment is part of the id: editing corpus_configs.json mustn't resume
    # old results under the same config name
    env = json.dumps(cfg_env, sort_keys=True)
    return hashlib.sha1(f"{binary}|{cfg}|{env}|{seed}|{chunk}|{fid}|{recipe}".encode()).hexdigest()[:16]


def select(rows, args):
    out = []
    for r in rows:
        if str(r.get("frames")) == "0":
            continue
        if args.source and r["source"] not in args.source:
            continue
        if args.mode and r["mode"] != args.mode:
            continue
        if args.match and not any(m.lower() in r["path"].lower() for m in args.match):
            continue
        out.append(r)
    return out[: args.limit] if args.limit else out


def cmd_run(args):
    rows = select(load_manifest(), args)
    configs = json.load(open(CONFIGS))
    rp = os.path.abspath(args.rp)
    binary = hashlib.sha1(open(rp, "rb").read()).hexdigest()[:12]
    results = args.results or os.path.join(DERIVED, "results", "results.csv")
    os.makedirs(os.path.dirname(results), exist_ok=True)
    os.makedirs(os.path.join(DERIVED, "work"), exist_ok=True)
    done = set()
    if os.path.exists(results):
        with open(results) as f:
            done = {r["run_id"] for r in csv.DictReader(f)}
    jobs = []
    for cfg in args.config:
        for seed in args.seed:
            for chunk in args.chunk:
                for recipe in args.recipe:
                    for row in rows:
                        if job_id(binary, cfg, seed, chunk, row["id"], recipe, configs[cfg]) in done:
                            continue
                        inp = materialize(row, recipe, chunk)
                        jobs.append((rp, binary, cfg, configs[cfg], seed, chunk, inp, row, recipe,
                                     args.threads, args.timeout))
    print(f"{len(rows)} inputs, {len(jobs)} runs to do ({len(done)} already in {results}), binary {binary}")
    new = not os.path.exists(results)
    if not new:
        with open(results) as f:
            header = next(csv.reader(f), [])
        if header != RESULT_COLS:   # appending rows under another header would shift the columns
            sys.exit(f"{results} has other columns than this corpus.py writes (an older version?) - "
                     "use a new --results file")
    with open(results, "a", newline="") as f, cf.ThreadPoolExecutor(args.jobs) as ex:
        w = csv.DictWriter(f, RESULT_COLS)
        if new:
            w.writeheader()
        for k, r in enumerate(ex.map(one_run, jobs), 1):
            w.writerow(r)
            f.flush()
            print(f"[{k}/{len(jobs)}] {r['class']:<11} {r['chunk_size']:>5} {r['recipe']:<8} {r['name'][:60]} ({r['wall_s']}s)", flush=True)
    print("results:", results)

# --------------------------------------------------------------------------------------------
# Report and compare

CLASSES = ["EXACT", "WRONG", "SEARCH_FAIL", "STRUCTURAL", "TIMEOUT", "ERROR", "CRASH"]


def load_results(path, binary=None):
    """Rows of one binary: `binary` (prefix) or, by default, the last one appended. A results
    file accumulates runs of several builds; mixing them double-counts every input."""
    with open(path) as f:
        rows = list(csv.DictReader(f))
    if not rows:
        return rows, None
    if binary is None:
        binary = rows[-1]["binary"]
    sel = [r for r in rows if r["binary"].startswith(binary)]
    others = sorted({r["binary"] for r in rows} - {r["binary"] for r in sel})
    print(f"{path}: binary {binary} ({len(sel)} of {len(rows)} rows"
          + (f"; also in the file: {', '.join(others)} - --binary selects" if others else "") + ")")
    return sel, binary


def table(head, rows):
    out = ["| " + " | ".join(head) + " |", "|" + "---|" * len(head)]
    out += ["| " + " | ".join(str(c) for c in r) + " |" for r in rows]
    return "\n".join(out)


def structural_cause(r):
    if r["class"] != "STRUCTURAL":
        return ""
    if int(r["max_frame"] or 0) > int(r["chunk_size"]):
        return "frames > chunk"
    if r["trailing_kind"]:
        return "trailing " + r["trailing_kind"]
    return "other"


def cmd_report(args):
    R, binary = load_results(args.results, args.binary)
    chunks = sorted({int(r["chunk_size"]) for r in R})
    md = [f"# Corpus test summary\n\n`{args.results}` - {len(R)} runs of binary {binary}, "
          f"generated {time.strftime('%Y-%m-%d %H:%M')}\n"]
    if any(r["recipe"].startswith("head") for r in R):
        md.append("`headN` inputs are the first N chunks at each chunk size, so they differ between "
                  "chunk-size columns; `secN` inputs are the same at every chunk size.\n")
    md.append("## Outcome by chunk size\n")
    md.append(table(["chunk"] + CLASSES + ["runs"],
                    [[c] + [sum(1 for r in R if int(r["chunk_size"]) == c and r["class"] == k) for k in CLASSES]
                     + [sum(1 for r in R if int(r["chunk_size"]) == c)] for c in chunks]))
    def fmt(r):   # CBR by bitrate, VBR pooled
        return (r["mode"], r["kbps"] if r["mode"] == "CBR" else "avg", r["sample_rate"])

    def exact_of(g, c):
        cell = [r for r in R if fmt(r) == g and int(r["chunk_size"]) == c]
        return f"{sum(1 for r in cell if r['class'] == 'EXACT')} / {len(cell)}"

    groups = sorted({fmt(r) for r in R}, key=lambda g: (g[0], -int(g[1]) if g[1].isdigit() else 0, g[2]))
    md.append("\n## Exact runs by format and chunk size (exact / runs)\n")
    md.append(table(["format"] + [f"{c // 1024} KB" for c in chunks],
                    [[f"{g[0]} {g[1]}{' kbps' if g[1].isdigit() else ''} @ {int(g[2]) / 1000:g} kHz"]
                     + [exact_of(g, c) for c in chunks] for g in groups]))
    md.append("\n## By source\n")
    md.append(table(["source"] + CLASSES,
                    [[s] + [sum(1 for r in R if r["source"] == s and r["class"] == k) for k in CLASSES]
                     for s in sorted({r["source"] for r in R})]))
    causes = {}
    for r in R:
        c = structural_cause(r)
        if c:
            causes[c] = causes.get(c, 0) + 1
    if causes:
        md.append("\n## Structural failures by cause\n")
        md.append(table(["cause", "runs"], sorted(causes.items(), key=lambda x: -x[1])))
    md.append("\n## Per input\n")
    inputs = sorted({(r["name"], r["recipe"], r["config"], r["seed"]) for r in R})
    cell = {(r["name"], r["recipe"], r["config"], r["seed"], int(r["chunk_size"])): r for r in R}
    rows = []
    for (n, rec, cfg, seed) in inputs:
        cs = [cell.get((n, rec, cfg, seed, c)) for c in chunks]
        rows.append([n[:50], rec, cfg, seed] + [
            "" if x is None else x["class"] + (f" {float(x['exact_pct']):.0f}%" if x["class"] == "WRONG" and x["exact_pct"] else "")
            for x in cs])
    md.append(table(["input", "recipe", "config", "seed"] + [f"{c // 1024} KB" for c in chunks], rows))
    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.results)), "summary.md")
    open(out, "w").write("\n".join(md) + "\n")
    print("summary:", out)


def key_of(r):
    return (r["id"], r["recipe"], str(r["chunk_size"]), r["config"], str(r["seed"]))


def is_regression(old, new):
    if new in ("CRASH", "ERROR") and old not in ("CRASH", "ERROR"):
        return True
    if old == "EXACT" and new != "EXACT":
        return True
    return old in ("WRONG", "SEARCH_FAIL") and new == "STRUCTURAL"


BASE_COLS = ["id", "recipe", "chunk_size", "config", "seed", "class", "name"]


def cmd_compare(args):
    new = {key_of(r): r for r in load_results(args.results, args.binary)[0]}
    if args.update_baseline:
        base = {}
        if os.path.exists(BASELINE):
            with open(BASELINE) as f:
                base = {key_of(r): r for r in csv.DictReader(f, delimiter="\t")}
        base.update(new)
        with open(BASELINE, "w", newline="") as f:
            w = csv.DictWriter(f, BASE_COLS, delimiter="\t", extrasaction="ignore")
            w.writeheader()
            for k in sorted(base):
                w.writerow(base[k])
        print(f"baseline: {len(base)} rows -> {BASELINE}")
        return 0
    if args.against:
        old = {key_of(r): r for r in load_results(args.against, args.against_binary)[0]}
    else:
        if not os.path.exists(BASELINE):
            sys.exit("no baseline - run with --update-baseline first")
        with open(BASELINE) as f:
            old = {key_of(r): r for r in csv.DictReader(f, delimiter="\t")}
    changes, regressions = [], 0
    for k in sorted(set(old) & set(new)):
        o, n = old[k]["class"], new[k]["class"]
        if o != n:
            reg = is_regression(o, n)
            regressions += reg
            changes.append(["REGRESSION" if reg else "change", new[k]["name"][:50], k[1], k[2], k[3], k[4], o, n])
    print(f"{len(set(old) & set(new))} runs compared ({len(set(new) - set(old))} new, "
          f"{len(set(old) - set(new))} not rerun), {len(changes)} changed, {regressions} regressions")
    if changes:
        print(table(["", "input", "recipe", "chunk", "config", "seed", "before", "after"], changes))
    return 1 if regressions else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("scan")
    r = sub.add_parser("run")
    r.add_argument("--rp", default=os.path.join(SOUND, "build", "rp"))
    r.add_argument("--results", help="results CSV (default musiclib/.derived/results/results.csv)")
    r.add_argument("--recipe", nargs="+", default=["orig"],
                   help="orig | strip | headN (first N chunks; differs per chunk size) | secN (first N seconds)")
    r.add_argument("--chunk", nargs="+", type=int, default=[1024])
    r.add_argument("--config", nargs="+", default=["default"], help="names from test/corpus_configs.json")
    r.add_argument("--seed", nargs="+", type=int, default=[1])
    r.add_argument("--source", nargs="+", help="top-level folders of musiclib/")
    r.add_argument("--mode", choices=["CBR", "VBR"])
    r.add_argument("--match", nargs="+", help="substrings of the path")
    r.add_argument("--limit", type=int)
    r.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) // 3))
    r.add_argument("--threads", type=int, default=3, help="RP_THREADS per run")
    r.add_argument("--timeout", type=int, default=900, help="seconds per run")
    p = sub.add_parser("report")
    p.add_argument("results")
    p.add_argument("--out")
    p.add_argument("--binary", help="binary hash (prefix); default: the last one in the file")
    c = sub.add_parser("compare")
    c.add_argument("results")
    c.add_argument("--against", help="another results CSV instead of the baseline")
    c.add_argument("--update-baseline", action="store_true")
    c.add_argument("--binary", help="binary of RESULTS (prefix); default: the last one in the file")
    c.add_argument("--against-binary", help="binary of --against (prefix); default: its last one")
    args = ap.parse_args()
    return {"scan": cmd_scan, "run": cmd_run, "report": cmd_report, "compare": cmd_compare}[args.cmd](args)


if __name__ == "__main__":
    sys.exit(main() or 0)
