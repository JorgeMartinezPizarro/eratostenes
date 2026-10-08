#!/bin/bash
# Count-only sweep, meant to be pasted directly into docs/BENCHMARK.md:
#
#   CPU: runs both eratostenes and primesieve at N=1e10..1e13 and prints
#      a markdown table with the ratio between them.
#
#      Requires primesieve on PATH (https://github.com/kimwalisch/primesieve,
#      built from the same tag on every machine: docs/ISSUES.md) for the
#      comparison column; the script aborts if it's missing.
#
#      -s is deliberately *not* passed by default: the CLI's own automatic
#      width (src/tuning.hpp, from N and the machine's caches) is what this
#      project recommends running with, so that's what gets benchmarked. Set
#      SEGMENT to force a specific width instead.
#
# The .db I/O sweep is scripts/benchmark_io.sh (make benchmark-io).
#
# Usage: ./scripts/benchmark.sh
# Env overrides:
#   THREADS  thread count for both programs (default: nproc -- an
#            apples-to-apples comparison needs the same thread count)
#   SEGMENT  forced -s for eratostenes (default: unset, the CLI's auto width)
#   REPS     runs per program and N, as interleaved pairs alternating which
#            one goes first (era/ps, ps/era, ...); the table shows each
#            one's mean (default: 1). Note that at N=1e13 each primesieve
#            run costs minutes too.
#
# Pairs and means, not "primesieve once, then the best of REPS": a
# machine's speed drifts over a session, and interleaving the pairs and
# alternating which program goes first shares that drift evenly between
# both. Reps run back to back, with no pause between them.
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
THREADS="${THREADS:-$(nproc)}"
SEGMENT="${SEGMENT:-}"
REPS="${REPS:-1}"

if ! command -v primesieve >/dev/null 2>&1; then
    echo "primesieve is not on the PATH -- install it (docs/ISSUES.md: the same version on every machine)" >&2
    echo "for the comparison column." >&2
    exit 1
fi

echo "Rebuilding eratostenes..." >&2
make re >/tmp/benchmark_build.log 2>&1 || { cat /tmp/benchmark_build.log >&2; exit 1; }

source scripts/lib.sh # mean_of

NS=(1e10 1e11 1e12 1e13)
# pi(N) for each N above, in the same order -- known values, used to catch
# a silently-wrong build/primesieve mismatch instead of just reporting a
# (meaningless) time. See scripts/test.sh for the same values at other N.
EXPECTED=(455052511 4118054813 37607912018 346065536839)

run_era() { # n expected -> sets t_e
    local out count_e
    seg_args=()
    [ -n "$SEGMENT" ] && seg_args=(-s "$SEGMENT")
    out=$("$BIN" "$1" -t "$THREADS" "${seg_args[@]}" 2>&1) || { echo "$out" >&2; exit 1; }
    t_e=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
    count_e=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
    if [ "$count_e" != "$2" ]; then
        echo "n=$1: eratostenes WRONG: got $count_e, expected $2" >&2
        exit 1
    fi
}
run_ps() { # n expected -> sets t_p
    local out count_p
    out=$(primesieve "$1" --count -t "$THREADS" --time -q 2>&1) || { echo "$out" >&2; exit 1; }
    t_p=$(echo "$out" | sed -nE 's/^Seconds: *([0-9.]+)$/\1/p')
    count_p=$(echo "$out" | grep -oE '^[0-9]+$' | head -1 || true)
    if [ "$count_p" != "$2" ]; then
        echo "n=$1: primesieve WRONG: got $count_p, expected $2" >&2
        exit 1
    fi
}

declare -A ERATO_TIME PRIMESIEVE_TIME

for i in "${!NS[@]}"; do
    n="${NS[$i]}"
    expected="${EXPECTED[$i]}"
    times_e=() times_p=()
    for ((r = 1; r <= REPS; r++)); do
        if (( r % 2 )); then run_era "$n" "$expected"; run_ps "$n" "$expected"
        else run_ps "$n" "$expected"; run_era "$n" "$expected"; fi
        times_e+=("$t_e"); times_p+=("$t_p")
        echo "  n=$n rep=$r eratostenes=${t_e}s primesieve=${t_p}s [ok]" >&2
    done
    ERATO_TIME["$n"]=$(mean_of 3 "${times_e[@]}")
    PRIMESIEVE_TIME["$n"]=$(mean_of 3 "${times_p[@]}")
done

echo >&2
bash scripts/machine_info.sh "$THREADS" "mean of $REPS, pairs interleaved"
echo
echo "| N | eratostenes | primesieve | ratio |"
printf "|---|---:|---:|---:|\n"
for n in "${NS[@]}"; do
    te="${ERATO_TIME[$n]}"
    tp="${PRIMESIEVE_TIME[$n]}"
    # %.1f rounds anything in [0.95, 1.05) up to "1.0x", which hides a real
    # few-percent win under primesieve (e.g. 0.97 -> "1.0x" reads as a tie).
    # Two decimals keeps that visible as "0.97x" instead.
    ratio=$(awk -v a="$te" -v b="$tp" 'BEGIN{printf "%.2f", a/b}')
    printf "| %s | %ss | %ss | %sx |\n" "$n" "$te" "$tp" "$ratio"
done
