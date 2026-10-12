#!/usr/bin/env python3
"""Parse rp benchmark logs and pcm_probe outputs into CSVs."""
import csv, glob, os, re, sys

SP = os.environ.get("BENCH_DIR") or os.path.dirname(os.path.abspath(__file__))

def last(pat, text, cast=str, group=1, default=None):
    m = re.findall(pat, text, re.M)
    if not m: return default
    v = m[-1]
    if isinstance(v, tuple): v = v[group - 1]
    return cast(v)

def file_meta(name):
    """(mode, kbps_or_q, seconds) from the fixture name."""
    m = re.match(r'cbr(\d+)_(\d+)s', name)
    if m: return 'CBR', m.group(1) + ' kbps', int(m.group(2))
    m = re.match(r'vbrq(\d+)_(\d+)s', name)
    if m: return 'VBR', 'q' + m.group(1), int(m.group(2))
    if name.startswith('bohemoan'): return 'CBR', '32 kbps', 359
    if name.startswith('Queen'): return 'VBR', 'LAME V (~121 kbps)', 359
    return '?', '?', 0

def parse_bench(path):
    t = open(path, errors='replace').read()
    b = re.search(r'BENCH rc=(\d+) secs=([\d.]+) cfg=(\S+) file=(\S+) cs=(\d+) seed=(\d+)', t)
    if not b: return None
    rc, secs, cfg, fname, cs, seed = b.groups()
    mode, rate, dur = file_meta(fname)
    r = dict(config=cfg, file=fname, mode=mode, rate=rate, duration_s=dur, chunk_size=int(cs),
             seed=int(seed), rc=int(rc), wall_s=round(float(secs), 2))
    r['chunks'] = last(r'^Total chunks: (\d+)', t, int)
    r['pre_breaks'] = 0 if 'Pre-shuffle check PASSED' in t else last(r'^Pre-shuffle check FAILED: (\d+)', t, int)
    r['resv_breaks'] = 0 if 'Reservoir check PASSED' in t else last(r'^Reservoir check FAILED on true order: (\d+)', t, int)
    r['edges'] = last(r'^Adjacency graph: (\d+) edges', t, int)
    r['supernodes'] = last(r'^Supernodes after chain collapse: (\d+)', t, int)
    r['lock_attempts'] = len(re.findall(r'^--- lock attempt', t, re.M))
    r['final_tau'] = last(r'^--- lock attempt \d+/\d+ \(tau ([\d.]+)\)', t, float)
    r['locks'] = last(r'^Edge locking total: (\d+) edges', t, int)
    lc = last(r'^Edge locking total: .*\[vs source: (\d+)/(\d+) correct\]', t, str, 1)
    r['locks_correct'] = int(lc) if lc is not None else None
    r['lock_skipped'] = 'Edge locking: skipped' in t
    r['repair_moves'] = last(r'^Block-move repair: (\d+) move', t, int)
    n = r['chunks'] or 0
    if r['rc'] == 124:
        outcome = 'TIMEOUT'
    elif 'EXACT (output.mp3 is byte-identical' in t:
        outcome = 'EXACT'
    elif re.search(r'^Reconstruction vs source:', t, re.M):
        outcome = 'COMPLETE_WRONG'
    elif 'Failed to reconstruct' in t:
        outcome = 'FAILED'
    else:
        outcome = 'ERROR'
    r['outcome'] = outcome
    r['exact_pos_pct'] = last(r'^Reconstruction vs source: \d+/\d+ chunks in original position \(([\d.]+)%\)', t, float)
    tj = re.findall(r'^Reconstruction junctions: (\d+)/(\d+)', t, re.M)
    dp = re.findall(r'^Deepest partial path: (\d+)/(\d+) supernodes, (\d+) chunks; true junctions (\d+)/(\d+); correct prefix = (\d+)', t, re.M)
    if tj:
        good, tot = map(int, tj[-1]); r['path_chunks'] = n; r['prefix'] = None
    elif dp:
        d = dp[-1]; good, tot = int(d[3]), int(d[4]); r['path_chunks'] = int(d[2]); r['prefix'] = int(d[5])
    else:
        good, tot = None, None; r['path_chunks'] = None; r['prefix'] = None
    r['true_junctions'] = good
    r['path_junctions'] = tot
    # fraction of ALL n-1 true junctions recovered (complete or partial path) - comparable across outcomes
    r['recovered_frac'] = round(good / (n - 1), 4) if good is not None and n > 1 else None
    r['path_precision'] = round(good / tot, 4) if good is not None and tot else None
    return r

def parse_probe(path):
    t = open(path, errors='replace').read()
    base = os.path.basename(path)[:-4]
    fname, cs = base.rsplit('__', 1)
    mode, rate, dur = file_meta(fname)
    r = dict(file=fname, mode=mode, rate=rate, duration_s=dur, chunk_size=int(cs))
    r['rc'] = last(r'^rc=(\d+)', t, int)
    r['secs'] = last(r'secs=([\d.]+)', t, float)
    r['chunks'] = last(r'chunks=(\d+)', t, int)
    r['gate1_exact_pct'] = last(r'^  frames compared: \d+, sample-exact: \d+ \(([\d.]+)%\)', t, float)
    r['junctions_deg2'] = last(r'junctions with deg>=2: (\d+) considered', t, int)
    for met in ('flux', 'M2-L2'):
        m = re.search(r'^  ' + re.escape(met) + r'\s+([\d.]+)\s+([\d.]+)%\s+([\d.]+)\s+([\d.]+)', t, re.M)
        k = 'flux' if met == 'flux' else 'm2'
        if m:
            r[k + '_auc'], r[k + '_rank1_pct'], r[k + '_meanrank'], r[k + '_baseline'] = map(float, m.groups())
    m = re.search(r'like-for-like .*?: (\d+) junctions, random mean rank ([\d.]+)\n\s+flux\s+AUC ([\d.]+)\s+rank-1\s+([\d.]+)%\s+meanRank ([\d.]+)\n\s+M2-L2\s+AUC ([\d.]+)\s+rank-1\s+([\d.]+)%\s+meanRank ([\d.]+)', t)
    if m:
        g = m.groups()
        r.update(lfl_junctions=int(g[0]), lfl_random_rank=float(g[1]), lfl_flux_auc=float(g[2]),
                 lfl_flux_rank1_pct=float(g[3]), lfl_flux_meanrank=float(g[4]), lfl_m2_auc=float(g[5]),
                 lfl_m2_rank1_pct=float(g[6]), lfl_m2_meanrank=float(g[7]))
    m = re.search(r'2-opt swap test: (\d+) structural swaps, true pair cheaper in \d+ \(([\d.]+)%\)', t)
    if m: r['twoopt_swaps'], r['twoopt_true_cheaper_pct'] = int(m.group(1)), float(m.group(2))
    med = t.split('median mode')[-1] if 'median mode' in t else ''
    for tau in ('1.00', '1.10', '1.25', '1.50'):
        m = re.search(r'^\s+' + tau + r'\s+(\d+)\s+(\d+)\s+[\d.]+%\s+([\d.]+)%', med, re.M)
        if m:
            k = tau.replace('.', '')
            r['lock' + k + '_wrong'] = int(m.group(2)); r['lock' + k + '_cov_pct'] = float(m.group(3))
    return r

def write(rows, path):
    keys = []
    for r in rows:
        for k in r:
            if k not in keys: keys.append(k)
    with open(path, 'w', newline='') as f:
        w = csv.DictWriter(f, keys); w.writeheader(); w.writerows(rows)
    print(f'{path}: {len(rows)} rows')

if __name__ == '__main__':
    rows = [r for p in sorted(glob.glob(SP + '/bench/logs/*.log')) if (r := parse_bench(p))]
    write(rows, SP + '/bench/runs.csv')
    prows = [parse_probe(p) for p in sorted(glob.glob(SP + '/probe/*__*.txt'))]
    write(prows, SP + '/probe/probe.csv')
