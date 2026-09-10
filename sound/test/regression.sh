#!/usr/bin/env bash
#
# C4 - local accuracy regression check. CI (test/ci_sanity.sh) only proves the pipeline runs;
# this proves it still reconstructs the real fixtures as well as it used to. Runs each fixture
# through `rp` at a fixed seed and compares four metrics against a committed baseline:
#
#   preBrk   pre-shuffle canFollow breaks on the true order      (must stay <= baseline; expect 0)
#   resvBrk  reservoir-check breaks on the true order            (must stay <= baseline)
#   supers   supernode count after chain collapse                (warn if it changes)
#   exact%   chunks placed in their original position            (must stay >= baseline - TOL)
#
# Fixtures are third-party and not in git (see PLAN_sideinfo_mainbits.md "Test fixtures").
# Missing ones are skipped. NOT run in CI - it needs the fixtures and takes a couple of minutes.
#
# Usage:
#   test/regression.sh [BUILD_DIR]            compare against test/regression_baseline.tsv
#   test/regression.sh --update [BUILD_DIR]   (re)write the baseline from the current build
# Run from sound/.

set -u
TOL=3                         # exact% may drop this many points before it's a regression
BUDGET=${RP_TIME_BUDGET:-20}  # per-fixture wall-clock budget for the search
BASELINE=test/regression_baseline.tsv
FIXTURES=(sample-3s CBR test7_shorter test7_short test7 ci_sanity)

UPDATE=0
[ "${1:-}" = "--update" ] && { UPDATE=1; shift; }
BUILD_DIR=${1:-cmake-build-debug}
RP="$BUILD_DIR/rp"
[ -x "$RP" ] || { echo "missing $RP - build first"; exit 1; }

measure() { # fixture -> "preBrk resvBrk supers exact"
    local out
    out=$(RP_SEED=1 RP_TIME_BUDGET="$BUDGET" timeout 300 "$RP" "test/$1.mp3" 2>&1)
    local preBrk resvBrk supers exact
    if grep -q "Pre-shuffle check PASSED" <<<"$out"; then preBrk=0
    else preBrk=$(sed -n 's/^Pre-shuffle check FAILED: \([0-9]*\).*/\1/p' <<<"$out"); fi
    if grep -q "Reservoir check PASSED on true order" <<<"$out"; then resvBrk=0
    else resvBrk=$(grep -c "RESERVOIR BREAK" <<<"$out"); fi
    supers=$(sed -n 's/^Supernodes after chain collapse: \([0-9]*\).*/\1/p' <<<"$out")
    exact=$(sed -n 's#^Reconstruction vs source: [0-9]*/[0-9]* chunks in original position (\([0-9.]*\)%).*#\1#p' <<<"$out")
    [ -z "$exact" ] && exact=FAILED
    echo "${preBrk:-?} ${resvBrk:-?} ${supers:-?} ${exact}"
}

if [ "$UPDATE" = 1 ]; then
    : > "$BASELINE"
    for f in "${FIXTURES[@]}"; do
        [ -f "test/$f.mp3" ] || { echo "skip $f (not present)"; continue; }
        read -r pb rb su ex <<<"$(measure "$f")"
        printf '%s\t%s\t%s\t%s\t%s\n' "$f" "$pb" "$rb" "$su" "$ex" | tee -a "$BASELINE"
    done
    echo "wrote $BASELINE"
    exit 0
fi

[ -f "$BASELINE" ] || { echo "no $BASELINE - run: test/regression.sh --update $BUILD_DIR"; exit 1; }

fail=0
printf "%-14s %-26s %-26s %s\n" fixture baseline current status
while IFS=$'\t' read -r f b_pb b_rb b_su b_ex; do
    [ -f "test/$f.mp3" ] || { printf "%-14s %-26s %-26s %s\n" "$f" "-" "-" "SKIP (fixture absent)"; continue; }
    read -r pb rb su ex <<<"$(measure "$f")"
    status=OK
    [ "$pb" != "?" ] && [ "$pb" -gt "$b_pb" ] 2>/dev/null && { status="REGRESSED preBrk $b_pb->$pb"; fail=1; }
    [ "$rb" != "?" ] && [ "$rb" -gt "$b_rb" ] 2>/dev/null && { status="REGRESSED resvBrk $b_rb->$rb"; fail=1; }
    [ "$su" != "$b_su" ] && [ "$status" = OK ] && status="note: supers $b_su->$su"
    if [ "$b_ex" != FAILED ] && [ "$ex" = FAILED ]; then
        status="REGRESSED recon $b_ex%->FAILED"; fail=1
    elif [ "$b_ex" != FAILED ] && [ "$ex" != FAILED ]; then
        awk -v c="$ex" -v b="$b_ex" -v t="$TOL" 'BEGIN{exit !(c < b - t)}' \
            && { status="REGRESSED exact ${b_ex}%->${ex}%"; fail=1; }
    fi
    printf "%-14s %-26s %-26s %s\n" "$f" \
        "pb=$b_pb rb=$b_rb ex=$b_ex" "pb=$pb rb=$rb ex=$ex" "$status"
done < "$BASELINE"

[ "$fail" = 0 ] && echo "PASS: no accuracy regression" || echo "FAIL: see REGRESSED rows above"
exit $fail
