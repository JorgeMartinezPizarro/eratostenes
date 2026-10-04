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

# Mean of its arguments (seconds with a fraction), two decimals, as the
# benchmark tables print it.
mean_of() {
    printf '%s\n' "$@" | awk '{s += $1; n++} END{printf "%.2f", s / n}'
}

# warm_up SECONDS BIN THREADS: SECONDS of all-core load before the first
# measured run, so the table reads the sustained power regime (PL1) and not
# the turbo budget a machine spends in its first seconds from idle
# (docs/RESEARCH.md, "Two power regimes on every machine"). 0 = none.
# A 1e14 count never finishes within the window on any machine, so the
# load is steady until timeout kills it.
warm_up() {
    (( $1 > 0 )) || return 0
    echo "  calentamiento: ${1}s de carga en $3 hilos" >&2
    # Background job killed by hand, not `timeout`: a non-interactive bash
    # says nothing about a background job dying by signal, so the log stays
    # clean.
    "$2" 1e14 -t "$3" >/dev/null 2>&1 &
    local pid=$!
    sleep "$1"
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}
