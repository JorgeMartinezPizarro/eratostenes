#!/bin/bash
# Prueba de regresion, cuatro partes:
#   1. Compara pi(N) contra primecount (--nth-prime/plain, independiente de
#      este proyecto -- ver https://github.com/kimwalisch/primecount) para
#      N = 1e8..1e11, usando --count-only (sin E/S a disco).
#   2. Construye un .db real en N=1e10 y comprueba un puñado de posiciones
#      ancla mas varios cientos de posiciones aleatorias, todas contra
#      primecount --nth-prime (ver README, seccion ".db output").
#   3. Varias combinaciones de -t/-s/--l1-bytes/--l2-bytes/--db-block-size/
#      --zstd-level a la vez (no una por una) en un N pequeño (1e7), cada
#      una en --count-only y en .db: ninguna deberia cambiar el resultado,
#      solo como se calcula o se empaqueta.
#   4. Round-trip texto vs .db en N=1e5..1e7: mismo pi(N) y, posicion por
#      posicion (todas en el N mas chico, una muestra aleatoria en los
#      demas -- comprobar cada posicion via nth_prime implica un proceso
#      por consulta, caro a partir de cientos de miles de primos), el mismo
#      primo en ambos formatos (antes scripts/verify_db.sh, fusionado aqui).
# Los valores esperados en 1, 2 y 3 salen de primecount, no de constantes
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

random_positions() {
    local count="$1" n="$2"
    shuf -i "1-$n" -n "$count" 2>/dev/null || \
        awk -v n="$n" -v s="$count" 'BEGIN { srand(); for (i = 0; i < s; i++) print int(rand() * n) + 1 }'
}

# Corre eratostenes N --count-only (con args extra, p.ej. -t/-s) y compara
# contra primecount. Usado por las partes 1 y 3.
check_count_only() {
    local n="$1" expected="$2" label="$3"; shift 3
    local output actual
    output=$("$BIN" "$n" "$@" --count-only 2>&1)
    actual=$(echo "$output" | sed -nE 's/Listo\. ([0-9,]+) primos.*/\1/p' | tr -d ',')
    if [ "$actual" == "$expected" ]; then
        printf "OK   %-40s pi(N)=%s\n" "$label" "$actual"
    else
        printf "FAIL %-40s esperado=%s obtenido=%s\n" "$label" "$expected" "${actual:-<sin salida>}"
        echo "--- salida completa ---"
        echo "$output"
        echo "-----------------------"
        fail=1
    fi
}

# Comprueba `sample` posiciones aleatorias (mas cualquiera en `anchors`) de
# un .db contra primecount --nth-prime y el --count total. Una sola linea
# de resumen (no una por posicion -- serian cientos), detalle solo si algo
# falla. Usado por las partes 2 y 3.
check_db_positions() {
    local db="$1" expected_count="$2" sample="$3" label="$4"; shift 4
    local anchors=("$@")

    local actual_count
    actual_count=$("$NTH_BIN" "$db" --count 2>&1)
    if [ "$actual_count" != "$expected_count" ]; then
        printf "FAIL %-30s --count esperado=%s obtenido=%s\n" "$label" "$expected_count" "${actual_count:-<sin salida>}"
        fail=1
        return
    fi

    local positions=("${anchors[@]}")
    while IFS= read -r pos; do positions+=("$pos"); done < <(random_positions "$sample" "$expected_count")

    local checked=0 mismatch=0
    for pos in "${positions[@]}"; do
        local expected actual
        expected=$("$PRIMECOUNT" "$pos" --nth-prime)
        actual=$("$NTH_BIN" "$db" "$pos" 2>&1)
        checked=$((checked + 1))
        if [ "$expected" != "$actual" ]; then
            echo "  posicion $pos: primecount=$expected nth_prime=$actual"
            mismatch=1
        fi
    done

    if [ "$mismatch" -eq 0 ]; then
        printf "OK   %-30s --count=%s, %s posiciones (vs primecount)\n" "$label" "$actual_count" "$checked"
    else
        printf "FAIL %-30s --count=%s, %s posiciones comprobadas\n" "$label" "$actual_count" "$checked"
        fail=1
    fi
}

# --- 1: pi(N) por --count-only, contra primecount ---
NS=(100000000 1000000000 10000000000 100000000000)

for n in "${NS[@]}"; do
    expected=$("$PRIMECOUNT" "$n")
    start=$(date +%s.%N)
    check_count_only "$n" "$expected" "N=$n" -t "$THREADS"
    end=$(date +%s.%N)
    awk -v a="$start" -v b="$end" -v n="$n" 'BEGIN { printf "       (%.2fs)\n", b - a }'
done

# --- 2: .db en N=1e10, posiciones ancla + 300 aleatorias contra primecount ---
DB="$WORKDIR/n1e10.db"
"$BIN" 10000000000 -t "$THREADS" -o "$DB" >/dev/null 2>&1
expected_pi_1e10=$("$PRIMECOUNT" 10000000000)
check_db_positions "$DB" "$expected_pi_1e10" 300 ".db N=1e10" 1 1000 10000 200000000 "$expected_pi_1e10"
rm -f "$DB"

# --- 3: combinaciones de parametros en un N pequeño (1e7) -- no busca N
# mas grande, busca que una buena variedad de combinaciones de -t/-s/
# --l1-bytes/--l2-bytes/--db-block-size/--zstd-level, TODAS A LA VEZ (no
# una por una), sigan dando el resultado correcto tanto en --count-only
# como en .db. Una sola linea por combinacion.
N3=10000000
expected_pi_1e7=$("$PRIMECOUNT" "$N3")

# Corre una combinacion de flags en --count-only y en .db (con 10
# posiciones aleatorias via nth_prime), ambas contra primecount; una sola
# linea de resultado por combinacion.
check_combo() {
    local label="$1"; shift
    local out actual db actual_count ok=1

    out=$("$BIN" "$N3" "$@" --count-only 2>&1)
    actual=$(echo "$out" | sed -nE 's/Listo\. ([0-9,]+) primos.*/\1/p' | tr -d ',')
    [ "$actual" == "$expected_pi_1e7" ] || ok=0

    db="$WORKDIR/combo.db"
    "$BIN" "$N3" "$@" -o "$db" >/dev/null 2>&1
    actual_count=$("$NTH_BIN" "$db" --count 2>&1)
    [ "$actual_count" == "$expected_pi_1e7" ] || ok=0

    if [ "$ok" -eq 1 ]; then
        while IFS= read -r pos; do
            local expected got
            expected=$("$PRIMECOUNT" "$pos" --nth-prime)
            got=$("$NTH_BIN" "$db" "$pos" 2>&1)
            [ "$expected" == "$got" ] || ok=0
        done < <(random_positions 10 "$expected_pi_1e7")
    fi
    rm -f "$db"

    if [ "$ok" -eq 1 ]; then
        printf "OK   %s\n" "$label"
    else
        printf "FAIL %s (count-only=%s .db-count=%s)\n" "$label" "$actual" "$actual_count"
        fail=1
    fi
}

check_combo "combo default"                    -t "$THREADS"
check_combo "combo 1h/cache chica/bloque chico" -t 1 -s 2000 --l1-bytes 16384 --l2-bytes 131072 --db-block-size 200 --zstd-level 19
check_combo "combo 2h/segmento grande/bloque grande" -t 2 -s 5000000 --db-block-size 500000 --zstd-level 1
check_combo "combo cache forzada grande"       -t "$THREADS" --l1-bytes 1048576 --l2-bytes 8388608 --db-block-size 65536 --zstd-level 9
check_combo "combo segmento minimo"            -t "$THREADS" -s 64 --db-block-size 100 --zstd-level 1

# --- 4: round-trip texto vs .db, posicion por posicion ---
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
