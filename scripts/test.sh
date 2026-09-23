#!/bin/bash
# Prueba de regresion, tres partes:
#   1. Compara pi(N) contra primecount (--nth-prime/plain, independiente de
#      este proyecto -- ver https://github.com/kimwalisch/primecount) para
#      N = 1e8..1e11, usando --count-only (sin E/S a disco).
#   2. Construye un .db real en N=1e10 y comprueba unos cuantos primos por
#      posicion via nth_prime contra primecount --nth-prime (ver README,
#      seccion ".db output").
#   3. Round-trip texto vs .db en N=1e5..1e7: mismo pi(N) y, posicion por
#      posicion (todas en el N mas chico, una muestra aleatoria en los
#      demas -- comprobar cada posicion via nth_prime implica un proceso
#      por consulta, caro a partir de cientos de miles de primos), el mismo
#      primo en ambos formatos (antes scripts/verify_db.sh, fusionado aqui).
# Los valores esperados en 1 y 2 salen de primecount, no de constantes
# hardcodeadas -- necesita estar instalado (Debian/Ubuntu: paquete
# primecount-bin; ver docker/Dockerfile, etapa "dev").
# Pensado para `make test`, pero tambien se puede correr suelto
# (./scripts/test.sh) siempre que los binarios ya esten compilados.
set -u
cd "$(dirname "$0")/.."

BIN=./eratostenes
NTH_BIN=./nth_prime
PRIMECOUNT=${PRIMECOUNT:-primecount}
THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

if [ ! -x "$BIN" ] || [ ! -x "$NTH_BIN" ]; then
    echo "Error: no se encuentra $BIN o $NTH_BIN compilados. Ejecuta 'make' primero." >&2
    exit 1
fi
if ! command -v "$PRIMECOUNT" >/dev/null 2>&1; then
    echo "Error: no se encuentra '$PRIMECOUNT' (paquete primecount-bin) -- los valores" >&2
    echo "       esperados de este test salen de ahi, no de constantes hardcodeadas." >&2
    exit 1
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

fail=0

# --- 1: pi(N) por --count-only, contra primecount ---
NS=(100000000 1000000000 10000000000 100000000000)

for i in "${!NS[@]}"; do
    n="${NS[$i]}"
    expected=$("$PRIMECOUNT" "$n")

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

# --- 2: .db en N=1e10, primos por posicion contra primecount --nth-prime ---
DB="$WORKDIR/n1e10.db"
"$BIN" 10000000000 -t "$THREADS" -o "$DB" >/dev/null 2>&1

expected_pi_1e10=$("$PRIMECOUNT" 10000000000)
POS=(1 1000 10000 200000000 "$expected_pi_1e10")

for pos in "${POS[@]}"; do
    expected=$("$PRIMECOUNT" "$pos" --nth-prime)
    actual=$("$NTH_BIN" "$DB" "$pos" 2>&1)

    if [ "$actual" == "$expected" ]; then
        printf "OK   .db N=1e10 pos=%-12s primo=%s\n" "$pos" "$actual"
    else
        printf "FAIL .db N=1e10 pos=%-12s esperado=%-12s obtenido=%s\n" "$pos" "$expected" "${actual:-<sin salida>}"
        fail=1
    fi
done

count=$("$NTH_BIN" "$DB" --count 2>&1)
if [ "$count" == "$expected_pi_1e10" ]; then
    printf "OK   .db N=1e10 --count=%s\n" "$count"
else
    printf "FAIL .db N=1e10 --count esperado=%s obtenido=%s\n" "$expected_pi_1e10" "${count:-<sin salida>}"
    fail=1
fi
rm -f "$DB"

# --- 3: round-trip texto vs .db, posicion por posicion ---
random_positions() {
    local count="$1" n="$2"
    shuf -i "1-$n" -n "$count" 2>/dev/null || \
        awk -v n="$n" -v s="$count" 'BEGIN { srand(); for (i = 0; i < s; i++) print int(rand() * n) + 1 }'
}

NS2=(100000 1000000 10000000)
SAMPLES=(0 300 300)

for i in "${!NS2[@]}"; do
    n="${NS2[$i]}"
    sample="${SAMPLES[$i]}"
    txt="$WORKDIR/rt$n.txt"
    db="$WORKDIR/rt$n.db"

    "$BIN" "$n" -t "$THREADS" -o "$txt" >/dev/null 2>&1
    "$BIN" "$n" -t "$THREADS" -o "$db"  >/dev/null 2>&1

    mapfile -t lines < "$txt"
    expected_count=${#lines[@]}
    actual_count=$("$NTH_BIN" "$db" --count)

    if [ "$actual_count" != "$expected_count" ]; then
        printf "FAIL round-trip N=%-12s total: txt=%s db=%s\n" "$n" "$expected_count" "$actual_count"
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
        actual=$("$NTH_BIN" "$db" "$pos")
        checked=$((checked + 1))
        if [ "$expected" != "$actual" ]; then
            echo "  posicion $pos: txt=$expected db=$actual"
            mismatch=1
        fi
    done

    if [ "$mismatch" -eq 0 ]; then
        printf "OK   round-trip N=%-12s pi(N)=%-10s posiciones comprobadas=%s\n" "$n" "$expected_count" "$checked"
    else
        printf "FAIL round-trip N=%-12s pi(N)=%-10s posiciones comprobadas=%s\n" "$n" "$expected_count" "$checked"
        fail=1
    fi
done

if [ "$fail" -eq 0 ]; then
    echo "Todas las pruebas OK."
else
    echo "Alguna prueba fallo." >&2
fi
exit "$fail"
