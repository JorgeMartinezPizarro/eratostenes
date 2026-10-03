#!/bin/bash
# Interleaved A/B of two eratostenes configurations on one window (the last
# WIDTH numbers below N): A B A B ... REPS times, the sieve's own `total:`
# time of each run, then the means and B's delta against A. For checking a
# single knob on a machine where the only instrument is wall time; the
# interleaving cancels a slow drift of the host, the per-run list shows a
# fast one. A defaults to the automatic configuration; B is any set of CLI
# options (see eratostenes --help: -s, --tune key=value, --l1-bytes, ...).
# Environment variables can go in front of a configuration, e.g.
# B="ERATOSTENES_MED64_NTA=0 -s 15728640".
#
# Usage: B="-s 15728640 --tune sparse=1/2" make benchmark-ab
#        A="--tune sparse=1/1" B="--tune sparse=1/2" REPS=3 make benchmark-ab
# Env: A (default: auto), B (required), N (default 1e13), WIDTH (default
#      1e10), THREADS (default nproc), REPS (default 2)
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
THREADS="${THREADS:-$(nproc)}"
REPS="${REPS:-2}"
N_IN="${N:-1e13}"
WIDTH_IN="${WIDTH:-1e10}"
A_CFG="${A:-}"
B_CFG="${B:-}"

if [ -z "$B_CFG" ]; then
    echo "Falta B: la configuracion a comparar, p.ej. B=\"-s 15728640 --tune sparse=1/2\" make benchmark-ab" >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "No existe $BIN -- compila antes (make)." >&2
    exit 1
fi

source scripts/lib.sh # to_dec, num_lt
N=$(to_dec "$N_IN")
WIDTH=$(to_dec "$WIDTH_IN")
if (( N <= WIDTH )); then echo "WIDTH=$WIDTH_IN no cabe por debajo de N=$N_IN" >&2; exit 1; fi
START=$(( (N - WIDTH) / 240 * 240 ))

# split "VAR=x VAR2=y -s 123 --tune k=v" into leading env assignments and args
split_cfg() { # CFG -> sets envs, args (arrays)
    envs=(); args=()
    local w
    for w in $1; do
        if [ ${#args[@]} -eq 0 ] && [[ "$w" =~ ^[A-Za-z_][A-Za-z0-9_]*= ]]; then envs+=("$w"); else args+=("$w"); fi
    done
}
run_cfg() { # CFG -> t (total seconds), c (count)
    split_cfg "$1"
    local out
    out=$(env "${envs[@]}" "$BIN" "$N" --start "$START" -t "$THREADS" "${args[@]}" 2>&1) || { echo "$out" >&2; exit 1; }
    t=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
    c=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
    startup=$(echo "$out" | grep -E '^Starting|^  (segment|sub-block|sparse cutoff|medium-tier prefetchnta)' \
        | sed -E 's/^Starting [0-9]+ threads, limit=[0-9]+, (segment=[0-9]+), wheel mod [0-9]+ \([0-9]+ primes\), ([0-9]+ small[^,]*\(sub-block [^)]*\))?.*/\1 \2/' \
        | sed 's/^ *//' | paste -sd ';' - | sed 's/;/; /g')
}

bash scripts/machine_info.sh "$THREADS" "last ${WIDTH_IN} below ${N_IN}, A/B x$REPS"
echo "A: ${A_CFG:-auto}"
echo "B: $B_CFG"

ta=(); tb=(); ca=""; cb=""
for ((r = 1; r <= REPS; r++)); do
    run_cfg "$A_CFG"; ta+=("$t"); ca=$c
    [ $r -eq 1 ] && echo "  A config: $startup"
    echo "  A rep $r: ${t}s"
    run_cfg "$B_CFG"; tb+=("$t"); cb=$c
    [ $r -eq 1 ] && echo "  B config: $startup"
    echo "  B rep $r: ${t}s"
done
if [ "$ca" != "$cb" ]; then echo "  COUNT MISMATCH: A $ca vs B $cb" >&2; exit 1; fi

mean() { printf '%s\n' "$@" | awk '{s += $1} END {printf "%.2f", s / NR}'; }
ma=$(mean "${ta[@]}"); mb=$(mean "${tb[@]}")
echo
echo "A ${A_CFG:-auto}: ${ta[*]} -> mean ${ma}s"
echo "B $B_CFG: ${tb[*]} -> mean ${mb}s"
awk -v a="$ma" -v b="$mb" 'BEGIN { printf "B vs A: %+.1f%%\n", (b - a) / a * 100 }'
# every B run below every A run (or above): the sign is solid even at REPS=2
awk -v A="${ta[*]}" -v B="${tb[*]}" 'BEGIN {
    na = split(A, a, " "); nb = split(B, b, " "); below = 1; above = 1
    for (i = 1; i <= nb; i++) for (j = 1; j <= na; j++) { if (b[i] >= a[j]) below = 0; if (b[i] <= a[j]) above = 0 }
    if (below) print "every B run below every A run"; else if (above) print "every B run above every A run"; else print "runs overlap: the sign is not settled"
}'
