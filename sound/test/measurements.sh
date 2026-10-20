#!/usr/bin/env bash
#
# C3 - regenerate the quantitative claims made in CLAUDE.md, in one command, so they can be
# re-verified instead of going stale (the doc has a documented history of undated claims that
# stopped being true - "~0.17s", "test7.mp3 0 breaks").
#
# Informational only (no pass/fail - that's test/regression.sh). Prints, per available
# fixture: pre-shuffle canFollow breaks, reservoir-check-on-true-order breaks, total chunks,
# supernode count, the "exactly one valid successor" ratio, and (from sideinfo_probe) the
# global_gain junction-delta AUC + its Measurement-B rank-1 rate.
#
# Fixtures are third-party and not in git (see PLAN_sideinfo_mainbits.md "Test fixtures");
# missing ones are skipped with a note.
#
# Usage: test/measurements.sh [BUILD_DIR]   (run from sound/)

set -u
BUILD_DIR=${1:-cmake-build-debug}
RP="$BUILD_DIR/rp"
PROBE="$BUILD_DIR/sideinfo_probe"
FIXTURES=(sample-3s CBR test7_shorter test7_short test7 ci_sanity)

for b in "$RP" "$PROBE"; do
    [ -x "$b" ] || { echo "missing $b - build first (cmake --build $BUILD_DIR)"; exit 1; }
done

hr() { printf '%.0s-' {1..92}; echo; }
printf "%-14s %7s %7s %8s %9s %11s %9s %9s\n" \
       fixture chunks supers preBrk resvBrk 1-succ-% gg-AUC gg-rank1
hr
for f in "${FIXTURES[@]}"; do
    mp3="test/$f.mp3"
    [ -f "$mp3" ] || { printf "%-14s  (not present)\n" "$f"; continue; }

    # rp: fixed seed, no search (cap nodes hard so we only pay for the diagnostics we parse).
    out=$(RP_SEED=1 RP_MAX_NODES=1 timeout 120 "$RP" "$mp3" 2>&1)
    chunks=$(sed -n 's/^Total chunks: \([0-9]*\).*/\1/p'      <<<"$out")
    supers=$(sed -n 's/^Supernodes after chain collapse: \([0-9]*\).*/\1/p' <<<"$out")
    preBrk=$(sed -n 's/^Pre-shuffle check \(PASSED.*all [0-9]* consecutive pairs compatible\|FAILED: \([0-9]*\).*\)/\2/p' <<<"$out")
    [ -z "$preBrk" ] && grep -q "Pre-shuffle check PASSED" <<<"$out" && preBrk=0
    resvBrk=$(grep -c "RESERVOIR BREAK" <<<"$out")
    grep -q "Reservoir check PASSED on true order" <<<"$out" && resvBrk=0
    onesucc=$(sed -n 's#^Adjacency graph: \([0-9]*\)/\([0-9]*\) chunks have exactly one valid successor#\1 \2#p' <<<"$out")
    if [ -n "$onesucc" ]; then
        set -- $onesucc; onepct=$(awk -v a="$1" -v b="$2" 'BEGIN{printf "%.0f", 100*a/b}')
    else onepct="?"; fi

    pout=$("$PROBE" "$mp3" 2>&1)
    ggAUC=$(awk '/^globalGain /&&NF==4{print $NF; exit}' <<<"$pout")   # Measurement A row: "globalGain  med med AUC"
    ggR1=$(grep -oE '^globalGain [^[]*rank1= *[0-9.]+%' <<<"$pout" | grep -oE '[0-9.]+%$')  # overall rank1, not the deg>=3 one

    printf "%-14s %7s %7s %8s %9s %10s%% %9s %9s\n" \
           "$f" "${chunks:-?}" "${supers:-?}" "${preBrk:-?}" "${resvBrk:-0}" "${onepct}" "${ggAUC:-?}" "${ggR1:-?}"
done
hr
echo "pre/resv breaks should be 0 on every fixture (true order is always valid);"
echo "gg-AUC ~0.68-0.77 and gg-rank1 ~60-74% on the VBR fixtures per CLAUDE.md item 7."
