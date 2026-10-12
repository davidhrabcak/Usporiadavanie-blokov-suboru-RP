#!/usr/bin/env bash
# One benchmark run (BENCHMARK_pcm_vs_main.md). usage: bench_one.sh CONFIG FILE CHUNKSIZE SEED
# env: SP = work dir with bin/ (frozen rp binaries: rp_main, rp_mainfix, rp_pcm, rp_pcm_final3)
#      and bench/logs/ (one log per run, parsed by parse.py). Run many in parallel, e.g.
#   printf '%s\n' final FILE 4096 1 ... | xargs -d '\n' -n4 -P4 test/bench/bench_one.sh
# Configs main/mainfix/pcm and the pcm_* ablations (pass 1) used rp_main/rp_mainfix/rp_pcm with
# no extra env; see the Notes sheet of the workbook for what each binary is.
cfg=$1; f=$2; cs=$3; seed=$4
case $cfg in
  main)           bin=rp_main;       envs=() ;;
  mainfix)        bin=rp_mainfix;    envs=() ;;
  pcm)            bin=rp_pcm;        envs=() ;;
  pcm_nopcm)      bin=rp_pcm;        envs=(RP_PCM=0) ;;
  pcm_nolock)     bin=rp_pcm;        envs=(RP_LOCK=0) ;;
  pcm_norepair)   bin=rp_pcm;        envs=(RP_REPAIR=0) ;;
  pcm_tau15)      bin=rp_pcm;        envs=(RP_LOCK_TAU=1.5) ;;
  final)          bin=rp_pcm_final3; envs=(RP_THREADS=4) ;;
  final_nopcm)    bin=rp_pcm_final3; envs=(RP_THREADS=4 RP_PCM=0) ;;
  final_nolock)   bin=rp_pcm_final3; envs=(RP_THREADS=4 RP_LOCK=0) ;;
  final_norepair) bin=rp_pcm_final3; envs=(RP_THREADS=4 RP_REPAIR=0) ;;
  final_tau15)    bin=rp_pcm_final3; envs=(RP_THREADS=4 RP_LOCK_TAU=1.5) ;;
  final_repair1)  bin=rp_pcm_final3; envs=(RP_THREADS=4 RP_REPAIR=1) ;;
  mainfix150)     bin=rp_mainfix;    envs=(RP_TIME_BUDGET=150) ;;
  ho_mainfix)     bin=rp_mainfix;    envs=() ;;
  ho_final)       bin=rp_pcm_final3; envs=(RP_THREADS=4) ;;
  *) echo "unknown config $cfg"; exit 1 ;;
esac
b=$(basename "$f" .mp3 | tr ' –' '__')
log="$SP/bench/logs/${cfg}__${b}__${cs}__s${seed}.log"
wd=$(mktemp -d "$SP/bench/wd.XXXXXX"); cd "$wd"
s=$(date +%s.%N)
env RP_TIME_BUDGET=30 "${envs[@]}" RP_CHUNK_SIZE=$cs RP_SEED=$seed timeout 900 "$SP/bin/$bin" "$f" 2>&1 \
  | grep -v '^  *[0-9]\+  ' > "$log"; rc=${PIPESTATUS[0]}
echo "BENCH rc=$rc secs=$(echo "$(date +%s.%N) - $s" | bc) cfg=$cfg file=$b cs=$cs seed=$seed" >> "$log"
cd /; rm -rf "$wd"
