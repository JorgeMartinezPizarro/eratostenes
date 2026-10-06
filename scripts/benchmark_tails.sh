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
#   REPS     runs per program and N, as interleaved pairs alternating which
#            one goes first (era/ps, ps/era, ...); the table shows each
#            one's mean (default: 1). See benchmark.sh for why pairs and
#            means.
#   NS       space-separated N list, integers or 1eX, up to 2^64 - 1 (~1.8e19)
#            (default: "1e13 1e14 1e15 1e16 1e17 1e18"). Memory grows with
#            pi(sqrt N) in both programs, per thread: ~1.2 GB each at 1e19
#            (152M base primes), ~0.4 GB at 1e18.
#   WIDTH    window width, integer or 1eX (default: 1e11)
#   SEGMENT  forced -s for eratostenes (default: unset, the CLI's auto width)
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
THREADS="${THREADS:-$(nproc)}"
REPS="${REPS:-1}"
NS="${NS:-1e13 1e14 1e15 1e16 1e17 1e18}"
WIDTH="${WIDTH:-1e11}"
SEGMENT="${SEGMENT:-}"   # forced -s for eratostenes (default: the CLI's own auto width)

if ! command -v primesieve >/dev/null 2>&1; then
    echo "primesieve no esta en el PATH -- instalalo (apt-get install primesieve)." >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "No existe $BIN -- compila antes (make)." >&2
    exit 1
fi

# Numbers stay decimal strings: N goes up to 2^64 - 1, and bash arithmetic
# stops at 2^63 - 1 (1e19 overflowed it).
source scripts/lib.sh # to_dec, num_lt, mean_of
dec_sub() { # a - b, in 9-digit limbs; fails when b > a
    local a=$1 b=$2 len i x y borrow=0 out="" limb
    len=$(( ${#a} > ${#b} ? ${#a} : ${#b} ))
    len=$(( (len + 8) / 9 * 9 ))
    a=$(printf "%${len}s" "$a" | tr ' ' 0)
    b=$(printf "%${len}s" "$b" | tr ' ' 0)
    for (( i = len - 9; i >= 0; i -= 9 )); do
        x=$(( 10#${a:i:9} )); y=$(( 10#${b:i:9} + borrow ))
        if (( x < y )); then x=$(( x + 1000000000 )); borrow=1; else borrow=0; fi
        printf -v limb '%09d' $(( x - y ))
        out=$limb$out
    done
    (( borrow == 0 )) || return 1
    out=$(echo "$out" | sed 's/^0*//')
    echo "${out:-0}"
}
dec_mod() { # a mod m, m small
    local a=$1 m=$2 r=0 i
    for (( i = 0; i < ${#a}; i++ )); do r=$(( (r * 10 + ${a:i:1}) % m )); done
    echo "$r"
}

width=$(to_dec "$WIDTH")

run_era() { # stop start -> sets t_e, c_e
    local out
    seg_args=()
    [ -n "$SEGMENT" ] && seg_args=(-s "$SEGMENT")
    out=$("$BIN" "$1" --start "$2" -t "$THREADS" "${seg_args[@]}" 2>&1) || { echo "$out" >&2; exit 1; }
    t_e=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
    c_e=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
}
run_ps() { # stop start -> sets t_p, c_p
    local out
    out=$(primesieve "$2" "$1" --count -t "$THREADS" --time -q 2>&1) || { echo "$out" >&2; exit 1; }
    t_p=$(echo "$out" | sed -nE 's/^Seconds: *([0-9.]+)$/\1/p')
    c_p=$(echo "$out" | grep -oE '^[0-9]+$' | head -1 || true)
}
declare -A MEAN_E MEAN_P COUNT
for n in $NS; do
    stop=$(to_dec "$n")
    # Both programs keep every base prime in each thread's bucket ring (8
    # bytes each), eratostenes its own list too: skip an N that doesn't fit in
    # memory instead of swapping (or the OOM killer).
    need_kb=$(awk -v n="$stop" -v t="$THREADS" 'BEGIN{r = sqrt(n); printf "%d", (t + 1) * 8 * 1.15 * r / log(r) / 1024}')
    avail_kb=$(sed -nE 's/^MemAvailable: *([0-9]+) kB$/\1/p' /proc/meminfo 2>/dev/null || true)
    if [ -n "$avail_kb" ] && [ "$need_kb" -gt "$avail_kb" ]; then
        echo "  N=$n: saltado, necesita ~$(awk -v k="$need_kb" 'BEGIN{printf "%.1f", k / 1048576}') GiB con $THREADS hilos y MemAvailable da $(awk -v k="$avail_kb" 'BEGIN{printf "%.1f", k / 1048576}') GiB (prueba con menos THREADS)" >&2
        continue
    fi
    if ! diff=$(dec_sub "$stop" "$width") || [ "$diff" = 0 ]; then
        echo "WIDTH=$WIDTH no cabe por debajo de N=$n" >&2
        exit 1
    fi
    # --start rounds down to a multiple of 64 wheel indices (8 * 30 = 240
    # numbers, src/main.cpp split_ranges), so primesieve gets that same start
    # or the counts differ by the few primes in between. The default windows
    # (N - 1e11) are already multiples of 240.
    start=$(dec_sub "$diff" "$(dec_mod "$diff" 240)")
    times_e=() times_p=() count=""
    for ((r = 1; r <= REPS; r++)); do
        if (( r % 2 )); then run_era "$stop" "$start"; run_ps "$stop" "$start"
        else run_ps "$stop" "$start"; run_era "$stop" "$start"; fi
        if [ -z "$c_e" ] || [ "$c_e" != "$c_p" ]; then
            echo "N=$n rep=$r: los recuentos no coinciden: eratostenes=${c_e:-?} primesieve=${c_p:-?}" >&2
            exit 1
        fi
        count="$c_e"
        times_e+=("$t_e"); times_p+=("$t_p")
        echo "  N=$n rep=$r eratostenes=${t_e}s primesieve=${t_p}s ($count primos) [ok]" >&2
    done
    MEAN_E[$n]=$(mean_of 3 "${times_e[@]}")
    MEAN_P[$n]=$(mean_of 3 "${times_p[@]}")
    COUNT[$n]="$count"
done

echo
bash scripts/machine_info.sh "$THREADS" "last $WIDTH below N, mean of $REPS, pairs interleaved"
echo
# Same layout as README.md's tail table: the window width goes in the header
# line above, the counts (checked above) stay in the progress lines.
echo "| N | eratostenes | primesieve | ratio |"
echo "|---|---:|---:|---:|"
for n in $NS; do
    [ -n "${COUNT[$n]:-}" ] || continue # skipped for memory
    ratio=$(awk -v a="${MEAN_E[$n]}" -v b="${MEAN_P[$n]}" 'BEGIN{printf "%.2f", a / b}')
    printf "| %s | %ss | %ss | %sx |\n" "$n" "${MEAN_E[$n]}" "${MEAN_P[$n]}" "$ratio"
done
