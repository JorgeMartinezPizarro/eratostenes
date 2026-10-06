#!/bin/bash
# Shared helpers for the benchmark scripts (benchmark_tails.sh,
# benchmark_mini.sh, benchmark_ab.sh): `source scripts/lib.sh` after the
# cd to the repo root. Nothing here runs anything.

# "1e13" / "100000" -> plain digits. Numbers stay decimal strings: N goes up
# to 2^64 - 1, and bash arithmetic stops at 2^63 - 1 (1e19 overflowed it).
to_dec() {
    local d
    if [[ "$1" =~ ^([0-9]+)[eE]([0-9]+)$ ]]; then
        d=${BASH_REMATCH[1]}$(printf '%*s' "${BASH_REMATCH[2]}" '' | tr ' ' 0)
    elif [[ "$1" =~ ^[0-9]+$ ]]; then
        d=$1
    else
        echo "valor no valido: $1 (usa un entero o 1eX)" >&2
        return 1
    fi
    d=$(echo "$d" | sed 's/^0*//')
    echo "${d:-0}"
}

# a < b for decimal numbers bash can't compare itself (seconds with a
# fraction): exit status 0 when true.
num_lt() { awk -v a="$1" -v b="$2" 'BEGIN{exit !(a < b)}'; }

# mean_of DECIMALS t1 t2 ...: the mean of the times, printed with that many
# decimals (3 for both eratostenes' `total:` and primesieve's `Seconds:`;
# eratostenes printed 2 until 2026-10-07).
mean_of() {
    local prec=$1; shift
    printf '%s\n' "$@" | awk -v p="$prec" '{s += $1; n++} END{printf "%." p "f", s / n}'
}

