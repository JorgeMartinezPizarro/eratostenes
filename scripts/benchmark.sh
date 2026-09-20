#!/bin/bash
# Sweeps WHEEL_PRIMES (mod 30) x N (1e9..1e13), rebuilding between each,
# and prints a timing table. Restores whatever wheel was active before
# running and leaves the binary rebuilt with it.
#
# mod 6 and mod 210 were tried against mod 30 up to 1e13 (see the
# i5-11400F section of README#benchmarks): mod 6 avoids mod 30's L3-cliff
# ratio jump but isn't actually faster (less wheel-filtering costs about
# as much as the cache cliff saves), and mod 210's table grows too fast to
# be worth it past 1e10. mod 30 stays the one worth tracking here; add
# wheels back to MODS below if that ever changes.
#
# Usage: ./scripts/benchmark.sh
# Env overrides: THREADS (default: nproc), SEGMENT (default: 10000000 --
# the width the L3-cliff comparison in the README used; the tool's own CLI
# default is smaller, 4194304), REPS (default: 1, keeps the fastest of
# REPS runs per N/wheel -- there's real run-to-run noise on this kind of
# box, see BENCHMARK section of the README/commit history).
set -euo pipefail
cd "$(dirname "$0")/.."

THREADS="${THREADS:-$(nproc)}"
SEGMENT="${SEGMENT:-10000000}"
REPS="${REPS:-1}"

MODS=(30)
NS=(1e9 1e10 1e11 1e12 1e13)
# pi(N) for each N above, in the same order -- known values, used to catch
# a silently-wrong build instead of just reporting a (meaningless) time for
# one. See scripts/test.sh for the same values at other N.
EXPECTED=(50847534 455052511 4118054813 37607912018 346065536839)

WHEEL_FILE=src/wheel.hpp

# Only rewrites the *active config* block (the lines after `#include
# <array>`), never the documentation block above it -- that block lists the
# exact same lines as commented-out examples, and a pattern match that
# isn't scoped this way can silently uncomment a line there too, giving two
# active WHEEL_PRIMES definitions (a hard compile error, but a confusing
# one if you don't know to look for it -- ask me how I know).
set_wheel() {
    local mod=$1
    sed -i "/#include <array>/,\$ s|^\( *constexpr.*WHEEL_PRIMES = .*\)|//\1|" "$WHEEL_FILE"
    sed -i "/#include <array>/,\$ s|^//\( *constexpr.*WHEEL_PRIMES = .*// mod ${mod}\)\$|\1|" "$WHEEL_FILE"

    local active
    active=$(grep -c "^ *constexpr.*WHEEL_PRIMES = " "$WHEEL_FILE")
    if [ "$active" -ne 1 ]; then
        echo "set_wheel $mod: se esperaba 1 linea WHEEL_PRIMES activa, hay $active." >&2
        git diff -- "$WHEEL_FILE" >&2
        exit 1
    fi
}

ORIG_WHEEL=$(mktemp)
cp "$WHEEL_FILE" "$ORIG_WHEEL"
cleanup() {
    cp "$ORIG_WHEEL" "$WHEEL_FILE"
    rm -f "$ORIG_WHEEL"
    echo "Reconstruyendo con la rueda original..." >&2
    make re >/tmp/benchmark_build.log 2>&1 || cat /tmp/benchmark_build.log >&2
}
trap cleanup EXIT

declare -A RESULT

for mod in "${MODS[@]}"; do
    set_wheel "$mod"
    echo "== compilando mod $mod ==" >&2
    if ! make re >/tmp/benchmark_build.log 2>&1; then
        echo "build fallo para mod $mod:" >&2
        cat /tmp/benchmark_build.log >&2
        exit 1
    fi

    for i in "${!NS[@]}"; do
        n="${NS[$i]}"
        expected="${EXPECTED[$i]}"
        best=""
        for ((r = 1; r <= REPS; r++)); do
            out=$(./eratostenes "$n" -t "$THREADS" -s "$SEGMENT" --count-only 2>&1)
            t=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
            count=$(echo "$out" | sed -nE 's/.*Listo\. ([0-9,]+) primos.*/\1/p' | tr -d ',')
            mark="ok"
            [ "$count" = "$expected" ] || mark="pi(N) MAL: obtenido $count, esperado $expected"
            echo "  mod=$mod n=$n rep=$r t=${t}s [$mark]" >&2
            if [ "$mark" != "ok" ]; then
                echo "Abortando: resultado incorrecto, no tiene sentido seguir midiendo." >&2
                exit 1
            fi
            if [ -z "$best" ] || awk -v a="$t" -v b="$best" 'BEGIN{exit !(a<b)}'; then
                best="$t"
            fi
        done
        RESULT["$mod,$n"]="$best"
    done
done

echo
printf "| N"
for mod in "${MODS[@]}"; do printf " | mod %s" "$mod"; done
printf " |\n|---"
for _ in "${MODS[@]}"; do printf "|---:"; done
printf "|\n"
for n in "${NS[@]}"; do
    printf "| %s" "$n"
    for mod in "${MODS[@]}"; do
        printf " | %ss" "${RESULT[$mod,$n]}"
    done
    printf " |\n"
done
