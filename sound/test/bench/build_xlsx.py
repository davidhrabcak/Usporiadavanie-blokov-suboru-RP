#!/usr/bin/env python3
"""Readable benchmark workbook: main vs full-pcm-decode (and optional extra configs).

Every summary number is a formula over the 'All runs' sheet, so the workbook recalculates if
runs are added or corrected. Usage: BENCH_DIR=<dir with bench/runs.csv, probe/probe.csv>
python3 build_xlsx.py OUT.xlsx   (then recalculate in LibreOffice/Excel).
"""
import csv, os, re, sys
from openpyxl import Workbook
from openpyxl.chart import BarChart, Reference
from openpyxl.chart.label import DataLabelList
from openpyxl.comments import Comment
from openpyxl.worksheet.formula import ArrayFormula
from openpyxl.formatting.rule import CellIsRule, ColorScaleRule, FormulaRule
from openpyxl.styles import Alignment, Border, Font, PatternFill, Side
from openpyxl.utils import get_column_letter as L

SP = os.environ.get("BENCH_DIR") or os.path.dirname(os.path.abspath(__file__))
OUT = sys.argv[1] if len(sys.argv) > 1 else SP + '/BENCHMARK_pcm_vs_main.xlsx'
runs = list(csv.DictReader(open(SP + '/bench/runs.csv')))
probe = list(csv.DictReader(open(SP + '/probe/probe.csv')))

# ---------------------------------------------------------------- styling
FONT = 'Arial'
INK, INK2, MUTED = '0B0B0B', '52514E', '8A8985'
SERIES = ['2A78D6', 'EB6834', '1BAF7A', 'EDA100', 'E87BA4', '6250D6']   # fixed categorical order
HEAD = PatternFill('solid', fgColor='1F3864')
BAND = PatternFill('solid', fgColor='EEF2F8')
NOTE = PatternFill('solid', fgColor='FFF8E1')
GOOD = PatternFill('solid', fgColor='C6EFCE')
WARN = PatternFill('solid', fgColor='FFEB9C')
BAD = PatternFill('solid', fgColor='FFC7CE')
thin = Side(style='thin', color='D0D0D0')
BOX = Border(left=thin, right=thin, top=thin, bottom=thin)
F = lambda **k: Font(name=FONT, **k)

def title(ws, text, sub=None, row=1):
    ws.cell(row=row, column=1, value=text).font = F(bold=True, size=15, color=INK)
    if sub:
        c = ws.cell(row=row + 1, column=1, value=sub)
        c.font = F(italic=True, size=10, color=INK2)
    return row + (3 if sub else 2)

def para(ws, row, text, width_cols=10, bold=False, fill=None, size=10, height=None):
    ws.merge_cells(start_row=row, start_column=1, end_row=row, end_column=width_cols)
    c = ws.cell(row=row, column=1, value=text)
    c.font = F(bold=bold, size=size, color=INK)
    c.alignment = Alignment(wrap_text=True, vertical='top')
    if fill: c.fill = fill
    if height: ws.row_dimensions[row].height = height
    return row + 1

def header(ws, row, cols, start=1, height=48):
    for i, h in enumerate(cols):
        c = ws.cell(row=row, column=start + i, value=h)
        c.font = F(bold=True, color='FFFFFF', size=10)
        c.fill = HEAD
        c.alignment = Alignment(wrap_text=True, vertical='center', horizontal='center')
        c.border = BOX
    ws.row_dimensions[row].height = height

def cell(ws, row, col, value, fmt=None, bold=False, align=None, fill=None, color=INK):
    c = ws.cell(row=row, column=col, value=value)
    c.font = F(bold=bold, color=color, size=10)
    c.border = BOX
    if fmt: c.number_format = fmt
    if align: c.alignment = Alignment(horizontal=align, vertical='center', wrap_text=True)
    if fill: c.fill = fill
    return c

def widths(ws, ws_widths):
    for i, w in enumerate(ws_widths, 1): ws.column_dimensions[L(i)].width = w

PCT, PCT1, NUM1 = '0%', '0.0%', '0.0'

# ---------------------------------------------------------------- data model
CONFIGS = [  # key, short label, long description
    ('main', 'Main branch (as-is)',
     'sideinfo-mainbits: frame-structure graph + side-info / partial-spectrum ranking + depth-first search.'),
    ('mainfix', 'Main + last-chunk fix',
     'Main with one shared bug fix (a last chunk holding only the end of the final frame was rejected). Isolates that fix from the PCM work.'),
    ('pcm', 'PCM branch (first version)',
     'full-pcm-decode @1446fe6: every candidate junction decoded to audio; spectral-flux score used for ranking, edge locking and repair.'),
    ('final', 'PCM branch (final)',
     'full-pcm-decode @1c5abe1: + faster/threaded locking with a fixed cap, bounded repair, safer fallbacks. This is the version in the pull request.'),
    ('gm1', 'Global matching (after search)',
     'global-matching @9d705fe: when the search fails, choose every successor at once by minimum summed flux, then repair.'),
    ('gm2', 'Global matching (first)',
     'global-matching @9d705fe: global matching tried before the search.'),
]
present = {r['config'] for r in runs}
_only = os.environ.get('BENCH_CONFIGS')   # e.g. main,mainfix,pcm,final
if _only:
    keep = set(_only.split(','))
    runs = [r for r in runs if r['config'] in keep or not any(r['config'] == k or r['config'] == 'ho_' + k for k in ('gm1', 'gm2'))]
    runs = [r for r in runs if r['config'] not in ('gm1', 'gm2', 'ho_gm1', 'ho_gm2') or r['config'] in keep]
    present = {r['config'] for r in runs}
CONFIGS = [c for c in CONFIGS if c[0] in present]
MAINCMP = [c for c in CONFIGS]                     # compared everywhere
REF, BEST = 'mainfix', 'final'

def fkey(f):
    m = re.match(r'(cbr|vbrq)(\d+)_(\d+)s', f)
    if m: return (0 if m.group(1) == 'cbr' else 1, int(m.group(3)), int(m.group(2)))
    return (2, 0, 0 if f.startswith('bohemoan') else 1)

FILES = sorted({r['file'] for r in runs if r['config'] == 'main'}, key=fkey)
SIZES = [1024, 2048, 4096, 8192]

def describe(f):
    """(display name, mode, bitrate setting, length s)"""
    m = re.match(r'cbr(\d+)_(\d+)s', f)
    if m: return (f'{m.group(1)} kbps constant, {m.group(2)} s', 'Constant (CBR)', f'{m.group(1)} kbps', int(m.group(2)))
    m = re.match(r'vbrq(\d+)_(\d+)s', f)
    if m:
        q = {'2': 'high quality (~190 kbps)', '5': 'medium quality (~130 kbps)'}[m.group(1)]
        return (f'Variable q{m.group(1)}, {m.group(2)} s', 'Variable (VBR)', f'q{m.group(1)} - {q}', int(m.group(2)))
    if f.startswith('bohemoan'): return ('Full song, 32 kbps constant (6 min)', 'Constant (CBR)', '32 kbps', 359)
    if f.startswith('Queen'): return ('Full song, variable (6 min)', 'Variable (VBR)', 'LAME VBR (~121 kbps avg)', 359)
    return (f, '?', '?', 0)

def num(v):
    if v in (None, '', 'None'): return None
    if v in ('True', 'False'): return v == 'True'
    try:
        x = float(v); return int(x) if x.is_integer() and '.' not in v else x
    except ValueError: return v

OUTCOME_TEXT = {'EXACT': 'Exact', 'COMPLETE_WRONG': 'Wrong order', 'FAILED': 'Failed', 'TIMEOUT': 'Timed out', 'ERROR': 'Crashed'}
CFG_LABEL = {k: lab for k, lab, _ in CONFIGS}
CFG_LABEL.update({'ho_mainfix': 'Main + last-chunk fix', 'ho_final': 'PCM branch (final)',
                  'ho_gm1': 'Global matching (after search)', 'ho_gm2': 'Global matching (first)',
                  'mainfix150': 'Main + fix, 150 s budget', 'ho_mainfix150': 'Main + fix, 150 s budget',
                  'final_nopcm': 'PCM final without PCM ranking', 'final_nolock': 'PCM final without locking',
                  'final_norepair': 'PCM final without repair', 'final_tau15': 'PCM final, strict locking only',
                  'pcm_nopcm': 'PCM v1 without PCM ranking', 'pcm_nolock': 'PCM v1 without locking',
                  'pcm_norepair': 'PCM v1 without repair', 'pcm_tau15': 'PCM v1, strict locking only'})

wb = Workbook()

# ================================================================ All runs (data)
wr = wb.active
wr.title = 'All runs'
RCOLS = [  # (key, header, width, fmt, description)
    ('config', 'Config key', 14, None, 'Internal name of the configuration (see Read me).'),
    ('label', 'Algorithm / configuration', 32, None, 'What was run.'),
    ('set', 'Test set', 10, None, 'main = segment from 120 s + full songs; held-out = segment from 200 s (never used for tuning).'),
    ('file', 'Test file key', 26, None, 'File name of the test MP3.'),
    ('fdesc', 'Test file', 30, None, 'Readable description of the file.'),
    ('mode', 'Bitrate mode', 14, None, 'Constant bitrate (every frame the same size) or variable.'),
    ('rate', 'Bitrate setting', 22, None, 'Encoder setting.'),
    ('duration_s', 'Length (s)', 8, '0', 'Length of the audio.'),
    ('chunk_kb', 'Chunk size (KB)', 8, '0', 'The file is cut into pieces of this size and shuffled.'),
    ('seed', 'Shuffle seed', 7, '0', 'Which random shuffle (1-3).'),
    ('chunks', 'Chunks', 8, '#,##0', 'How many pieces the file was cut into.'),
    ('outcome_text', 'Result', 12, None, 'Exact = byte-identical original; Wrong order = a complete but wrong file; Failed = no complete order found in time.'),
    ('exact', 'Exact (1/0)', 7, '0', '1 if the output is byte-identical to the original.'),
    ('wrong', 'Wrong order (1/0)', 7, '0', '1 if a complete but wrong order was output.'),
    ('recovered_frac', 'Junctions recovered', 10, PCT, 'Share of the true chunk-to-chunk links (chunks - 1 of them) present in the result, or in the deepest partial result if it failed.'),
    ('exact_pos_pct', 'Chunks in original position (%)', 10, '0.0', 'Only for complete results.'),
    ('wall_s', 'Run time (s)', 9, '0.0', 'Wall-clock time (machine shared by several runs - indicative only; a few runs span a machine suspend and show inflated times).'),
    ('supernodes', 'Search units after merging', 10, '#,##0', 'Pieces left for the search after certain links were merged.'),
    ('edges', 'Possible links (graph edges)', 10, '#,##0', 'Chunk pairs that are structurally allowed to follow each other.'),
    ('locks', 'Links locked by PCM', 9, '#,##0', 'Links forced before the search because the audio evidence was very clear.'),
    ('locks_wrong', 'Of which wrong', 8, '0', 'Locked links that were not true (checked against the original).'),
    ('final_tau', 'Lock threshold used', 9, '0.00', 'Strictness of the last locking attempt (1.5 strict ... 1.1 loose).'),
    ('repair_moves', 'Repair moves', 8, '0', 'Block moves/swaps applied by the repair step.'),
    ('pre_breaks', 'Structural breaks in true order', 9, '0', 'Pairs of the TRUE order the structure check rejects (>0 makes an exact result impossible).'),
]
header(wr, 1, [c[1] for c in RCOLS])
for i, c in enumerate(RCOLS, 1):
    wr.column_dimensions[L(i)].width = c[2]
    wr.cell(row=1, column=i).comment = Comment(c[4], 'benchmark')
order_keys = [c[0] for c in CONFIGS]
def rsort(r):
    k = r['config']
    return (order_keys.index(k) if k in order_keys else 50, k, fkey(r['file']), int(r['chunk_size']), int(r['seed']))
for ri, r in enumerate(sorted(runs, key=rsort), 2):
    desc = describe(r['file'])
    locks, lc = num(r['locks']), num(r['locks_correct'])
    vals = {
        'config': r['config'], 'label': CFG_LABEL.get(r['config'], r['config']),
        'set': 'held-out' if r['config'].startswith('ho_') else 'main', 'file': r['file'],
        'fdesc': desc[0], 'mode': desc[1], 'rate': desc[2], 'duration_s': desc[3],
        'chunk_kb': int(r['chunk_size']) // 1024, 'seed': int(r['seed']), 'chunks': num(r['chunks']),
        'outcome_text': OUTCOME_TEXT.get(r['outcome'], r['outcome']),
        'exact': 1 if r['outcome'] == 'EXACT' else 0, 'wrong': 1 if r['outcome'] == 'COMPLETE_WRONG' else 0,
        'recovered_frac': num(r['recovered_frac']), 'exact_pos_pct': num(r['exact_pos_pct']),
        'wall_s': num(r['wall_s']), 'supernodes': num(r['supernodes']), 'edges': num(r['edges']),
        'locks': locks, 'locks_wrong': (locks - lc) if isinstance(locks, int) and isinstance(lc, int) else None,
        'final_tau': num(r['final_tau']), 'repair_moves': num(r['repair_moves']), 'pre_breaks': num(r['pre_breaks']),
    }
    for ci, c in enumerate(RCOLS, 1):
        x = wr.cell(row=ri, column=ci, value=vals[c[0]])
        x.font = F(size=9)
        if c[3]: x.number_format = c[3]
NR = len(runs) + 1
wr.freeze_panes = 'F2'
wr.auto_filter.ref = f'A1:{L(len(RCOLS))}{NR}'
COL = {c[0]: L(i) for i, c in enumerate(RCOLS, 1)}
def R(k): return f"'All runs'!${COL[k]}$2:${COL[k]}${NR}"
for v, fill in (('"Exact"', GOOD), ('"Wrong order"', WARN), ('"Failed"', BAD), ('"Crashed"', BAD), ('"Timed out"', BAD)):
    wr.conditional_formatting.add(f'{COL["outcome_text"]}2:{COL["outcome_text"]}{NR}',
                                  CellIsRule(operator='equal', formula=[v], fill=fill))

def crit(cfg, extra=''):
    return f'{R("config")},"{cfg}"' + extra

# ================================================================ Read me
rm = wb.create_sheet('Read me', 0)
widths(rm, [26, 14, 14, 14, 14, 14, 14, 14, 14, 14])
r = title(rm, 'MP3 chunk reconstruction - benchmark of the PCM-decode branch',
          'Every number on the summary sheets is a formula over the sheet "All runs" (one row per run).')
r = para(rm, r, 'What is being tested', bold=True, size=12)
r = para(rm, r, 'An MP3 file is cut into equal pieces ("chunks", 1-8 KB), the pieces are shuffled, and the program has to put '
                'them back into the original order using only what is inside the file. A run is a success only if the '
                'rebuilt file is byte-for-byte identical to the original ("Exact"). The first chunk always stays first.',
         height=48)
r = para(rm, r, 'How each algorithm works (in short)', bold=True, size=12)
for k, lab, d in CONFIGS:
    lc_ = rm.cell(row=r, column=1, value=lab); lc_.font = F(bold=True, size=10); lc_.alignment = Alignment(vertical='top', wrap_text=True)
    rm.merge_cells(start_row=r, start_column=2, end_row=r, end_column=10)
    c = rm.cell(row=r, column=2, value=d); c.font = F(size=10); c.alignment = Alignment(wrap_text=True, vertical='top')
    rm.row_dimensions[r].height = 30
    r += 1
r += 1
r = para(rm, r, 'Possible results of one run', bold=True, size=12)
for lab, d, fill in (('Exact', 'The output file is identical to the original. The only real success.', GOOD),
                     ('Wrong order', 'A complete file that plays, but some pieces are in the wrong place. Worse than failing: it looks like a success.', WARN),
                     ('Failed', 'No complete order found within the time budget; nothing is written. The "Junctions recovered" column then '
                                'describes the longest partial result.', BAD)):
    c = rm.cell(row=r, column=1, value=lab); c.font = F(bold=True, size=10); c.fill = fill; c.alignment = Alignment(vertical='top')
    rm.merge_cells(start_row=r, start_column=2, end_row=r, end_column=10)
    c = rm.cell(row=r, column=2, value=d); c.font = F(size=10); c.alignment = Alignment(wrap_text=True, vertical='top')
    rm.row_dimensions[r].height = 28
    r += 1
r += 1
r = para(rm, r, 'Headline numbers (main test set, 3 shuffles of every file x chunk size)', bold=True, size=12)
header(rm, r, ['Algorithm', 'Runs', 'Exact', 'Exact rate', 'Wrong order', 'Failed', 'Junctions recovered (avg)'], height=34)
r += 1
for k, lab, _ in CONFIGS:
    cs = crit(k, f',{R("set")},"main"')
    cell(rm, r, 1, lab, bold=True)
    cell(rm, r, 2, f'=COUNTIFS({cs})', '0', align='center')
    cell(rm, r, 3, f'=COUNTIFS({cs},{R("outcome_text")},"Exact")', '0', align='center')
    cell(rm, r, 4, f'=IF(B{r}=0,"-",C{r}/B{r})', PCT, align='center', bold=True)
    cell(rm, r, 5, f'=COUNTIFS({cs},{R("outcome_text")},"Wrong order")', '0', align='center')
    cell(rm, r, 6, f'=B{r}-C{r}-E{r}', '0', align='center')
    cell(rm, r, 7, f'=IFERROR(AVERAGEIFS({R("recovered_frac")},{cs}),"-")', PCT, align='center')
    r += 1
r += 1
r = para(rm, r, 'What the sheets contain', bold=True, size=12)
for name, d in (('Overview', 'The headline table above, plus held-out, equal-time and component results, with a chart.'),
                ('By chunk size', 'Exact rate per chunk size (1, 2, 4, 8 KB), split into constant and variable bitrate. Chart.'),
                ('By bitrate', 'Exact rate per encoding setting (32-320 kbps constant, two variable settings, two full songs). Chart.'),
                ('Per file', 'One row per test file and chunk size: what each algorithm achieved, in words, plus a verdict.'),
                ('Components', 'What happens when one part of the PCM branch is switched off (ablation).'),
                ('Held-out', 'Second set of test files cut from a different part of the song, never used to tune anything.'),
                ('Equal time', 'Main given 5x more search time on the cases only the PCM branch solved.'),
                ('Signal quality', 'How well each scoring signal recognises the true next chunk, measured on the original order.'),
                ('All runs', 'Raw data: one row per run. Hover a column header for its definition.')):
    rm.cell(row=r, column=1, value=name).font = F(bold=True, size=10, color='1F3864')
    rm.merge_cells(start_row=r, start_column=2, end_row=r, end_column=10)
    c = rm.cell(row=r, column=2, value=d); c.font = F(size=10); c.alignment = Alignment(wrap_text=True)
    r += 1
r += 1
r = para(rm, r, 'Test files', bold=True, size=12)
r = para(rm, r, 'Short files: cut from "Bohemian Rhapsody" (from 120 s; held-out set from 200 s) and re-encoded with LAME at 44.1 kHz: '
                'constant 32 / 64 / 128 / 192 / 320 kbps and variable quality 2 / 5, each 10 s and 30 s long '
                '(sound/test/make_bitrate_variants.sh). Full songs: a 6-minute 32 kbps constant file and a 6-minute variable file. '
                'The three bohemoan-rhapsody-2k/4k/8k files are byte-identical, so that song was tested once per chunk size. '
                '320 kbps with 1 KB chunks is out of scope: its frames (1045 bytes) are longer than a chunk.', height=74)
r = para(rm, r, 'Fairness notes', bold=True, size=12)
r = para(rm, r, 'Every run had a 30 s search budget and a 900 s hard limit. The PCM branch may make up to three locking attempts '
                '(30 s each) plus a repair step, so it can take longer - the "Equal time" sheet checks whether main simply needed '
                'more time (it did not). Runs shared a 16-core machine, so times are indicative only. Some PCM-final runs were '
                'repeated on the final binary after fixes; the Notes in the report (BENCHMARK_pcm_vs_main.md) list exactly which.', height=74)

# ================================================================ Overview
ov = wb.create_sheet('Overview', 1)
widths(ov, [34, 11, 11, 11, 11, 11, 13, 13, 4, 12])
r = title(ov, 'Overview', 'Main test set: 16 files x 4 chunk sizes x 3 shuffles = 192 runs per algorithm.')
header(ov, r, ['Algorithm', 'Runs', 'Exact', 'Exact rate', 'Wrong order', 'Failed', 'Junctions recovered (avg)', 'Median run time (s)'], height=40)
hr = r; r += 1
first = r
for k, lab, _ in CONFIGS:
    cs = crit(k, f',{R("set")},"main"')
    cell(ov, r, 1, lab, bold=True)
    cell(ov, r, 2, f'=COUNTIFS({cs})', '0', align='center')
    cell(ov, r, 3, f'=COUNTIFS({cs},{R("outcome_text")},"Exact")', '0', align='center')
    cell(ov, r, 4, f'=IF(B{r}=0,0,C{r}/B{r})', PCT, align='center', bold=True)
    cell(ov, r, 5, f'=COUNTIFS({cs},{R("outcome_text")},"Wrong order")', '0', align='center')
    cell(ov, r, 6, f'=B{r}-C{r}-E{r}', '0', align='center')
    cell(ov, r, 7, f'=IFERROR(AVERAGEIFS({R("recovered_frac")},{cs}),0)', PCT, align='center')
    c8 = cell(ov, r, 8, None, NUM1, align='center')
    c8.value = ArrayFormula(f'H{r}', f'=MEDIAN(IF(({R("config")}="{k}")*({R("set")}="main"),{R("wall_s")}))')
    r += 1
last = r - 1
ov.cell(row=hr, column=8, value='Median run time (s)')
ov.cell(row=r, column=1, value='Junctions recovered = share of true chunk-to-chunk links in the result (or in the longest partial result when the run failed).').font = F(italic=True, size=9, color=INK2)
r += 1
ov.cell(row=r, column=1, value='Run time: median, because a few runs span a machine suspend and their recorded wall time (up to ~5800 s) is not real compute time.').font = F(italic=True, size=9, color=INK2)
r += 2
ch = BarChart(); ch.type = 'bar'; ch.style = 10
ch.title = 'Exact reconstructions (share of 192 runs)'
ch.y_axis.title = None; ch.x_axis.title = None
data = Reference(ov, min_col=4, min_row=hr, max_row=last)
cats = Reference(ov, min_col=1, min_row=first, max_row=last)
ch.add_data(data, titles_from_data=True); ch.set_categories(cats)
ch.legend = None
ch.y_axis.number_format = '0%'; ch.y_axis.scaling.min = 0; ch.y_axis.scaling.max = 1
ch.y_axis.majorGridlines = None
s = ch.series[0]
s.graphicalProperties.solidFill = SERIES[0]; s.graphicalProperties.line.noFill = True
from openpyxl.chart.series import DataPoint
for i in range(len(CONFIGS)):
    pt = DataPoint(idx=i); pt.graphicalProperties.solidFill = SERIES[i]; pt.graphicalProperties.line.noFill = True
    s.dPt.append(pt)
s.dLbls = DataLabelList(); s.dLbls.showVal = True; s.dLbls.numFmt = '0%'
s.dLbls.showSerName = False; s.dLbls.showCatName = False; s.dLbls.showLegendKey = False
ch.height = 7.5; ch.width = 18
ov.add_chart(ch, f'A{r}')
r += 17

def small_table(ws, r, heading, explain, rows_spec, cols):
    """rows_spec: list of (label, criteria-string); cols: Runs/Exact/Exact rate/Wrong/Failed/Recovered"""
    ws.cell(row=r, column=1, value=heading).font = F(bold=True, size=12)
    r += 1
    r = para(ws, r, explain, width_cols=8, fill=NOTE, height=32)
    header(ws, r, ['Algorithm', 'Runs', 'Exact', 'Exact rate', 'Wrong order', 'Failed', 'Junctions recovered (avg)'], height=34)
    r += 1
    for lab, cs in rows_spec:
        cell(ws, r, 1, lab, bold=True)
        cell(ws, r, 2, f'=COUNTIFS({cs})', '0', align='center')
        cell(ws, r, 3, f'=COUNTIFS({cs},{R("outcome_text")},"Exact")', '0', align='center')
        cell(ws, r, 4, f'=IF(B{r}=0,"-",C{r}/B{r})', PCT, align='center', bold=True)
        cell(ws, r, 5, f'=COUNTIFS({cs},{R("outcome_text")},"Wrong order")', '0', align='center')
        cell(ws, r, 6, f'=B{r}-C{r}-E{r}', '0', align='center')
        cell(ws, r, 7, f'=IFERROR(AVERAGEIFS({R("recovered_frac")},{cs}),"-")', PCT, align='center')
        r += 1
    return r + 1

ho_rows = [(CFG_LABEL[k], crit(k)) for k in ('ho_mainfix', 'ho_final', 'ho_gm1', 'ho_gm2') if k in present]
if ho_rows:
    r = small_table(ov, r, 'Held-out test set (seed 1)',
                    'Same 14 settings cut from a different part of the song (from 200 s). Nothing was tuned on these files, so this checks '
                    'that the improvement is not specific to the main test files.', ho_rows, None)
abl = [(CFG_LABEL.get(k, k), crit(k, f',{R("seed")},1')) for k in ('final', 'final_nopcm', 'final_nolock', 'final_norepair', 'final_tau15', 'mainfix') if k in present]
abl[0] = ('PCM branch (final), everything on', abl[0][1])
r = small_table(ov, r, 'Which part of the PCM branch matters? (seed 1, 64 cases)',
                'One component switched off at a time. A drop in exact results shows how much that component contributes. '
                'Main + fix is shown for reference.', abl, None)

# ================================================================ By chunk size
bc = wb.create_sheet('By chunk size', 2)
widths(bc, [16, 12] + [13] * (2 * len(CONFIGS)))
r = title(bc, 'Results by chunk size', 'Exact rate = share of runs (all files of that bitrate mode, 3 shuffles) rebuilt byte-for-byte. Larger chunks = fewer, more distinctive pieces.')
chart_rows = {}
for mode in ('Constant (CBR)', 'Variable (VBR)', 'All files'):
    bc.cell(row=r, column=1, value=mode).font = F(bold=True, size=12); r += 1
    header(bc, r, ['Chunk size', 'Runs per algorithm'] + [f'{lab}\nexact rate' for _, lab, _ in CONFIGS] +
           [f'{lab}\njunctions recovered' for _, lab, _ in CONFIGS], height=60)
    hr = r; r += 1
    for cs in SIZES:
        mcrit = '' if mode == 'All files' else f',{R("mode")},"{mode}"'
        base = f',{R("set")},"main",{R("chunk_kb")},{cs // 1024}{mcrit}'
        cell(bc, r, 1, f'{cs // 1024} KB', bold=True, align='center')
        cell(bc, r, 2, f'=COUNTIFS({crit("main", base)})', '0', align='center')
        for j, (k, _, _) in enumerate(CONFIGS):
            cs_ = crit(k, base)
            cell(bc, r, 3 + j, f'=IFERROR(COUNTIFS({cs_},{R("outcome_text")},"Exact")/COUNTIFS({cs_}),"-")', PCT, align='center')
            cell(bc, r, 3 + len(CONFIGS) + j, f'=IFERROR(AVERAGEIFS({R("recovered_frac")},{cs_}),"-")', PCT, align='center')
        r += 1
    chart_rows[mode] = (hr, r - 1)
    bc.conditional_formatting.add(f'C{hr+1}:{L(2 + 2 * len(CONFIGS))}{r-1}',
        ColorScaleRule(start_type='num', start_value=0, start_color='F8696B', mid_type='num', mid_value=0.5,
                       mid_color='FFEB84', end_type='num', end_value=1, end_color='63BE7B'))
    r += 1

def grouped_bar(ws, hr, lr, cat_col, first_col, n, title_, anchor, height=8, width=22):
    ch = BarChart(); ch.type = 'col'; ch.grouping = 'clustered'; ch.style = 10
    ch.title = title_
    for j in range(n):
        ref = Reference(ws, min_col=first_col + j, min_row=hr, max_row=lr)
        ch.add_data(ref, titles_from_data=True)
        ch.series[-1].graphicalProperties.solidFill = SERIES[j]
        ch.series[-1].graphicalProperties.line.noFill = True
    ch.set_categories(Reference(ws, min_col=cat_col, min_row=hr + 1, max_row=lr))
    ch.y_axis.number_format = '0%'; ch.y_axis.scaling.min = 0; ch.y_axis.scaling.max = 1
    ch.y_axis.majorGridlines = None
    ch.legend.position = 'b'
    ch.height = height; ch.width = width
    ws.add_chart(ch, anchor)

hr, lr = chart_rows['All files']
grouped_bar(bc, hr, lr, 1, 3, len(CONFIGS), 'Exact rate by chunk size (all files)', f'A{r + 1}')

# ================================================================ By bitrate
bb = wb.create_sheet('By bitrate', 3)
widths(bb, [34, 16, 10] + [13] * (2 * len(CONFIGS)))
r = title(bb, 'Results by encoding setting', 'All chunk sizes and lengths of that setting, 3 shuffles each. "Full song" rows are the 6-minute files.')
groups = []
for k in (32, 64, 128, 192, 320):
    groups.append((f'{k} kbps constant (10 s + 30 s files)', 'Constant (CBR)', [f for f in FILES if f.startswith(f'cbr{k}_')]))
for q, d in (('2', 'high quality'), ('5', 'medium quality')):
    groups.append((f'Variable q{q}, {d} (10 s + 30 s files)', 'Variable (VBR)', [f for f in FILES if f.startswith(f'vbrq{q}_')]))
groups.append(('Full song, 32 kbps constant (6 min)', 'Constant (CBR)', [f for f in FILES if f.startswith('bohemoan')]))
groups.append(('Full song, variable (6 min)', 'Variable (VBR)', [f for f in FILES if f.startswith('Queen')]))
header(bb, r, ['Encoding setting', 'Mode', 'Runs per algorithm'] + [f'{lab}\nexact rate' for _, lab, _ in CONFIGS] +
       [f'{lab}\njunctions recovered' for _, lab, _ in CONFIGS], height=60)
hr = r; r += 1
for lab_, mode, fl in groups:
    cell(bb, r, 1, lab_, bold=True)
    cell(bb, r, 2, mode, align='center')
    def gc(k):
        return [crit(k, f',{R("set")},"main",{R("file")},"{f}"') for f in fl]
    cell(bb, r, 3, '=' + '+'.join(f'COUNTIFS({c})' for c in gc('main')), '0', align='center')
    for j, (k, _, _) in enumerate(CONFIGS):
        ex = '+'.join(f'COUNTIFS({c},{R("outcome_text")},"Exact")' for c in gc(k))
        n_ = '+'.join(f'COUNTIFS({c})' for c in gc(k))
        cell(bb, r, 4 + j, f'=IFERROR(({ex})/({n_}),"-")', PCT, align='center')
        sm = '+'.join(f'SUMIFS({R("recovered_frac")},{c})' for c in gc(k))
        cell(bb, r, 4 + len(CONFIGS) + j, f'=IFERROR(({sm})/({n_}),"-")', PCT, align='center')
    r += 1
bb.conditional_formatting.add(f'D{hr+1}:{L(3 + 2 * len(CONFIGS))}{r-1}',
    ColorScaleRule(start_type='num', start_value=0, start_color='F8696B', mid_type='num', mid_value=0.5,
                   mid_color='FFEB84', end_type='num', end_value=1, end_color='63BE7B'))
r += 1
grouped_bar(bb, hr, r - 2, 1, 4, len(CONFIGS), 'Exact rate by encoding setting', f'A{r}', height=9, width=28)

# ================================================================ Per file
pf = wb.create_sheet('Per file', 4)
r = title(pf, 'Per test file and chunk size', 'Each cell summarises 3 shuffles. The verdict compares the PCM branch (final) with main + last-chunk fix.')
cols = ['Test file', 'Mode', 'Chunk size (KB)', 'Chunks', 'Possible next chunks per junction (avg)']
for _, lab, _ in CONFIGS: cols += [f'{lab}\nresult', f'{lab}\njunctions recovered']
cols += ['Verdict (PCM final vs main + fix)']
header(pf, r, cols, height=62)
widths(pf, [34, 14, 9, 8, 12] + [17, 11] * len(CONFIGS) + [26])
hr = r; r += 1
pkey = {(p['file'], int(p['chunk_size'])): p for p in probe}
for f in FILES:
    d = describe(f)
    for cs in SIZES:
        cell(pf, r, 1, d[0], bold=True)
        cell(pf, r, 2, d[1], align='center')
        cell(pf, r, 3, cs // 1024, '0', align='center')
        cell(pf, r, 4, f'=IFERROR(AVERAGEIFS({R("chunks")},{R("file")},"{f}",{R("chunk_kb")},{cs // 1024}),"-")', '#,##0', align='center')
        p = pkey.get((f, cs))
        if f.startswith('cbr320') and cs == 1024: candv = 'n/a (frames > chunk)'
        elif p is None or p.get('junctions_deg2') in (None, ''): candv = 'not measured'
        elif p.get('junctions_deg2') in (None, '', '0'): candv = '1 (no choice)'
        else: candv = round(2 * num(p['flux_baseline']) - 1, 1)
        cell(pf, r, 5, candv, '0.0', align='center')
        col = 6
        for k, lab, _ in CONFIGS:
            base = crit(k, f',{R("set")},"main",{R("file")},"{f}",{R("chunk_kb")},{cs // 1024}')
            ex = f'COUNTIFS({base},{R("outcome_text")},"Exact")'
            wr_ = f'COUNTIFS({base},{R("outcome_text")},"Wrong order")'
            n_ = f'COUNTIFS({base})'
            cell(pf, r, col, f'=IF({n_}=0,"-",IF({ex}={n_},"Exact (all "&{n_}&")",IF({ex}>0,"Exact "&{ex}&" of "&{n_},'
                             f'IF({wr_}>0,"Wrong order","Failed"))))', align='center')
            cell(pf, r, col + 1, f'=IFERROR(AVERAGEIFS({R("recovered_frac")},{base}),"-")', PCT, align='center')
            col += 2
        ref_c = 6 + 2 * [c[0] for c in CONFIGS].index(REF)
        best_c = 6 + 2 * [c[0] for c in CONFIGS].index(BEST)
        cellc = f',{R("set")},"main",{R("file")},"{f}",{R("chunk_kb")},{cs // 1024}'
        exr = f'COUNTIFS({crit(REF, cellc)},{R("outcome_text")},"Exact")'
        exb = f'COUNTIFS({crit(BEST, cellc)},{R("outcome_text")},"Exact")'
        recr, recb = f'{L(ref_c + 1)}{r}', f'{L(best_c + 1)}{r}'
        cell(pf, r, col, f'=IF({exb}>{exr},"PCM better (more exact)",IF({exb}<{exr},"PCM worse",'
                         f'IF({exb}=3,"Both exact",IF(AND(ISNUMBER({recb}),ISNUMBER({recr}),{recb}>{recr}+0.05),'
                         f'"Neither exact - PCM closer","Same"))))', align='center')
        if cs == SIZES[-1]:
            for cc in range(1, col + 1): pf.cell(row=r, column=cc).border = Border(left=thin, right=thin, top=thin, bottom=Side(style='medium', color='1F3864'))
        r += 1
lastpf = r - 1
rc = f'F{hr+1}:{L(5 + 2 * len(CONFIGS))}{lastpf}'
pf.conditional_formatting.add(rc, FormulaRule(formula=[f'LEFT(F{hr+1},5)="Exact"'], fill=GOOD))
pf.conditional_formatting.add(rc, FormulaRule(formula=[f'F{hr+1}="Wrong order"'], fill=WARN))
pf.conditional_formatting.add(rc, FormulaRule(formula=[f'F{hr+1}="Failed"'], fill=BAD))
vc = L(6 + 2 * len(CONFIGS))
pf.conditional_formatting.add(f'{vc}{hr+1}:{vc}{lastpf}', FormulaRule(formula=[f'LEFT({vc}{hr+1},10)="PCM better"'], fill=GOOD))
pf.conditional_formatting.add(f'{vc}{hr+1}:{vc}{lastpf}', FormulaRule(formula=[f'{vc}{hr+1}="PCM worse"'], fill=BAD))
pf.freeze_panes = pf.cell(row=hr + 1, column=6)
r += 1
pf.cell(row=r, column=1, value='Verdict counts').font = F(bold=True, size=11); r += 1
for v in ('PCM better (more exact)', 'Neither exact - PCM closer', 'Both exact', 'Same', 'PCM worse'):
    cell(pf, r, 1, v); cell(pf, r, 2, f'=COUNTIF({vc}{hr+1}:{vc}{lastpf},"{v}")', '0', align='center'); r += 1
r = para(pf, r + 1, '"Possible next chunks per junction" = average number of structurally allowed successors at the points where '
                    'there is a choice (from pcm_probe). Higher = harder. "Junctions recovered" = share of the true links present in '
                    'the result or in the longest partial result.', width_cols=12, fill=NOTE, height=30)

# ================================================================ Components (ablation detail)
cp = wb.create_sheet('Components', 5)
r = title(cp, 'Contribution of each component (seed 1)', 'Each row switches one part of the PCM branch (final) off. Fewer exact results = that part was helping.')
COMP = [('final', 'Nothing switched off', 'Full PCM branch.'),
        ('final_nopcm', 'Audio (PCM) ranking off', 'Candidates are no longer ordered by the audio-continuity score; locking and repair still use it.'),
        ('final_nolock', 'Edge locking off', 'No links are forced before the search.'),
        ('final_norepair', 'Repair off', 'The block-move / swap correction after the search is skipped.'),
        ('final_tau15', 'Only strict locking', 'Locking threshold fixed at 1.5 - no looser second and third attempts.'),
        ('mainfix', 'Reference: main + fix', 'The main branch (with the shared fix), for comparison.')]
header(cp, r, ['Configuration', 'What changes', 'Exact (of 64)', 'Change vs full PCM', 'Wrong order', 'Failed', 'Junctions recovered (avg)'], height=40)
widths(cp, [28, 60, 11, 12, 11, 9, 13])
r += 1; full_r = r
for k, lab, d in COMP:
    if k not in present: continue
    cs_ = crit(k, f',{R("seed")},1,{R("set")},"main"')
    cell(cp, r, 1, lab, bold=True); cell(cp, r, 2, d)
    cell(cp, r, 3, f'=COUNTIFS({cs_},{R("outcome_text")},"Exact")', '0', align='center')
    cell(cp, r, 4, f'=C{r}-C${full_r}', '+0;-0;0', align='center')
    cell(cp, r, 5, f'=COUNTIFS({cs_},{R("outcome_text")},"Wrong order")', '0', align='center')
    cell(cp, r, 6, f'=COUNTIFS({cs_})-C{r}-E{r}', '0', align='center')
    cell(cp, r, 7, f'=IFERROR(AVERAGEIFS({R("recovered_frac")},{cs_}),"-")', PCT, align='center')
    cp.row_dimensions[r].height = 30
    r += 1
cp.conditional_formatting.add(f'D{full_r}:D{r-1}', CellIsRule(operator='lessThan', formula=['0'], fill=BAD))

# ================================================================ Held-out (detail)
if ho_rows:
    hs = wb.create_sheet('Held-out', 6)
    r = title(hs, 'Held-out test set - every case (seed 1)', 'Files cut from 200 s of the song with the same 14 encoder settings. Not used for any tuning.')
    hcfg = [k for k in ('ho_mainfix', 'ho_final', 'ho_gm1', 'ho_gm2') if k in present]
    header(hs, r, ['Test file', 'Chunk size (KB)'] + [f'{CFG_LABEL[k]}\nresult' for k in hcfg] + [f'{CFG_LABEL[k]}\njunctions recovered' for k in hcfg] + [f'{CFG_LABEL[k]}\ntime (s)' for k in hcfg], height=60)
    widths(hs, [34, 9] + [16] * (3 * len(hcfg)))
    hr = r; r += 1
    hofiles = sorted({x['file'] for x in runs if x['config'] == 'ho_mainfix'}, key=fkey)
    for f in hofiles:
        for cs in SIZES:
            cell(hs, r, 1, describe(f)[0], bold=True); cell(hs, r, 2, cs // 1024, '0', align='center')
            for j, k in enumerate(hcfg):
                base = crit(k, f',{R("file")},"{f}",{R("chunk_kb")},{cs // 1024}')
                cell(hs, r, 3 + j, f'=IFERROR(INDEX({R("outcome_text")},MATCH(1,INDEX(({R("config")}="{k}")*({R("file")}="{f}")*({R("chunk_kb")}={cs // 1024}),0),0)),"-")', align='center')
                cell(hs, r, 3 + len(hcfg) + j, f'=IFERROR(AVERAGEIFS({R("recovered_frac")},{base}),"-")', PCT, align='center')
                cell(hs, r, 3 + 2 * len(hcfg) + j, f'=IFERROR(AVERAGEIFS({R("wall_s")},{base}),"-")', NUM1, align='center')
            r += 1
    rng_ = f'C{hr+1}:{L(2 + len(hcfg))}{r-1}'
    for v, fill in (('"Exact"', GOOD), ('"Wrong order"', WARN), ('"Failed"', BAD)):
        hs.conditional_formatting.add(rng_, CellIsRule(operator='equal', formula=[v], fill=fill))
    hs.freeze_panes = 'C' + str(hr + 1)

# ================================================================ Equal time
eqk = [k for k in ('mainfix150', 'ho_mainfix150') if k in present]
if eqk:
    et = wb.create_sheet('Equal time', 7)
    r = title(et, 'Was main just short of time?', 'On every case where only the PCM branch (final) was exact, main + fix was re-run with a 150 s budget (5x).')
    header(et, r, ['Test file', 'Test set', 'Chunk size (KB)', 'Main + fix, 30 s', 'Main + fix, 150 s', 'PCM final', 'PCM final time (s)'], height=40)
    widths(et, [34, 10, 9, 15, 15, 15, 12])
    hr = r; r += 1
    eq = sorted({(x['config'], x['file'], int(x['chunk_size'])) for x in runs if x['config'] in eqk}, key=lambda t: (t[0], fkey(t[1]), t[2]))
    def lookup(cfg, f, kb, col='outcome_text'):
        return f'=IFERROR(INDEX({R(col)},MATCH(1,INDEX(({R("config")}="{cfg}")*({R("file")}="{f}")*({R("chunk_kb")}={kb})*({R("seed")}=1),0),0)),"-")'
    for cfg, f, cs in eq:
        ho = cfg.startswith('ho_')
        kb = cs // 1024
        cell(et, r, 1, describe(f)[0], bold=True); cell(et, r, 2, 'held-out' if ho else 'main', align='center')
        cell(et, r, 3, kb, '0', align='center')
        cell(et, r, 4, lookup('ho_mainfix' if ho else 'mainfix', f, kb), align='center')
        cell(et, r, 5, lookup(cfg, f, kb), align='center')
        cell(et, r, 6, lookup('ho_final' if ho else 'final', f, kb), align='center')
        cell(et, r, 7, lookup('ho_final' if ho else 'final', f, kb, 'wall_s'), NUM1, align='center')
        r += 1
    rng_ = f'D{hr+1}:F{r-1}'
    for v, fill in (('"Exact"', GOOD), ('"Wrong order"', WARN), ('"Failed"', BAD)):
        et.conditional_formatting.add(rng_, CellIsRule(operator='equal', formula=[v], fill=fill))
    r += 1
    cell(et, r, 1, 'Main + fix exact with 150 s', bold=True); cell(et, r, 5, f'=COUNTIF(E{hr+1}:E{r-2},"Exact")', '0', align='center', bold=True)
    cell(et, r + 1, 1, 'Cases checked', bold=True); cell(et, r + 1, 5, f'=COUNTA(E{hr+1}:E{r-2})', '0', align='center', bold=True)

# ================================================================ Signal quality
sq = wb.create_sheet('Signal quality', 8)
r = title(sq, 'How well does each signal recognise the true next chunk?',
          'Measured by pcm_probe on the ORIGINAL order: at every point where several chunks could structurally follow, the candidates are ranked by each signal.')
r = para(sq, r, 'PCM flux = the audio-continuity score of the PCM branch. Old spectral score = the partial-spectrum signal (Milestone 2) the main branch uses. '
                '"Ranked first" = the true next chunk got the best score. "Random guess" = what picking at random would give. '
                '"Strict locks" = links the PCM branch would force before searching (threshold 1.5): how many are wrong and what share of links they cover.',
         width_cols=12, fill=NOTE, height=46)
header(sq, r, ['Test file', 'Chunk size (KB)', 'Decision points', 'Possible next chunks (avg)', 'Random guess ranked first',
               'PCM flux: true chunk ranked first', 'Old spectral score: ranked first', 'PCM flux separation (AUC, 0.5 = useless, 1 = perfect)',
               'Swap test: true pair scores better', 'Strict locks: wrong', 'Strict locks: share of links covered'], height=74)
widths(sq, [36, 9, 10, 11, 11, 13, 13, 14, 12, 10, 12])
hr = r; r += 1
for p in sorted(probe, key=lambda p: (fkey(p['file']) if p['file'] in FILES or p['file'].startswith(('cbr', 'vbr')) else (3, 0, p['file']), int(p['chunk_size']))):
    if p.get('junctions_deg2') in (None, '', '0') or num(p.get('flux_baseline')) in (None, 0): continue
    name = describe(p['file'])[0] if (p['file'] in FILES) else f"Older fixture: {p['file']}"
    base = num(p['flux_baseline'])
    cell(sq, r, 1, name, bold=True); cell(sq, r, 2, int(p['chunk_size']) // 1024, '0', align='center')
    cell(sq, r, 3, num(p['junctions_deg2']), '#,##0', align='center')
    cell(sq, r, 4, round(2 * base - 1, 1), '0.0', align='center')
    cell(sq, r, 5, f'=1/D{r}', PCT, align='center', color=INK2)
    cell(sq, r, 6, num(p['flux_rank1_pct']) / 100, PCT, align='center', bold=True)
    cell(sq, r, 7, num(p['m2_rank1_pct']) / 100 if num(p['m2_rank1_pct']) is not None else None, PCT, align='center')
    cell(sq, r, 8, num(p['flux_auc']), '0.00', align='center')
    cell(sq, r, 9, num(p['twoopt_true_cheaper_pct']) / 100 if num(p.get('twoopt_true_cheaper_pct')) is not None else None, PCT, align='center')
    cell(sq, r, 10, num(p.get('lock150_wrong')), '0', align='center')
    cell(sq, r, 11, num(p.get('lock150_cov_pct')) / 100 if num(p.get('lock150_cov_pct')) is not None else None, PCT, align='center')
    r += 1
sq.conditional_formatting.add(f'F{hr+1}:G{r-1}', ColorScaleRule(start_type='num', start_value=0, start_color='F8696B',
                              mid_type='num', mid_value=0.5, mid_color='FFEB84', end_type='num', end_value=1, end_color='63BE7B'))
sq.conditional_formatting.add(f'J{hr+1}:J{r-1}', CellIsRule(operator='greaterThan', formula=['0'], fill=BAD))
sq.freeze_panes = 'C' + str(hr + 1)
r = para(sq, r + 1, 'Missing rows: 320 kbps at 1 KB (frames longer than chunks - out of scope), cases without any decision point, '
                    'and the 6-minute 32 kbps file at 1 KB (the probe did not finish in 90 min). The 2 KB row of that file covers its first 400 decision points.',
         width_cols=11, fill=NOTE, height=32)

# move raw data to the end, fonts everywhere
wb.move_sheet('All runs', offset=len(wb.sheetnames))
for ws in wb.worksheets:
    ws.sheet_view.showGridLines = ws.title == 'All runs'
wb.active = 0
wb.save(OUT)
print('wrote', OUT, 'configs:', [c[0] for c in CONFIGS])
