#!/bin/sh
# bench.sh -- per-phase comptime cost of the buffalo pipeline.
#
# The question is "how much compile time does running the spec-read/NFA/DFA
# pipeline plus a minimal emit step inside cccc's comptime VM actually add for
# a real ~40-rule lexer, and which phase owns it". A single wall-clock total
# cannot answer that, so this walks the BUF_STOP_AFTER ablation ladder in
# src/buf_comptime.c:
#
#   0 nothing   1 +read   2 +tokcheck   3 +NFA   4 +DFA   5 +emit
#
# For each spec x each rung it runs cccc REPS times, reports the median wall
# time and the delta from rung 0 (the no-op comptime baseline), then prints
# the arena peaks (BUF_STATS) and the generated-file size for the full run.
# Differencing adjacent rungs gives the per-phase cost.
#
#   make bench                 # both reference specs
#   REPS=9 tests/bench.sh      # more reps
#   tests/bench.sh path/to/spec.bflo ...
#
# Plain POSIX sh; needs cccc on PATH (or $CCCC) and perl for the hi-res timer.

set -eu

CCCC="${CCCC:-cccc}"
REPS="${REPS:-5}"
here=$(cd "$(dirname "$0")/.." && pwd)
SRC="$here/src/buf_comptime.c"
OUT="$here/build/bench.gen.c"

if ! command -v "$CCCC" >/dev/null 2>&1; then
    echo "skip bench (no cccc on PATH; set \$CCCC to override)"
    exit 0
fi
if ! command -v perl >/dev/null 2>&1; then
    echo "skip bench (no perl for the hi-res timer)"
    exit 0
fi

mkdir -p "$here/build"

SPECS=${*:-"$here/examples/calc.bflo $here/examples/clike.bflo"}

BENCH_T="$here/build/.bench_t"
export BENCH_T

# One timed cccc run. $1 spec, $2 rung, $3 extra -D (may be empty). Writes the
# elapsed seconds to $BENCH_T and echoes it; cccc's own output is discarded.
run_once() {
    _spec=$1; _stop=$2; _extra=${3:-}
    perl -MTime::HiRes=time -e '
        my $s  = time;
        my $rc = system(@ARGV);
        open(my $f, ">", $ENV{BENCH_T}) or die;
        printf $f "%.3f", time - $s;
        close $f;
        exit($rc == 0 ? 0 : 1);
    ' -- "$CCCC" -c=generated "$SRC" \
        -Iinclude/buffalo -Iruntime \
        -D "BUF_SPEC=\"$_spec\"" -D "BUF_STOP_AFTER=$_stop" $_extra \
        -o "$OUT" >/dev/null 2>&1
    cat "$BENCH_T"
}

# Median of REPS timed runs. $3 is an optional extra -D passed to every run.
median_of() {
    _spec=$1; _stop=$2; _mextra=${3:-}
    i=0
    _vals=""
    while [ "$i" -lt "$REPS" ]; do
        _v=$(run_once "$_spec" "$_stop" "$_mextra") || { echo "FAIL"; return 1; }
        _vals="$_vals$_v
"
        i=$((i + 1))
    done
    printf '%s' "$_vals" | sort -n | awk 'NF{a[n++]=$1} END{print a[int(n/2)]}'
}

echo "buffalo M3 bench -- $(uname -sm), cccc $($CCCC --version 2>/dev/null | head -1), $REPS reps, median wall (s)"
echo

for spec in $SPECS; do
    name=$(basename "$spec")
    echo "=== $name ==="
    base=""
    prev=""
    printf '  %-14s %8s %10s %10s\n' rung median "d(base)" "d(phase)"
    for n in 0 1 2 3 4 5; do
        label=$(awk -v n="$n" 'BEGIN{
            split("no-op read +tokcheck +NFA +DFA +emit", L, " ");
            print L[n+1]}')
        m=$(median_of "$spec" "$n") || exit 1
        if [ "$n" -eq 0 ]; then base=$m; fi
        db=$(awk -v a="$m" -v b="$base" 'BEGIN{printf "%+.3f", a-b}')
        dp=$(awk -v a="$m" -v b="$prev" 'BEGIN{if(b=="")print "-";else printf "%+.3f", a-b}')
        printf '  %-14s %8s %10s %10s\n' "$n $label" "$m" "$db" "$dp"
        if [ "$n" -eq 4 ]; then dfa_m=$m; fi
        prev=$m
    done

    # Opt-in Moore minimisation (-D BUF_MINIMIZE): its own added comptime cost,
    # measured against the +DFA rung. Off by default -- docs/performance.md.
    mm=$(median_of "$spec" 4 "-D BUF_MINIMIZE") || exit 1
    dpm=$(awk -v a="$mm" -v b="$dfa_m" 'BEGIN{printf "%+.3f", a-b}')
    printf '  %-14s %8s %10s %10s\n' "4 +DFA+min" "$mm" "" "$dpm"

    # Arena peaks + generated-file size for the full pipeline.
    echo
    "$CCCC" -c=generated "$SRC" -Iinclude/buffalo -Iruntime \
        -D "BUF_SPEC=\"$spec\"" -D BUF_STOP_AFTER=5 -D BUF_STATS \
        -o "$OUT" 2>&1 >/dev/null | sed 's/^/  /' || true
    printf '  gen.c size    %8d bytes\n\n' "$(wc -c < "$OUT")"
done
