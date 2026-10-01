#!/bin/bash
# Top-of-range comparison against primesieve: the last WIDTH numbers below
# each N (default: the last 1e11 below 1e15, 1e16 and 1e17, i.e. 0.01%,
# 0.001% and 0.0001%), counted by both programs with the same thread count.
# Every window has the same width, so each run costs about the same while N
# grows: what changes is the height -- how many base primes are active and
# which tiers they fall in (the sparse tier dominates from ~1e14 up) -- which
# is exactly what a full run at that N can't show in reasonable time.
#
# eratostenes runs as `eratostenes N --start N-WIDTH` (count-only; see
# --start in src/arg_parser.hpp), primesieve as `primesieve N-WIDTH N -c`
# (N-WIDTH rounded down to a multiple of 240, see below).
# Both counts must match each other, or the script stops: there is no known
# pi() table for these windows, so primesieve is the cross-check.
#
# The machine's identity is printed above the table (scripts/machine_info.sh),
# ready to paste into docs/BENCHMARK.md.
#
# Usage: bash scripts/benchmark_tails.sh   (or: make benchmark-tails)
# Env overrides:
#   THREADS  thread count for both programs (default: nproc)
#   REPS     runs per program and N, alternating which one goes first
#            (era/ps, ps/era, ...); the table keeps each one's fastest
#            (default: 1)
#   NS       space-separated N list, integers or 1eX (default: "1e15 1e16 1e17")
#   WIDTH    window width, integer or 1eX (default: 1e11)
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
THREADS="${THREADS:-$(nproc)}"
REPS="${REPS:-1}"
NS="${NS:-1e14 1e15 1e16 1e17 1e18}"
WIDTH="${WIDTH:-1e11}"

if ! command -v primesieve >/dev/null 2>&1; then
    echo "primesieve no esta en el PATH -- instalalo (apt-get install primesieve)." >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "No existe $BIN -- compila antes (make)." >&2
    exit 1
fi

# "1e15" / "100000" -> exact 64-bit integer (bash arithmetic, no doubles).
to_int() {
    if [[ "$1" =~ ^([0-9]+)[eE]([0-9]+)$ ]]; then
        echo $(( BASH_REMATCH[1] * 10 ** BASH_REMATCH[2] ))
    elif [[ "$1" =~ ^[0-9]+$ ]]; then
        echo "$1"
    else
        echo "valor no valido: $1 (usa un entero o 1eX)" >&2
        exit 1
    fi
}

width=$(to_int "$WIDTH")

run_era() { # stop start -> sets t_e, c_e
    local out
    out=$("$BIN" "$1" --start "$2" -t "$THREADS" 2>&1) || { echo "$out" >&2; exit 1; }
    t_e=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
    c_e=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
}
run_ps() { # stop start -> sets t_p, c_p
    local out
    out=$(primesieve "$2" "$1" --count -t "$THREADS" --time -q 2>&1) || { echo "$out" >&2; exit 1; }
    t_p=$(echo "$out" | sed -nE 's/^Seconds: *([0-9.]+)$/\1/p')
    c_p=$(echo "$out" | grep -oE '^[0-9]+$' | head -1 || true)
}
faster() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(b == "" || a < b)}'; }

declare -A BEST_E BEST_P COUNT
for n in $NS; do
    stop=$(to_int "$n")
    if [ "$width" -ge "$stop" ]; then
        echo "WIDTH=$WIDTH no cabe por debajo de N=$n" >&2
        exit 1
    fi
    # --start rounds down to a multiple of 64 wheel indices (8 * 30 = 240
    # numbers, src/main.cpp split_ranges), so primesieve gets that same start
    # or the counts differ by the few primes in between. The default windows
    # (N - 1e11) are already multiples of 240.
    start=$(( (stop - width) / 240 * 240 ))
    best_e="" best_p="" count=""
    for ((r = 1; r <= REPS; r++)); do
        if (( r % 2 )); then run_era "$stop" "$start"; run_ps "$stop" "$start"
        else run_ps "$stop" "$start"; run_era "$stop" "$start"; fi
        if [ -z "$c_e" ] || [ "$c_e" != "$c_p" ]; then
            echo "N=$n rep=$r: los recuentos no coinciden: eratostenes=${c_e:-?} primesieve=${c_p:-?}" >&2
            exit 1
        fi
        count="$c_e"
        faster "$t_e" "$best_e" && best_e="$t_e"
        faster "$t_p" "$best_p" && best_p="$t_p"
        echo "  N=$n rep=$r eratostenes=${t_e}s primesieve=${t_p}s ($count primos) [ok]" >&2
    done
    BEST_E[$n]="$best_e"
    BEST_P[$n]="$best_p"
    COUNT[$n]="$count"
done

echo
bash scripts/machine_info.sh "$THREADS" "best of $REPS"
echo
echo "| N | tail | primes | eratostenes | primesieve | ratio |"
echo "|---|---|---:|---:|---:|---:|"
for n in $NS; do
    stop=$(to_int "$n")
    pct=$(awk -v w="$width" -v s="$stop" 'BEGIN{printf "%g%%", 100 * w / s}')
    ratio=$(awk -v a="${BEST_E[$n]}" -v b="${BEST_P[$n]}" 'BEGIN{printf "%.2f", a / b}')
    primes=$(printf '%d' "${COUNT[$n]}" | sed -E ':a; s/([0-9])([0-9]{3})(,|$)/\1,\2\3/; ta')
    printf "| %s | last %s (%s) | %s | %ss | %ss | %sx |\n" \
        "$n" "$WIDTH" "$pct" "$primes" "${BEST_E[$n]}" "${BEST_P[$n]}" "$ratio"
done
