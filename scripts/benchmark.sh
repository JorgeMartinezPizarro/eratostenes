#!/bin/bash
# Count-only sweep, meant to be pasted directly into docs/BENCHMARK.md:
#
#   CPU: runs both eratostenes and primesieve at N=1e10..1e13 and prints
#      a markdown table with the ratio between them.
#
#      Requires primesieve on PATH (https://github.com/kimwalisch/primesieve
#      -- `apt-get install primesieve` on Debian/Ubuntu/WSL) for the
#      comparison column; the script aborts with a clear message if it's
#      missing rather than silently only benchmarking eratostenes.
#
#      Only mod 30 (this project's shipped default) is swept -- mod 6 and
#      mod 210 were tried against it up to 1e13 (see the i5-11400F section
#      of README#benchmarks): mod 6 avoids mod 30's old L3-cliff ratio jump
#      but isn't actually faster (less wheel-filtering costs about as much
#      as the cache cliff saves), and mod 210's table grows too fast to be
#      worth it past 1e10. If that ever changes, sweeping other wheels
#      means rebuilding between them (see git history for how earlier
#      versions of this script did that) -- not done here since there's
#      currently only one worth tracking.
#
#      -s is deliberately *not* passed by default: the CLI's own auto
#      default (sized from N and the machine's real L2/L3, see
#      src/arg_parser.hpp) is what this project actually recommends running
#      with, so that's what gets benchmarked. Set SEGMENT to force a
#      specific width instead (e.g. to compare against the auto default).
#
# The .db I/O sweep that used to follow lives in scripts/benchmark_io.sh
# (make benchmark-io).
#
# Usage: ./scripts/benchmark.sh
# Env overrides: THREADS (default: nproc, used for both eratostenes and
# primesieve -- an apples-to-apples comparison needs the same thread
# count), SEGMENT (default: unset, i.e. the CLI's own auto -s), REPS
# (default: 1, keeps the fastest of REPS eratostenes runs per N in the CPU
# sweep -- there's real run-to-run noise on this kind of box, see BENCHMARK
# section of the README/commit history; primesieve itself always runs once
# per N regardless of REPS -- it's the fixed reference, not what's being
# tuned, and at N=1e13 a single run already costs several minutes).
# Reps run back to back, with no pause between them: a pause leaves the
# cores idle right before the next run, which then pays the ramp-up (at
# 1e10, ~0.19s from a cold start vs ~0.13s warm on the i5-13500), while
# primesieve's own reference run always starts warm. Thermal throttling
# was ruled out on both benchmark machines (clocks >= 4.2GHz, < 70C).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
THREADS="${THREADS:-$(nproc)}"
SEGMENT="${SEGMENT:-}"
REPS="${REPS:-1}"

if ! command -v primesieve >/dev/null 2>&1; then
    echo "primesieve no esta en el PATH -- instalalo (apt-get install primesieve)" >&2
    echo "para poder generar la columna de comparacion." >&2
    exit 1
fi

echo "Reconstruyendo eratostenes..." >&2
make re >/tmp/benchmark_build.log 2>&1 || { cat /tmp/benchmark_build.log >&2; exit 1; }

NS=(1e10 1e11 1e12 1e13)
# pi(N) for each N above, in the same order -- known values, used to catch
# a silently-wrong build/primesieve mismatch instead of just reporting a
# (meaningless) time. See scripts/test.sh for the same values at other N.
EXPECTED=(455052511 4118054813 37607912018 346065536839)

declare -A ERATO_TIME
declare -A PRIMESIEVE_TIME

for i in "${!NS[@]}"; do
    n="${NS[$i]}"
    expected="${EXPECTED[$i]}"

    # --- primesieve: run once regardless of REPS. It's the fixed reference,
    # not what's being tuned here -- REPS exists to smooth out eratostenes'
    # own run-to-run noise, and at N=1e13 a single primesieve run already
    # costs several minutes, so paying that REPS times over just to also
    # smooth the reference isn't worth it.
    out=$(primesieve "$n" --count -t "$THREADS" --time -q 2>&1)
    t_p=$(echo "$out" | sed -nE 's/^Seconds: *([0-9.]+)$/\1/p')
    count_p=$(echo "$out" | grep -oE '^[0-9]+$' | head -1)
    if [ "$count_p" != "$expected" ]; then
        echo "n=$n: primesieve MAL: obtenido $count_p, esperado $expected" >&2
        exit 1
    fi
    echo "  n=$n primesieve=${t_p}s [ok]" >&2

    best_e=""
    for ((r = 1; r <= REPS; r++)); do
        # --- eratostenes ---
        seg_args=()
        [ -n "$SEGMENT" ] && seg_args=(-s "$SEGMENT")
        out=$("$BIN" "$n" -t "$THREADS" "${seg_args[@]}" 2>&1)
        t_e=$(echo "$out" | sed -nE 's/.*primes\), .*total: *([0-9.]+)s.*/\1/p')
        # sed above only matches the "Starting..." + "total:" combined
        # blob in edge cases; fall back to a plain total: match.
        [ -z "$t_e" ] && t_e=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
        count_e=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
        if [ "$count_e" != "$expected" ]; then
            echo "n=$n rep=$r: eratostenes MAL: obtenido $count_e, esperado $expected" >&2
            exit 1
        fi
        if [ -z "$best_e" ] || awk -v a="$t_e" -v b="$best_e" 'BEGIN{exit !(a<b)}'; then
            best_e="$t_e"
        fi

        echo "  n=$n rep=$r eratostenes=${t_e}s [ok]" >&2
    done
    ERATO_TIME["$n"]="$best_e"
    PRIMESIEVE_TIME["$n"]="$t_p"
done

echo >&2
bash scripts/machine_info.sh "$THREADS" "eratostenes best of $REPS, primesieve 1 run"
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
