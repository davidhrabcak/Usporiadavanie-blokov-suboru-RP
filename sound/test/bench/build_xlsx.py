#!/usr/bin/env python3
"""Build the benchmark workbook from bench/runs.csv and probe/probe.csv."""
import csv, os, sys
from openpyxl import Workbook
from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
from openpyxl.utils import get_column_letter
from openpyxl.formatting.rule import ColorScaleRule
from openpyxl.worksheet.table import Table, TableStyleInfo

SP = os.environ.get("BENCH_DIR") or os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else SP + '/benchmark_pcm_vs_main.xlsx'

FONT = 'Arial'
HDR_FILL = PatternFill('solid', fgColor='1F3864')
HDR_FONT = Font(name=FONT, bold=True, color='FFFFFF')
SUB_FILL = PatternFill('solid', fgColor='D9E1F2')
BOLD = Font(name=FONT, bold=True)
BASE = Font(name=FONT)
THIN = Side(style='thin', color='BFBFBF')

CONFIGS = [  # (key, label, description)
    ('main', 'main (as-is)', 'branch sideinfo-mainbits @777c455, unmodified algorithm'),
    ('mainfix', 'main + final-fragment fix', 'main with the shared chunk_meta/adjacency fix for a header-less last chunk (aa9ce7c) - isolates that fix from the PCM work'),
    ('pcm', 'PCM branch v1', 'full-pcm-decode @1446fe6: PCM ranking + locking + repair, final-fragment fix, byte-capped window; locking still capped at 40k decodes, repair unbounded'),
    ('final', 'PCM branch final', 'full-pcm-decode @1c5abe1: + threaded/memoised locking with a fixed 640k-decode cap, bounded two-phase repair, repair skipped after a strict-tau success, wrong lock on chunk 0 abandons the attempt (RP_THREADS=4 in this benchmark)'),
]
ABL = [('final', 'final (all on)'), ('final_nopcm', 'RP_PCM=0'), ('final_nolock', 'RP_LOCK=0'),
       ('final_norepair', 'RP_REPAIR=0'), ('final_tau15', 'RP_LOCK_TAU=1.5'), ('final_repair1', 'RP_REPAIR=1 (forced)'),
       ('pcm', 'v1 (all on)'), ('pcm_nopcm', 'v1 RP_PCM=0'), ('pcm_nolock', 'v1 RP_LOCK=0'),
       ('pcm_norepair', 'v1 RP_REPAIR=0'), ('pcm_tau15', 'v1 RP_LOCK_TAU=1.5')]

def load(path):
    with open(path) as f: return list(csv.DictReader(f))

def num(v):
    if v in (None, '', 'None'): return None
    if v in ('True', 'False'): return v == 'True'
    try:
        x = float(v); return int(x) if x.is_integer() and '.' not in v else x
    except ValueError:
        return v

def header(ws, row, cols, widths=None):
    for c, h in enumerate(cols, 1):
        cell = ws.cell(row=row, column=c, value=h)
        cell.font = HDR_FONT; cell.fill = HDR_FILL
        cell.alignment = Alignment(wrap_text=True, vertical='center', horizontal='center')
    if widths:
        for c, w in enumerate(widths, 1): ws.column_dimensions[get_column_letter(c)].width = w

def style_all(ws):
    for row in ws.iter_rows():
        for cell in row:
            if cell.font is None or cell.font.name != FONT or (not cell.font.bold and cell.font.color is None):
                f = cell.font
                cell.font = Font(name=FONT, bold=f.bold, color=f.color, italic=f.italic, size=f.size)

runs = load(SP + '/bench/runs.csv')
probe = load(SP + '/probe/probe.csv')
FILE_ORDER = lambda f: (0 if f.startswith('cbr') else 1 if f.startswith('vbr') else 2,
                        int(''.join(ch for ch in f.split('_')[0] if ch.isdigit()) or 0) if f[:3] in ('cbr', 'vbr') else 0,
                        f)
files = sorted({r['file'] for r in runs}, key=FILE_ORDER)
sizes = sorted({int(r['chunk_size']) for r in runs})

wb = Workbook()

# ---------------- Notes ----------------
ws = wb.active; ws.title = 'Notes'
notes = [
    ('MP3 chunk reconstruction: main vs full-pcm-decode', True),
    ('', False),
    ('What was run', True),
    ('Each run shuffles the file into fixed-size chunks (chunk 0 pinned) with RP_SEED and asks rp to restore the order.', False),
    ('Chunk sizes 1, 2, 4, 8 KB (RP_CHUNK_SIZE). Seeds 1-3 for the main comparison, seed 1 for ablations.', False),
    ('Every run: RP_TIME_BUDGET=30 (search budget per attempt), hard timeout 900 s.', False),
    ('Pass 1 (main, main+fix, PCM v1, v1 ablations) ran 14 processes in parallel on 16 cores; pass 2 (PCM final + ablations) ran 4 processes x RP_THREADS=4.', False),
    ('Wall-clock times are therefore load-affected and only roughly comparable between passes.', False),
    ('PCM final: EXACT runs from the first pass-2 attempt (bb1100d) were kept unless that log showed locking skipped by its thread-scaled cap; every other final / final_* / ho_final run was rerun on 1c5abe1. bb1100d and 1c5abe1 differ only in the lock cap (those cells rerun) and failure-path handling.', False),
    ('Budgets differ by design: main searches once (30 s); the PCM branch may run 3 lock attempts x 30 s + locking + up to 60 s repair. The "Budget check" sheet reruns main+fix with a 150 s budget where PCM final is exact and main+fix is not.', False),
    ('Deepest-path accounting: PCM final keeps the deepest partial path over all lock attempts; PCM v1 (pass 1) only reported its last attempt, so v1 recovered_frac understates it on FAILED runs. The main backport keeps the maximum over all beam widths.', False),
    ('PCM v1 has 4 ERROR runs: the pinned-start crash after a wrong lock on chunk 0 (fixed in 1c5abe1). They count as non-exact.', False),
    ('The repair policy (skip after a strict-tau success) and the two-phase repair scan were derived from cells of this same matrix (cbr32_10s 1 KB, cbr320_10s 4 KB, test7_short). The "Held-out" sheet is a second segment (start 200 s) not used for any tuning.', False),
    ('', False),
    ('Configurations', True),
] + [(f'{lab}: {desc}', False) for _, lab, desc in CONFIGS] + [
    ('', False),
    ('Outcomes (Runs sheet, column "outcome")', True),
    ('EXACT - output is byte-identical to the source.', False),
    ('COMPLETE_WRONG - a complete, structurally valid ordering that is not the source order.', False),
    ('FAILED - no complete ordering within the budget (or search exhausted). rp writes nothing.', False),
    ('TIMEOUT - hit the 900 s hard limit.', False),
    ('', False),
    ('Metrics', True),
    ('recovered_frac - true successor junctions in the result (complete ordering, or deepest partial path for FAILED) / (chunks - 1). Comparable across outcomes.', False),
    ('path_precision - true junctions / junctions in the (partial) path.', False),
    ('Exact (of 3) - number of seeds 1-3 with a byte-exact result.', False),
    ('', False),
    ('Test files', True),
    ('cbr<K>_<D>s / vbrq<Q>_<D>s: generated by sound/test/make_bitrate_variants.sh from the Queen track (start 120 s), LAME, 44.1 kHz joint stereo, no tags/Xing.', False),
    ('bohemoan-rhapsody-2k/4k/8k.mp3 are byte-identical (md5 85368b27...), a 6 min 32 kbps CBR file; tested once at each chunk size.', False),
    ('Queen - Bohemian Rhapsody VBR.mp3: 6 min LAME VBR (~121 kbps average).', False),
    ('cbr320 at 1 KB is out of scope: its 1045-byte frames are longer than a chunk, so some chunks contain no frame header.', False),
    ('', False),
    ('Probe sheet', True),
    ('pcm_probe on the TRUE order: how well each per-edge signal ranks the true successor among the structural candidates (budget-independent).', False),
    ('flux = PCM junction spectral flux (PCM branch); m2 = Milestone 2 partial spectral L2 (main branch). lfl_* = like-for-like candidate sets.', False),
    ('lockXXX_wrong / _cov_pct = median-mode edge locks at tau X.XX that are wrong / % of branch-point true edges locked.', False),
    ('gate1_exact_pct < 100 at 4-8 KB is expected: the byte-capped window starts mid-chunk, and its first warm-up frames cannot be sample-exact.', False),
]
for i, (t, b) in enumerate(notes, 1):
    c = ws.cell(row=i, column=1, value=t); c.font = Font(name=FONT, bold=b, size=14 if i == 1 else 10)
ws.column_dimensions['A'].width = 150

# ---------------- Runs ----------------
wr = wb.create_sheet('Runs')
cols = ['config', 'file', 'mode', 'rate', 'duration_s', 'chunk_size', 'seed', 'outcome', 'recovered_frac',
        'path_precision', 'exact_pos_pct', 'true_junctions', 'path_junctions', 'prefix', 'chunks', 'pre_breaks',
        'resv_breaks', 'edges', 'supernodes', 'lock_attempts', 'final_tau', 'locks', 'locks_correct',
        'lock_skipped', 'repair_moves', 'wall_s', 'rc']
header(wr, 1, cols, [14, 30, 6, 18, 9, 9, 6, 16, 10, 10, 10, 9, 9, 8, 8, 8, 8, 9, 10, 8, 8, 7, 8, 8, 8, 9, 5])
runs_sorted = sorted(runs, key=lambda r: ([c[0] for c in CONFIGS + [(a, '', '') for a, _ in ABL]].index(r['config'])
                                          if r['config'] in [c[0] for c in CONFIGS] + [a for a, _ in ABL] else 99,
                                          FILE_ORDER(r['file']), int(r['chunk_size']), int(r['seed'])))
for i, r in enumerate(runs_sorted, 2):
    for c, k in enumerate(cols, 1):
        cell = wr.cell(row=i, column=c, value=num(r.get(k)))
        cell.font = BASE
        if k in ('recovered_frac', 'path_precision'): cell.number_format = '0.0%'
NR = len(runs_sorted) + 1
wr.freeze_panes = 'C2'
wr.auto_filter.ref = f'A1:{get_column_letter(len(cols))}{NR}'
C = {k: get_column_letter(i) for i, k in enumerate(cols, 1)}
rng = lambda k: f"Runs!${C[k]}$2:${C[k]}${NR}"

# ---------------- Summary (file x chunk size) ----------------
ws = wb.create_sheet('Summary', 1)
ws['A1'] = 'Exact reconstructions (of 3 seeds) and mean recovered junctions per file x chunk size'
ws['A1'].font = Font(name=FONT, bold=True, size=13)
ws['A2'] = 'All cells are COUNTIFS/AVERAGEIFS over the Runs sheet. Recovered = true junctions found / (chunks-1), averaged over seeds; "-" = no runs.'
ws['A2'].font = Font(name=FONT, italic=True, size=9)
r0 = 4
fixed_cols = ['file', 'mode', 'rate', 'chunk KB', 'chunks']
hdr = fixed_cols[:]
for _, lab, _ in CONFIGS: hdr += [f'{lab}\nexact /3', f'{lab}\ncomplete-wrong /3', f'{lab}\nrecovered']
header(ws, r0, hdr, [30, 6, 18, 8, 8] + [11, 11, 11] * len(CONFIGS))
ws.row_dimensions[r0].height = 58
row = r0 + 1
chunks_of = {}
for r in runs:
    if r['chunks']: chunks_of[(r['file'], int(r['chunk_size']))] = int(r['chunks'])
meta = {r['file']: (r['mode'], r['rate']) for r in runs}
first_data = row
for f in files:
    for cs in sizes:
        ws.cell(row=row, column=1, value=f); ws.cell(row=row, column=2, value=meta[f][0])
        ws.cell(row=row, column=3, value=meta[f][1]); ws.cell(row=row, column=4, value=cs // 1024)
        ws.cell(row=row, column=5, value=chunks_of.get((f, cs)))
        col = 6
        for key, _, _ in CONFIGS:
            crit = f'{rng("config")},"{key}",{rng("file")},$A{row},{rng("chunk_size")},$D{row}*1024'
            ws.cell(row=row, column=col, value=f'=COUNTIFS({crit},{rng("outcome")},"EXACT")')
            ws.cell(row=row, column=col + 1, value=f'=COUNTIFS({crit},{rng("outcome")},"COMPLETE_WRONG")')
            c = ws.cell(row=row, column=col + 2,
                        value=f'=IFERROR(AVERAGEIFS({rng("recovered_frac")},{crit}),"-")')
            c.number_format = '0.0%'
            col += 3
        row += 1
last_data = row - 1
# totals
ws.cell(row=row, column=1, value='TOTAL exact / runs').font = BOLD
col = 6
for key, _, _ in CONFIGS:
    L = get_column_letter(col)
    ws.cell(row=row, column=col, value=f'=SUM({L}{first_data}:{L}{last_data})').font = BOLD
    L2 = get_column_letter(col + 1)
    ws.cell(row=row, column=col + 1, value=f'=SUM({L2}{first_data}:{L2}{last_data})').font = BOLD
    ws.cell(row=row, column=col + 2, value=f'=COUNTIFS({rng("config")},"{key}",{rng("seed")},"<=3")').font = BOLD
    col += 3
row += 1
ws.cell(row=row, column=1, value='Exact rate').font = BOLD
col = 6
for key, _, _ in CONFIGS:
    L, Lr = get_column_letter(col), get_column_letter(col + 2)
    c = ws.cell(row=row, column=col, value=f'=IF({Lr}{row-1}=0,"-",{L}{row-1}/{Lr}{row-1})')
    c.number_format = '0.0%'; c.font = BOLD
    col += 3
ws.cell(row=row + 1, column=1, value='(the "recovered" column of the TOTAL row holds the number of runs)').font = Font(name=FONT, italic=True, size=9)
ws.freeze_panes = ws.cell(row=r0 + 1, column=6)
# heat scale on exact & recovered
col = 6
for _ in CONFIGS:
    L = get_column_letter(col)
    ws.conditional_formatting.add(f'{L}{first_data}:{L}{last_data}',
        ColorScaleRule(start_type='num', start_value=0, start_color='F8696B', mid_type='num', mid_value=1.5,
                       mid_color='FFEB84', end_type='num', end_value=3, end_color='63BE7B'))
    L = get_column_letter(col + 2)
    ws.conditional_formatting.add(f'{L}{first_data}:{L}{last_data}',
        ColorScaleRule(start_type='num', start_value=0, start_color='F8696B', mid_type='num', mid_value=0.8,
                       mid_color='FFEB84', end_type='num', end_value=1, end_color='63BE7B'))
    col += 3

# ---------------- By group (mode x chunk size) ----------------
wg = wb.create_sheet('By group', 2)
wg['A1'] = 'Exact-reconstruction rate by bitrate mode and chunk size (seeds 1-3, all files of that mode)'
wg['A1'].font = Font(name=FONT, bold=True, size=13)
hdr = ['mode', 'chunk KB', 'runs per config'] + [f'{lab}\nexact rate' for _, lab, _ in CONFIGS] + \
      [f'{lab}\nmean recovered' for _, lab, _ in CONFIGS]
header(wg, 3, hdr, [8, 8, 9] + [14] * (2 * len(CONFIGS)))
wg.row_dimensions[3].height = 45
row = 4
for mode in ('CBR', 'VBR'):
    for cs in sizes + ['all']:
        wg.cell(row=row, column=1, value=mode); wg.cell(row=row, column=2, value=(cs // 1024) if cs != 'all' else 'all')
        cscrit = f',{rng("chunk_size")},$B{row}*1024' if cs != 'all' else ''
        base = f'{rng("mode")},$A{row}{cscrit},{rng("seed")},"<=3"'
        wg.cell(row=row, column=3, value=f'=COUNTIFS({rng("config")},"main",{base})')
        for j, (key, _, _) in enumerate(CONFIGS):
            crit = f'{rng("config")},"{key}",{base}'
            c = wg.cell(row=row, column=4 + j,
                        value=f'=IFERROR(COUNTIFS({crit},{rng("outcome")},"EXACT")/COUNTIFS({crit}),"-")')
            c.number_format = '0.0%'
            c = wg.cell(row=row, column=4 + len(CONFIGS) + j,
                        value=f'=IFERROR(AVERAGEIFS({rng("recovered_frac")},{crit}),"-")')
            c.number_format = '0.0%'
        if cs == 'all':
            for cc in range(1, 4 + 2 * len(CONFIGS)): wg.cell(row=row, column=cc).font = BOLD
        row += 1
wg.conditional_formatting.add(f'D4:{get_column_letter(3 + 2 * len(CONFIGS))}{row-1}',
    ColorScaleRule(start_type='num', start_value=0, start_color='F8696B', mid_type='num', mid_value=0.5,
                   mid_color='FFEB84', end_type='num', end_value=1, end_color='63BE7B'))

# ---------------- Ablations ----------------
wa = wb.create_sheet('Ablations', 3)
wa['A1'] = 'Ablations, seed 1: outcome per configuration (E = exact, W = complete-but-wrong, F = failed, T = timeout) and recovered junctions'
wa['A1'].font = Font(name=FONT, bold=True, size=13)
hdr = ['file', 'chunk KB'] + [f'{lab}\noutcome' for _, lab in ABL] + [f'{lab}\nrecovered' for _, lab in ABL]
header(wa, 3, hdr, [30, 8] + [11] * (2 * len(ABL)))
wa.row_dimensions[3].height = 58
row = 4
for f in files:
    for cs in sizes:
        wa.cell(row=row, column=1, value=f); wa.cell(row=row, column=2, value=cs // 1024)
        for j, (key, _) in enumerate(ABL):
            crit = f'{rng("config")},"{key}",{rng("file")},$A{row},{rng("chunk_size")},$B{row}*1024,{rng("seed")},1'
            wa.cell(row=row, column=3 + j, value=(
                f'=IF(COUNTIFS({crit})=0,"-",IF(COUNTIFS({crit},{rng("outcome")},"EXACT")>0,"E",'
                f'IF(COUNTIFS({crit},{rng("outcome")},"COMPLETE_WRONG")>0,"W",'
                f'IF(COUNTIFS({crit},{rng("outcome")},"TIMEOUT")>0,"T","F"))))'))
            c = wa.cell(row=row, column=3 + len(ABL) + j, value=f'=IFERROR(AVERAGEIFS({rng("recovered_frac")},{crit}),"-")')
            c.number_format = '0.0%'
        row += 1
ws_last = row - 1
wa.cell(row=row, column=1, value='exact count').font = BOLD
for j in range(len(ABL)):
    L = get_column_letter(3 + j)
    wa.cell(row=row, column=3 + j, value=f'=COUNTIF({L}4:{L}{ws_last},"E")').font = BOLD
wa.freeze_panes = 'C4'
from openpyxl.formatting.rule import CellIsRule
L1, L2 = get_column_letter(3), get_column_letter(2 + len(ABL))
for val, color in (('"E"', 'C6EFCE'), ('"W"', 'FFEB9C'), ('"F"', 'FFC7CE'), ('"T"', 'FFC7CE')):
    wa.conditional_formatting.add(f'{L1}4:{L2}{ws_last}', CellIsRule(operator='equal', formula=[val], fill=PatternFill('solid', fgColor=color)))

# ---------------- Probe ----------------
wp = wb.create_sheet('Probe', 4)
pcols = ['file', 'mode', 'rate', 'chunk_size', 'chunks', 'junctions_deg2', 'flux_baseline', 'flux_auc',
         'flux_rank1_pct', 'flux_meanrank', 'm2_auc', 'm2_rank1_pct', 'm2_meanrank', 'lfl_junctions',
         'lfl_random_rank', 'lfl_flux_auc', 'lfl_flux_rank1_pct', 'lfl_m2_auc', 'lfl_m2_rank1_pct',
         'twoopt_swaps', 'twoopt_true_cheaper_pct', 'lock150_wrong', 'lock150_cov_pct', 'lock125_wrong',
         'lock125_cov_pct', 'lock110_wrong', 'lock110_cov_pct', 'lock100_wrong', 'lock100_cov_pct',
         'gate1_exact_pct', 'secs', 'rc']
header(wp, 1, pcols, [30, 6, 18] + [10] * (len(pcols) - 3))
wp.row_dimensions[1].height = 45
extra_files = {'CBR', 'sample-3s', 'test7_short', 'test7_shorter'}
ps = sorted(probe, key=lambda r: (r['file'] in extra_files, FILE_ORDER(r['file']), int(r['chunk_size'])))
for i, r in enumerate(ps, 2):
    for c, k in enumerate(pcols, 1):
        v = num(r.get(k))
        if k in ('mode', 'rate') and r['file'] in extra_files:
            v = {'CBR': ('CBR', '128 kbps (old fixture)'), 'sample-3s': ('VBR', 'old fixture'),
                 'test7_short': ('VBR', 'old fixture'), 'test7_shorter': ('VBR', 'old fixture')}[r['file']][0 if k == 'mode' else 1]
        wp.cell(row=i, column=c, value=v).font = BASE
wp.freeze_panes = 'D2'
wp.auto_filter.ref = f'A1:{get_column_letter(len(pcols))}{len(ps)+1}'
wp.cell(row=len(ps) + 3, column=1, value='Rows with junctions_deg2 = 0 have no branch points (structure alone is unambiguous); their AUC 0.5 / rank 0 are placeholders, not results.').font = Font(name=FONT, italic=True, size=9)

def pair_sheet(title, heading, configs, file_filter, cs_list, seed=1):
    w = wb.create_sheet(title)
    w['A1'] = heading; w['A1'].font = Font(name=FONT, bold=True, size=13)
    hdr = ['file', 'chunk KB'] + [f'{lab}\noutcome' for _, lab in configs] + [f'{lab}\nrecovered' for _, lab in configs] + [f'{lab}\nwall s' for _, lab in configs]
    header(w, 3, hdr, [30, 8] + [12] * (3 * len(configs)))
    w.row_dimensions[3].height = 45
    rr = 4
    keys = sorted({(r['file'], int(r['chunk_size'])) for r in runs if r['config'] in [c for c, _ in configs] and file_filter(r)},
                  key=lambda k: (FILE_ORDER(k[0]), k[1]))
    for f, cs in keys:
        w.cell(row=rr, column=1, value=f); w.cell(row=rr, column=2, value=cs // 1024)
        for j, (key, _) in enumerate(configs):
            crit = f'{rng("config")},"{key}",{rng("file")},$A{rr},{rng("chunk_size")},$B{rr}*1024,{rng("seed")},{seed}'
            w.cell(row=rr, column=3 + j, value=(
                f'=IF(COUNTIFS({crit})=0,"-",IF(COUNTIFS({crit},{rng("outcome")},"EXACT")>0,"E",'
                f'IF(COUNTIFS({crit},{rng("outcome")},"COMPLETE_WRONG")>0,"W",'
                f'IF(COUNTIFS({crit},{rng("outcome")},"TIMEOUT")>0,"T","F"))))'))
            c = w.cell(row=rr, column=3 + len(configs) + j, value=f'=IFERROR(AVERAGEIFS({rng("recovered_frac")},{crit}),"-")')
            c.number_format = '0.0%'
            c = w.cell(row=rr, column=3 + 2 * len(configs) + j, value=f'=IFERROR(AVERAGEIFS({rng("wall_s")},{crit}),"-")')
            c.number_format = '0.0'
        rr += 1
    last = rr - 1
    w.cell(row=rr, column=1, value='exact count').font = BOLD
    for j in range(len(configs)):
        L = get_column_letter(3 + j)
        w.cell(row=rr, column=3 + j, value=f'=COUNTIF({L}4:{L}{last},"E")').font = BOLD
    w.cell(row=rr + 1, column=1, value='complete-but-wrong count').font = BOLD
    for j in range(len(configs)):
        L = get_column_letter(3 + j)
        w.cell(row=rr + 1, column=3 + j, value=f'=COUNTIF({L}4:{L}{last},"W")').font = BOLD
    L1, L2 = get_column_letter(3), get_column_letter(2 + len(configs))
    for val, color in (('"E"', 'C6EFCE'), ('"W"', 'FFEB9C'), ('"F"', 'FFC7CE'), ('"T"', 'FFC7CE')):
        w.conditional_formatting.add(f'{L1}4:{L2}{last}', CellIsRule(operator='equal', formula=[val], fill=PatternFill('solid', fgColor=color)))
    w.freeze_panes = 'C4'
    return w

if any(r['config'].startswith('ho_') for r in runs):
    pair_sheet('Held-out', 'Held-out segment (start 200 s, sound/test/gen2), seed 1 - not used for any tuning',
               [('ho_mainfix', 'main + fix'), ('ho_final', 'PCM final')], lambda r: True, sizes)
if any(r['config'] == 'mainfix150' for r in runs):
    pair_sheet('Budget check', 'Equal-time check, seed 1: main + fix with a 150 s budget on cells where PCM final is exact and main + fix (30 s) is not',
               [('mainfix', 'main + fix (30 s)'), ('mainfix150', 'main + fix (150 s)'), ('final', 'PCM final')],
               lambda r: r['config'] == 'mainfix150', sizes)

for sh in wb.worksheets:
    for row_ in sh.iter_rows():
        for cell in row_:
            f = cell.font
            if f.name != FONT:
                cell.font = Font(name=FONT, bold=f.bold, italic=f.italic, size=f.size, color=f.color)
wb.save(OUT)
print('wrote', OUT)
