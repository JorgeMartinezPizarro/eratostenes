#!/bin/bash
# Prueba de regresion: compara pi(N) contra el valor de referencia conocido
# para N = 1e8 .. 1e11, usando --count-only (sin E/S a disco). Pensado para
# `make test`, pero tambien se puede correr suelto (./test.sh) siempre que
# el binario ya este compilado.
set -u

BIN=./eratostenes
THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

if [ ! -x "$BIN" ]; then
    echo "Error: no se encuentra $BIN compilado. Ejecuta 'make' primero." >&2
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
    output=$("$BIN" -n "$n" -t "$THREADS" --count-only 2>&1)
    end=$(date +%s.%N)
    elapsed=$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.2f", b - a }')

    actual=$(echo "$output" | sed -nE 's/Listo\. ([0-9]+) primos.*/\1/p')

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

if [ "$fail" -eq 0 ]; then
    echo "Todas las pruebas OK."
else
    echo "Alguna prueba fallo." >&2
fi
exit "$fail"
