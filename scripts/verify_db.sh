#!/bin/bash
# Verifica el modo de salida .db (SQLite + zstd, ver src/gap_block_sink.hpp
# y src/sqlite_prime_store.hpp) contra el modo texto ya validado por
# test.sh: mismo total de primos (pi(N)) y el primo en cada posicion
# comprobada coincide. Para el N mas pequeno comprueba TODAS las
# posiciones; para los mayores, una muestra aleatoria (comprobar cada
# posicion via nth_prime implica un proceso por consulta, caro a partir de
# cientos de miles de primos).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
NTH=./nth_prime
THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

if [ ! -x "$BIN" ] || [ ! -x "$NTH" ]; then
    echo "Error: falta $BIN o $NTH compilados. Ejecuta 'make' primero." >&2
    exit 1
fi

# N -> cuantas posiciones comprobar (0 = todas)
NS=(100000 1000000 10000000)
SAMPLES=(0 300 300)

random_positions() {
    local count="$1" n="$2"
    shuf -i "1-$n" -n "$count" 2>/dev/null || \
        awk -v n="$n" -v s="$count" 'BEGIN { srand(); for (i = 0; i < s; i++) print int(rand() * n) + 1 }'
}

fail=0
for i in "${!NS[@]}"; do
    n="${NS[$i]}"
    sample="${SAMPLES[$i]}"
    txt="$WORKDIR/n$n.txt"
    db="$WORKDIR/n$n.db"

    "$BIN" -n "$n" -t "$THREADS" -o "$txt" >/dev/null 2>&1
    "$BIN" -n "$n" -t "$THREADS" -o "$db"  >/dev/null 2>&1

    mapfile -t lines < "$txt"
    expected_count=${#lines[@]}
    actual_count=$("$NTH" "$db" --count)

    if [ "$actual_count" != "$expected_count" ]; then
        printf "FAIL N=%-12s total: txt=%s db=%s\n" "$n" "$expected_count" "$actual_count"
        fail=1
        continue
    fi

    if [ "$sample" -eq 0 ]; then
        positions=$(seq 1 "$expected_count")
    else
        positions=$(random_positions "$sample" "$expected_count")
    fi

    mismatch=0
    checked=0
    for pos in $positions; do
        expected="${lines[$((pos - 1))]}"
        actual=$("$NTH" "$db" "$pos")
        checked=$((checked + 1))
        if [ "$expected" != "$actual" ]; then
            echo "  posicion $pos: txt=$expected db=$actual"
            mismatch=1
        fi
    done

    if [ "$mismatch" -eq 0 ]; then
        printf "OK   N=%-12s pi(N)=%-10s posiciones comprobadas=%s\n" "$n" "$expected_count" "$checked"
    else
        printf "FAIL N=%-12s pi(N)=%-10s posiciones comprobadas=%s\n" "$n" "$expected_count" "$checked"
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "Todas las pruebas OK."
else
    echo "Alguna prueba fallo." >&2
fi
exit "$fail"
