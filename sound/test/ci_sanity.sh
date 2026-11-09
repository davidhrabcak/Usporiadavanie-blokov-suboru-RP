#!/usr/bin/env bash
#
# End-to-end check, run in CI on every push. `test/ci_sanity.mp3` is a synthetic VBR MP3
# (test/make_ci_sanity.sh, ffmpeg lavfi only) that `rp` must reconstruct exactly from a shuffled
# copy. A failure means the pipeline regressed (build break, crash, hang, wrong order), not
# heuristic drift.
#
# Usage: test/ci_sanity.sh [BUILD_DIR] [FIXTURE]   (run from sound/; default build dir cmake-build-debug)

set -euo pipefail
BUILD_DIR=${1:-cmake-build-debug}
FIXTURE=${2:-test/ci_sanity.mp3}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo ">> graph_check: buildAdjacency agrees with canFollow on every pair (1 and 4 KB chunks)"
for cs in 1024 4096; do
    RP_CHUNK_SIZE=$cs "$BUILD_DIR/graph_check" "$FIXTURE" > "$WORK/graph.out" 2>&1 \
        || { echo "FAIL: graph_check"; cat "$WORK/graph.out"; exit 1; }
done

echo ">> rp (RP_SEED=1, 120 s timeout)"
RP_SEED=1 RP_REPORT="$WORK/report.json" timeout 120 "$BUILD_DIR/rp" "$FIXTURE" > "$WORK/rp.out" 2>&1 \
    || { echo "FAIL: rp exited non-zero or timed out"; tail -n 40 "$WORK/rp.out"; exit 1; }
python3 test/corpus.py score "$WORK/report.json" --expect-exact \
    || { tail -n 40 "$WORK/rp.out"; exit 1; }
echo "PASS"
