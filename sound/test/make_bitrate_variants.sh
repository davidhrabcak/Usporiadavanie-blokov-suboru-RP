#!/usr/bin/env bash
#
# Generates short constant- and variable-bitrate test fixtures from one source track, for the
# bitrate / chunk-size matrix in BENCHMARK_pcm_vs_main.md.
#
#   test/gen/cbr<K>_<D>s.mp3    CBR at K kbps (32 64 128 192 320), D seconds long (10, 30)
#   test/gen/vbrq<Q>_<D>s.mp3   LAME VBR -q:a Q (2, 5), same lengths
#
# Every file is forced to 44.1 kHz stereo MPEG-1 (LAME would otherwise resample low bitrates to
# MPEG-2 22.05 kHz, which changes side-info size and the reservoir limit), carries no tags and no
# Xing/Info frame, and is cut from the same position of the source, so the only variables are
# bitrate mode and length.
#
# The source is third-party audio and is NOT committed (neither are the outputs - *.mp3 is
# gitignored). Pass any long music track:
#   test/make_bitrate_variants.sh "test/Queen – Bohemian Rhapsody VBR.mp3" [START_SECONDS]
#
# Requires: ffmpeg with libmp3lame.

set -euo pipefail
SRC=${1:?usage: $0 SOURCE.mp3 [START_SECONDS]}
START=${2:-120}
cd "$(dirname "$0")"
[ -f "$SRC" ] || SRC="../$SRC"
mkdir -p gen

enc() { # out duration codec-args...
    local out=$1 dur=$2; shift 2
    ffmpeg -hide_banner -loglevel error -y -ss "$START" -t "$dur" -i "$SRC" \
        -map 0:a:0 -map_metadata -1 -ar 44100 -ac 2 -c:a libmp3lame "$@" \
        -write_xing 0 -id3v2_version 0 -write_id3v1 0 "gen/$out"
    printf '%-18s %8d bytes  frame sizes: %s\n' "$out" "$(stat -c%s "gen/$out")" \
        "$(ffprobe -v error -select_streams a -show_entries packet=size -of csv=p=0 "gen/$out" \
           | sort -n | uniq -c | sort -rn | head -3 | awk '{printf "%sx%s ", $1, $2}')"
}

for d in 10 30; do
    for k in 32 64 128 192 320; do enc "cbr${k}_${d}s.mp3" "$d" -b:a "${k}k"; done
    for q in 2 5;               do enc "vbrq${q}_${d}s.mp3" "$d" -q:a "$q"; done
done
