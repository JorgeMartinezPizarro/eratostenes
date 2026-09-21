#!/bin/bash
# Sweeps N (1e10..1e13), running both eratostenes and primesieve at each
# and printing a markdown table with the ratio between them -- the exact
# table format used in README.md#benchmarks, meant to be pasted there
# directly after a run.
#
# Requires primesieve on PATH (https://github.com/kimwalisch/primesieve --
# `apt-get install primesieve` on Debian/Ubuntu/WSL) for the comparison
# column; the script aborts with a clear message if it's missing rather
# than silently only benchmarking eratostenes.
#
# Only mod 30 (this project's shipped default) is swept -- mod 6 and
# mod 210 were tried against it up to 1e13 (see the i5-11400F section of
# README#benchmarks): mod 6 avoids mod 30's old L3-cliff ratio jump but
# isn't actually faster (less wheel-filtering costs about as much as the
# cache cliff saves), and mod 210's table grows too fast to be worth it
# past 1e10. If that ever changes, sweeping other wheels means rebuilding
# between them (see git history for how earlier versions of this script
# did that) -- not done here since there's currently only one worth
# tracking.
#
# -s is deliberately *not* passed by default: the CLI's own auto default
# (sized from N and the machine's real L2/L3, see src/arg_parser.hpp) is
# what this project actually recommends running with, so that's what gets
# benchmarked. Set SEGMENT to force a specific width instead (e.g. to
# compare against the auto default).
#
# Usage: ./scripts/benchmark.sh
# Env overrides: THREADS (default: nproc, used for both tools -- an
# apples-to-apples comparison needs the same thread count), SEGMENT
# (default: unset, i.e. the CLI's own auto -s), REPS (default: 1, keeps
# the fastest of REPS runs per N -- there's real run-to-run noise on this
# kind of box, see BENCHMARK section of the README/commit history).
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v primesieve >/dev/null 2>&1; then
    echo "primesieve no esta en el PATH -- instalalo (apt-get install primesieve)" >&2
    echo "para poder generar la columna de comparacion." >&2
    exit 1
fi

THREADS="${THREADS:-$(nproc)}"
SEGMENT="${SEGMENT:-}"
REPS="${REPS:-1}"

NS=(1e10 1e11 1e12 1e13)
# pi(N) for each N above, in the same order -- known values, used to catch
# a silently-wrong build/primesieve mismatch instead of just reporting a
# (meaningless) time. See scripts/test.sh for the same values at other N.
EXPECTED=(455052511 4118054813 37607912018 346065536839)

echo "Reconstruyendo eratostenes..." >&2
make re >/tmp/benchmark_build.log 2>&1 || { cat /tmp/benchmark_build.log >&2; exit 1; }

declare -A ERATO_TIME
declare -A PRIMESIEVE_TIME

for i in "${!NS[@]}"; do
    n="${NS[$i]}"
    expected="${EXPECTED[$i]}"

    best_e=""
    best_p=""
    for ((r = 1; r <= REPS; r++)); do
        # --- eratostenes ---
        seg_args=()
        [ -n "$SEGMENT" ] && seg_args=(-s "$SEGMENT")
        out=$(./eratostenes "$n" -t "$THREADS" "${seg_args[@]}" --count-only 2>&1)
        t_e=$(echo "$out" | sed -nE 's/.*primos\), .*total: *([0-9.]+)s.*/\1/p')
        # sed above only matches the "Iniciando..." + "total:" combined
        # blob in edge cases; fall back to a plain total: match.
        [ -z "$t_e" ] && t_e=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
        count_e=$(echo "$out" | sed -nE 's/.*Listo\. ([0-9,]+) primos.*/\1/p' | tr -d ',')
        if [ "$count_e" != "$expected" ]; then
            echo "n=$n rep=$r: eratostenes MAL: obtenido $count_e, esperado $expected" >&2
            exit 1
        fi
        if [ -z "$best_e" ] || awk -v a="$t_e" -v b="$best_e" 'BEGIN{exit !(a<b)}'; then
            best_e="$t_e"
        fi

        # --- primesieve ---
        out=$(primesieve "$n" --count -t "$THREADS" --time -q 2>&1)
        t_p=$(echo "$out" | sed -nE 's/^Seconds: *([0-9.]+)$/\1/p')
        count_p=$(echo "$out" | grep -oE '^[0-9]+$' | head -1)
        if [ "$count_p" != "$expected" ]; then
            echo "n=$n rep=$r: primesieve MAL: obtenido $count_p, esperado $expected" >&2
            exit 1
        fi
        if [ -z "$best_p" ] || awk -v a="$t_p" -v b="$best_p" 'BEGIN{exit !(a<b)}'; then
            best_p="$t_p"
        fi

        echo "  n=$n rep=$r eratostenes=${t_e}s primesieve=${t_p}s [ok]" >&2
    done
    ERATO_TIME["$n"]="$best_e"
    PRIMESIEVE_TIME["$n"]="$best_p"
done

echo >&2
echo "| N | eratostenes | primesieve | ratio |"
printf "|---|---:|---:|---:|\n"
for n in "${NS[@]}"; do
    te="${ERATO_TIME[$n]}"
    tp="${PRIMESIEVE_TIME[$n]}"
    ratio=$(awk -v a="$te" -v b="$tp" 'BEGIN{printf "%.1f", a/b}')
    printf "| %s | %ss | %ss | %sx |\n" "$n" "$te" "$tp" "$ratio"
done
