#!/usr/bin/env python3
"""Markdown tables for BENCHMARK_pcm_vs_main.md from runs.csv / probe.csv (BENCH_DIR)."""
import collections, csv, os, re, sys

SP = os.environ.get("BENCH_DIR") or os.path.dirname(os.path.abspath(__file__))
runs = list(csv.DictReader(open(SP + '/bench/runs.csv')))
probe = list(csv.DictReader(open(SP + '/probe/probe.csv')))
R = {(r['config'], r['file'], int(r['chunk_size']), int(r['seed'])): r for r in runs}

def fkey(f):
    m = re.match(r'(cbr|vbrq)(\d+)_(\d+)s', f)
    if m: return (0 if m.group(1) == 'cbr' else 1, int(m.group(2)), int(m.group(3)))
    return (2, 0, f)

def fl(v):
    try: return float(v)
    except (TypeError, ValueError): return None

MAIN = [('main', 'main'), ('mainfix', 'main+fix'), ('pcm', 'PCM v1'), ('final', 'PCM final')]
SIZES = [1024, 2048, 4096, 8192]
NEW = sorted({r['file'] for r in runs if not r['config'].startswith('ho_')}, key=fkey)
HO = sorted({r['file'] for r in runs if r['config'].startswith('ho_')}, key=fkey)

def cell_stats(cfg, files, sizes, seeds=(1, 2, 3), mode=None):
    ex = n = cw = 0; rec = []
    for f in files:
        for cs in sizes:
            for s in seeds:
                r = R.get((cfg, f, cs, s))
                if not r or (mode and r['mode'] != mode): continue
                n += 1; ex += r['outcome'] == 'EXACT'; cw += r['outcome'] == 'COMPLETE_WRONG'
                if fl(r['recovered_frac']) is not None: rec.append(fl(r['recovered_frac']))
    return ex, cw, n, (sum(rec) / len(rec) if rec else None)

def pct(x): return '-' if x is None else f'{100*x:.0f}%'

out = []
P = out.append

P('### Exact reconstructions by bitrate mode and chunk size (seeds 1-3)\n')
P('| mode | chunk | ' + ' | '.join(f'{lab} exact' for _, lab in MAIN) + ' | ' + ' | '.join(f'{lab} recovered' for _, lab in MAIN) + ' |')
P('|---|---|' + '---|' * (2 * len(MAIN)))
for mode in ('CBR', 'VBR'):
    for cs in SIZES + [None]:
        sz = SIZES if cs is None else [cs]
        cells = [cell_stats(c, NEW, sz, mode=mode) for c, _ in MAIN]
        lab = 'all' if cs is None else f'{cs // 1024} KB'
        row = [f'**{mode}**' if cs is None else mode, f'**{lab}**' if cs is None else lab]
        row += [f'{e}/{n}' + (f' (+{w} wrong)' if w else '') for e, w, n, _ in cells]
        row += [pct(r) for *_, r in cells]
        P('| ' + ' | '.join(row) + ' |')
tot = [cell_stats(c, NEW, SIZES) for c, _ in MAIN]
P('| **all** | **all** | ' + ' | '.join(f'**{e}/{n}**' + (f' (+{w} wrong)' if w else '') for e, w, n, _ in tot) + ' | ' + ' | '.join(f'**{pct(r)}**' for *_, r in tot) + ' |')
P('')

P('### Per file and chunk size (exact out of 3 seeds; W = complete-but-wrong runs)\n')
P('| file | chunk | chunks | ' + ' | '.join(lab for _, lab in MAIN) + ' |')
P('|---|---|---|' + '---|' * len(MAIN))
for f in NEW:
    for cs in SIZES:
        ch = next((r['chunks'] for s in (1, 2, 3) for c, _ in MAIN if (r := R.get((c, f, cs, s))) and r['chunks']), '')
        row = [f.replace('_____', ' - ').replace('_', ' ') if f.startswith('Queen') else f, f'{cs // 1024} KB', ch]
        for c, _ in MAIN:
            rs = [R.get((c, f, cs, s)) for s in (1, 2, 3)]
            rs = [r for r in rs if r]
            if not rs: row.append('-'); continue
            e = sum(r['outcome'] == 'EXACT' for r in rs); w = sum(r['outcome'] == 'COMPLETE_WRONG' for r in rs)
            rec = [fl(r['recovered_frac']) for r in rs if fl(r['recovered_frac']) is not None]
            s = f'{e}/{len(rs)}' + (f' W{w}' if w else '')
            if e < len(rs) and rec: s += f' ({pct(sum(rec)/len(rec))})'
            row.append(f'**{s}**' if c == 'final' and e == len(rs) else s)
        P('| ' + ' | '.join(row) + ' |')
P('\n(percent in brackets: mean recovered true junctions when not all seeds were exact)\n')

ABL = [('final', 'all on'), ('final_nopcm', 'RP_PCM=0'), ('final_nolock', 'RP_LOCK=0'),
       ('final_norepair', 'RP_REPAIR=0'), ('final_tau15', 'RP_LOCK_TAU=1.5')]
P('### Ablations of PCM final (seed 1, all 64 file x chunk-size cells)\n')
P('| configuration | exact | complete-but-wrong | failed | mean recovered |')
P('|---|---|---|---|---|')
for c, lab in ABL:
    e, w, n, r = cell_stats(c, NEW, SIZES, seeds=(1,))
    P(f'| {lab} | {e}/{n} | {w} | {n - e - w} | {pct(r)} |')
e, w, n, r = cell_stats('mainfix', NEW, SIZES, seeds=(1,))
P(f'| main+fix (reference) | {e}/{n} | {w} | {n - e - w} | {pct(r)} |\n')

if HO:
    P('### Held-out segment (start 200 s, seed 1)\n')
    P('| configuration | exact | complete-but-wrong | failed | mean recovered |')
    P('|---|---|---|---|---|')
    for c, lab in (('ho_mainfix', 'main+fix'), ('ho_final', 'PCM final')):
        e, w, n, r = cell_stats(c, HO, SIZES, seeds=(1,))
        P(f'| {lab} | {e}/{n} | {w} | {n - e - w} | {pct(r)} |')
    better = worse = 0
    for f in HO:
        for cs in SIZES:
            a, b = R.get(('ho_mainfix', f, cs, 1)), R.get(('ho_final', f, cs, 1))
            if a and b:
                better += b['outcome'] == 'EXACT' and a['outcome'] != 'EXACT'
                worse += a['outcome'] == 'EXACT' and b['outcome'] != 'EXACT'
    P(f'\nCells where only PCM final is exact: {better}; only main+fix: {worse}.\n')

eq = [(k, r) for k, r in R.items() if k[0] in ('mainfix150', 'ho_mainfix150')]
if eq:
    P('### Equal-time check (main+fix with a 150 s budget, seed 1)\n')
    P('| file | chunk | main+fix 30 s | main+fix 150 s | PCM final | PCM final wall s |')
    P('|---|---|---|---|---|---|')
    for (c, f, cs, s), r in sorted(eq, key=lambda x: (x[0][0], fkey(x[0][1]), x[0][2])):
        ho = c.startswith('ho_')
        a = R.get(('ho_mainfix' if ho else 'mainfix', f, cs, s)); b = R.get(('ho_final' if ho else 'final', f, cs, s))
        name = (f + ' (held-out)') if ho else f
        P(f"| {name} | {cs // 1024} KB | {a['outcome'] if a else '-'} | {r['outcome']} | {b['outcome'] if b else '-'} | {b['wall_s'] if b else '-'} |")
    P('')

P('### Per-edge signal on the true order (pcm_probe): rank-1 of the true successor among structural candidates\n')
P('| file | chunk | branch points | candidates/junction (random mean rank) | flux AUC | flux rank-1 | M2 rank-1 | 2-opt true cheaper | tau1.5 locks wrong / coverage |')
P('|---|---|---|---|---|---|---|---|---|')
for r in sorted(probe, key=lambda r: (fkey(r['file']), int(r['chunk_size']))):
    if not r.get('junctions_deg2') or r['junctions_deg2'] in ('0', '') : continue
    P(f"| {r['file']} | {int(r['chunk_size']) // 1024} KB | {r['junctions_deg2']} | {r.get('flux_baseline','')} | {r.get('flux_auc','')} | {r.get('flux_rank1_pct','')}% | {r.get('m2_rank1_pct','')}% | {r.get('twoopt_true_cheaper_pct','')}% | {r.get('lock150_wrong','')} / {r.get('lock150_cov_pct','')}% |")
P('')
print('\n'.join(out))
