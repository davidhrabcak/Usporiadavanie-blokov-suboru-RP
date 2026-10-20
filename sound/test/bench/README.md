# Benchmark tooling (main vs full-pcm-decode)

Produces `sound/BENCHMARK_pcm_vs_main.xlsx` / `.md`.

1. Build each variant once and copy its `rp` to `$SP/bin/` (`rp_main`: branch
   `sideinfo-mainbits`; `rp_mainfix`: same + the final-fragment fix aa9ce7c; `rp_pcm`: 1446fe6;
   `rp_pcm_final3`: f9ad99c). Fixtures: `test/make_bitrate_variants.sh SRC [120 gen | 200 gen2]`.
2. Run `bench_one.sh CONFIG FILE CHUNK_SIZE SEED` for each cell (logs go to `$SP/bench/logs/`).
3. `pcm_probe` per file and chunk size: `RP_CHUNK_SIZE=N build/pcm_probe FILE > $SP/probe/<name>__<N>.txt`.
4. `BENCH_DIR=$SP python3 parse.py` -> `runs.csv`, `probe.csv`;
   `BENCH_CONFIGS=main,mainfix,pcm,final BENCH_DIR=$SP python3 build_xlsx.py OUT.xlsx` (needs openpyxl; BENCH_CONFIGS limits the compared configurations), then recalculate in
   LibreOffice/Excel (all summary cells are formulas over the Runs sheet).
