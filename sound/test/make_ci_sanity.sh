#!/usr/bin/env bash
#
# Regenerates test/ci_sanity.mp3 - the synthetic fixture used by test/ci_sanity.sh.
#
# The signal is pure ffmpeg lavfi synthesis (no third-party audio): three frequency-varying
# tone components (an FM-swept 220 Hz partial, a linear chirp, an amplitude-gated 1.1 kHz
# partial) plus a 70 Hz sub, all under a multi-LFO amplitude envelope. The point is lots of
# frame-to-frame spectral AND loudness variation, so VBR frame sizes span ~100-730 bytes and
# global_gain moves every frame - which makes the shuffled-chunk reconstruction unambiguous.
#
# The committed .mp3 is what CI actually tests against (hermetic, no ffmpeg needed on the
# runner, encoder-version independent). Only rerun this if you deliberately want a new fixture,
# and then re-check the thresholds in test/ci_sanity.sh.
#
# Requires: ffmpeg with libmp3lame.

set -euo pipefail
cd "$(dirname "$0")"

ffmpeg -hide_banner -loglevel error -y -f lavfi \
 -i "aevalsrc='(0.30*sin(2*PI*(220+90*sin(2*PI*0.5*t))*t) + 0.22*sin(2*PI*(330+120*t)*t) + 0.16*sin(2*PI*1100*t)*sin(2*PI*3*t) + 0.10*sin(2*PI*70*t)) * (0.35 + 0.30*sin(2*PI*0.8*t) + 0.20*sin(2*PI*0.17*t) + 0.15*sin(2*PI*2.3*t)):d=8:s=44100'" \
 -ac 2 -c:a libmp3lame -q:a 4 -write_xing 0 ci_sanity.mp3

echo "wrote $(pwd)/ci_sanity.mp3 ($(stat -c%s ci_sanity.mp3) bytes)"
