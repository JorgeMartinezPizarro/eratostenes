#!/bin/bash
# Prueba de regresion: compara pi(N) contra el valor de referencia conocido
# para N = 1e8 .. 1e11, usando --count-only (sin E/S a disco); y, aparte,
# construye un .db real en N=1e10 y comprueba unos cuantos primos conocidos
# por posicion via nth_prime (ver README, seccion ".db output"). Pensado
# para `make test`, pero tambien se puede correr suelto (./test.sh) siempre
# que los binarios ya esten compilados.
set -u

BIN=./eratostenes
NTH_BIN=./nth_prime
THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

if [ ! -x "$BIN" ] || [ ! -x "$NTH_BIN" ]; then
    echo "Error: no se encuentra $BIN o $NTH_BIN compilados. Ejecuta 'make' primero." >&2
    exit 1
fi

# N -> pi(N) conocido
NS=(100000000 1000000000 10000000000 100000000000)
EXPECTED=(5761455 50847534 455052511 4118054813)

fail=0
for i in "${!NS[@]}"; do
    n="${NS[$i]}"
    expected="${EXPECTED[$i]}"

    start=$(date +%s.%N)
    output=$("$BIN" "$n" -t "$THREADS" --count-only 2>&1)
    end=$(date +%s.%N)
    elapsed=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.2f", b - a }')

    actual=$(echo "$output" | sed -nE 's/Listo\. ([0-9,]+) primos.*/\1/p' | tr -d ',')

    if [ "$actual" == "$expected" ]; then
        printf "OK   N=%-15s pi(N)=%-12s (%ss)\n" "$n" "$actual" "$elapsed"
    else
        printf "FAIL N=%-15s esperado=%-12s obtenido=%s\n" "$n" "$expected" "${actual:-<sin salida>}"
        echo "--- salida completa ---"
        echo "$output"
        echo "-----------------------"
        fail=1
    fi
done


# --- .db: construye una vez en N=1e10 y comprueba primos conocidos por
# posicion (el primero, dos valores de referencia bien conocidos -- el
# primo 1000 y el 10000 -- el primo 200 millones, y el ultimo) via
# nth_prime, el lector de .db. ---
DB=$(mktemp --suffix=.db)
trap 'rm -f "$DB"' EXIT

"$BIN" 10000000000 -t "$THREADS" -o "$DB" >/dev/null 2>&1

# posicion (1-indexada, N=1 -> 2) -> primo N-esimo conocido
POS=(1 1000 10000 200000000 455052511)
PRIMES=(2 7919 104729 4222234741 9999999967)

for i in "${!POS[@]}"; do
    pos="${POS[$i]}"
    expected="${PRIMES[$i]}"
    actual=$("$NTH_BIN" "$DB" "$pos" 2>&1)

    if [ "$actual" == "$expected" ]; then
        printf "OK   .db N=1e10 pos=%-12s primo=%s\n" "$pos" "$actual"
    else
        printf "FAIL .db N=1e10 pos=%-12s esperado=%-12s obtenido=%s\n" "$pos" "$expected" "${actual:-<sin salida>}"
        fail=1
    fi
done

count=$("$NTH_BIN" "$DB" --count 2>&1)
if [ "$count" == "455052511" ]; then
    printf "OK   .db N=1e10 --count=%s\n" "$count"
else
    printf "FAIL .db N=1e10 --count esperado=455052511 obtenido=%s\n" "${count:-<sin salida>}"
    fail=1
fi

rm -f "$DB"
trap - EXIT

if [ "$fail" -eq 0 ]; then
    echo "Todas las pruebas OK."
else
    echo "Alguna prueba fallo." >&2
fi
exit "$fail"
