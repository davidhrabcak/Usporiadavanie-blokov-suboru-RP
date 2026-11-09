#!/usr/bin/env bash
#
# Split long MP3s in a corpus directory (default: musiclib/) into track-sized pieces.
#
#   test/split_long_tracks.sh [-n] [-m MIN_MINUTES] [DIR]
#     -n   dry run: print the plan, change nothing
#     -m   only split files longer than this many minutes (default 12)
#
# Piece length shrinks as the file gets longer: L = clamp(6 - D/10, 2, 5) minutes for a D-minute
# file, then n = round(D / L) equal pieces (20 min -> 5 x 4 min, 30 min -> 10 x 3 min,
# 60 min -> 30 x 2 min, 15 min -> 3 x 5 min).
#
# Why ffmpeg and not split(1): a byte split cuts through frames, and every piece after the first
# then starts mid-stream, where main_data_begin points into the previous piece's bytes. rp finds
# the start chunk by main_data_begin == 0 and the reservoir check rejects such a start, so those
# pieces would be useless as test inputs (CLAUDE.md, item 9, "Clips must start at the track's
# start"). A stream copy (-c copy, or ffmpeg's segment muxer) has the same problem. So:
#   - piece 1 is a stream copy of the first L minutes (the original encoder's bytes, valid start);
#   - pieces 2..n are each re-encoded by their own ffmpeg call (libmp3lame, same sample rate and
#     channel count; CBR at the source bitrate, VBR at -q:a 2), so each starts with an empty
#     bit reservoir. They are LAME output, not the original encoder's.
# The pieces are written next to the original as "NAME (part 01 of 10).mp3"; the original is
# moved to DIR/.long_originals/ (dot directories are skipped by test/corpus.py), not deleted.

set -euo pipefail
DRY=0; MIN=12
while getopts "nm:" o; do
    case $o in n) DRY=1 ;; m) MIN=$OPTARG ;; *) sed -n '3,7p' "$0"; exit 2 ;; esac
done
shift $((OPTIND - 1))
DIR=${1:-musiclib}
DIR=${DIR%/}
BACKUP="$DIR/.long_originals"

probe() { ffprobe -v error -select_streams a:0 -show_entries "$1" -of default=nw=1:nk=1 "$2" | head -1; }

find "$DIR" -mindepth 1 -name ".*" -prune -o -type f -iname '*.mp3' -print0 | sort -z |
while IFS= read -r -d '' f; do
    case $f in *"(part "[0-9]*" of "[0-9]*").mp3") continue ;; esac   # already a piece
    dur=$(probe format=duration "$f") || continue
    [ -n "$dur" ] && [ "$dur" != N/A ] || continue
    read -r n len < <(awk -v d="$dur" -v min="$MIN" 'BEGIN {
        m = d / 60; if (m <= min) { print 0, 0; exit }
        L = 6 - m / 10; if (L > 5) L = 5; if (L < 2) L = 2
        n = int(m / L + 0.5); if (n < 2) n = 2
        printf "%d %.3f\n", n, d / n }')
    [ "$n" -gt 0 ] || continue

    sr=$(probe stream=sample_rate "$f"); ch=$(probe stream=channels "$f")
    kbps=$(( ($(probe stream=bit_rate "$f") + 500) / 1000 ))
    # CBR packets come in at most two sizes (padding bit, plus the Xing/Info frame); VBR in many.
    sizes=$(ffprobe -v error -select_streams a:0 -read_intervals %+20 -show_entries packet=size \
            -of csv=p=0 "$f" | sort -u | wc -l)
    if [ "$sizes" -le 3 ]; then enc=(-b:a "${kbps}k"); mode="CBR ${kbps} kbps"
    else enc=(-q:a 2); mode="VBR (-q:a 2)"; fi

    printf '%s\n  %.1f min, %s, %s Hz, %s ch -> %d x %.2f min\n' \
        "$f" "$(awk -v d="$dur" 'BEGIN{print d/60}')" "$mode" "$sr" "$ch" "$n" "$(awk -v l="$len" 'BEGIN{print l/60}')"
    [ "$DRY" = 1 ] && continue

    base=${f%.*}; tmp=$(mktemp -d "$(dirname "$f")/.split.XXXXXX")
    for ((k = 0; k < n; k++)); do
        out=$(printf '%s/%s (part %02d of %02d).mp3' "$tmp" "$(basename "$base")" $((k + 1)) "$n")
        ss=$(awk -v l="$len" -v k="$k" 'BEGIN{printf "%.3f", l*k}')
        if [ "$k" = 0 ]; then codec=(-c:a copy); else codec=(-c:a libmp3lame -ar "$sr" -ac "$ch" "${enc[@]}"); fi
        ffmpeg -nostdin -v error -ss "$ss" -t "$len" -i "$f" -map 0:a:0 -map_metadata 0 \
               "${codec[@]}" -id3v2_version 3 "$out" \
            || { echo "  FAILED on part $((k + 1)); original left in place"; rm -rf "$tmp"; continue 2; }
    done
    mkdir -p "$BACKUP/$(dirname "${f#"$DIR"/}")"
    mv -n "$f" "$BACKUP/${f#"$DIR"/}"
    mv -n "$tmp"/*.mp3 "$(dirname "$f")/"
    rmdir "$tmp"
    echo "  done; original -> $BACKUP/${f#"$DIR"/}"
done
