#!/bin/bash
# Prueba de regresion, cinco partes:
#   1. Compara pi(N) contra primecount (--nth-prime/plain, independiente de
#      este proyecto -- ver https://github.com/kimwalisch/primecount) para
#      N = 1e8..1e11, sin -o (modo conteo, sin E/S a disco).
#   2. Construye un .db real en N=1e10 y comprueba un puñado de posiciones
#      ancla mas varios cientos de posiciones aleatorias, todas contra
#      primecount --nth-prime (ver README, seccion ".db output").
#   3. Varias combinaciones de -t/-s/--l1-bytes/--l2-bytes/--db-block-size/
#      --zstd-level/--tune a la vez (no una por una) en un N pequeño (1e7),
#      cada una sin -o (modo conteo) y en .db: ninguna deberia cambiar el
#      resultado, solo como se calcula o se empaqueta.
#   4. Round-trip texto vs .db en N=1e5..1e7: mismo pi(N) y, posicion por
#      posicion (todas en el N mas chico, una muestra aleatoria en los
#      demas -- comprobar cada posicion via nth_prime implica un proceso
#      por consulta, caro a partir de cientos de miles de primos), el mismo
#      primo en ambos formatos (antes scripts/verify_db.sh, fusionado aqui).
#   5. Modo conteo en N=1e10 contra primecount con los caminos que solo
#      aparecen con N grande o -s pequeño (tier disperso forzado, en rueda
#      mod 2310 y mod 210; sin med64; cortes rebajados, combinados), y
#      --start sobre varios tramos (el recuento es pi(N) - pi(N0 - 1)),
#      uno de ellos por encima de 2^53 (parseo exacto de N).
#   6. Limites pequenos (N < 2, N < 7) en los tres modos, argumentos que deben
#      rechazarse, y las reglas de ajuste del segmento leidas en el log de
#      arranque con --l1-bytes/--l2-bytes forzados (deterministas).
#   7. Un tramo del regimen sparse real (ultima 1e8 bajo 1e15) contra
#      primecount, y los errores que nth_prime debe detectar (.blk truncado
#      o ausente, posiciones fuera de rango).
# Los valores esperados en 1, 2, 3 y 5 salen de primecount, no de constantes
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

# Corre eratostenes N sin -o (modo conteo, con args extra, p.ej. -t/-s) y
# compara contra primecount. Usado por las partes 1 y 3.
check_count_only() {
    local n="$1" expected="$2" label="$3"; shift 3
    local output actual
    output=$("$BIN" "$n" "$@" 2>&1)
    actual=$(echo "$output" | sed -nE 's/Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
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

# --- 1: pi(N) sin -o (modo conteo), contra primecount ---
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
# una por una), sigan dando el resultado correcto tanto sin -o (modo
# conteo) como en .db. Una sola linea por combinacion.
N3=10000000
expected_pi_1e7=$("$PRIMECOUNT" "$N3")

# Corre una combinacion de flags sin -o (modo conteo) y en .db (con 10
# posiciones aleatorias via nth_prime), ambas contra primecount; una sola
# linea de resultado por combinacion.
check_combo() {
    local label="$1"; shift
    local out actual db actual_count ok=1

    out=$("$BIN" "$N3" "$@" 2>&1)
    actual=$(echo "$out" | sed -nE 's/Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
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
check_combo "combo disperso"                   -t 3 -s 2000 --db-block-size 1000
check_combo "combo cortes rebajados"           -t "$THREADS" -s 100000 --tune small=1/2 --tune med64=1/8 --tune sparse=1/2
check_combo "combo sin med64/disperso"         -t 2 -s 2000 --tune med64=0 --zstd-level 3
check_combo "combo NTA medio forzado"          -t "$THREADS" -s 100000 --tune medium_nta=1

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


# --- 5: caminos de N grande y --start, modo conteo, contra primecount ---
N5=10000000000
expected_pi_1e10=$("$PRIMECOUNT" "$N5")
check_count_only "$N5" "$expected_pi_1e10" "disperso forzado"              -t "$THREADS" -s 100000
check_count_only "$N5" "$expected_pi_1e10" "sin med64, disperso forzado"   -t 5 -s 500000 --tune med64=0
check_count_only "$N5" "$expected_pi_1e10" "cortes small/med64/sparse"     -t "$THREADS" --tune small=1/8 --tune med64=1/6 --tune sparse=1/4
check_count_only "$N5" "$expected_pi_1e10" "NTA medio forzado on"          -t "$THREADS" --tune medium_nta=1
check_count_only "$N5" "$expected_pi_1e10" "NTA medio forzado off"         -t "$THREADS" -s 100000 --tune medium_nta=0
check_count_only "$N5" "$expected_pi_1e10" "anillo sparse: paginas grandes on"  -t "$THREADS" -s 100000 --tune huge=1
check_count_only "$N5" "$expected_pi_1e10" "anillo sparse: paginas grandes off" -t 2 -s 100000 --tune huge=0

# --start N0: el recuento es el del tramo [N0, N], pi(N) - pi(N0 - 1).
check_start() {
    local n="$1" n0="$2"; shift 2
    local expected=$(( $("$PRIMECOUNT" "$n") - $("$PRIMECOUNT" $(( n0 - 1 ))) ))
    check_count_only "$n" "$expected" "--start $n0 (N=$n) $*" --start "$n0" "$@"
}
check_start "$N5" 9000000000 -t "$THREADS"
check_start "$N5" 9999000001 -t 3 -s 100000
check_start 1000000 2 -t "$THREADS"
check_start 100000000 7 -t 2 -s 20000
# Por encima de 2^53 (~9.007e15): 9007199254740997 es primo y = 1 mod 4, asi
# que leerlo via double lo redondeaba a ...996 y el tramo perdia ese primo.
check_start 9007199254740997 9007199254739761 -t "$THREADS"
# Arranque no alineado a 64 indices de la rueda (19999900000000000 = 10 mod 30):
# split_ranges redondea hacia abajo y los primos de esa cabecera no cuentan
# (3 de mas hasta 2026-10-04).
check_start 20000000000000000 19999900000000000 -t 2
check_start 20000000000000000 19999900000000000 -t 1
# Un primo de arranque cuyo indice de rueda es 63 mod 64 (239 -> 63, 2399 ->
# 639): split_ranges lo dejaba fuera del primer chunk (una menos que
# primesieve hasta 2026-10-05); 240 con --start 239 cribaba un rango vacio.
check_start 1000000 239 -t 2
check_start 1000000 2399 -t 1
check_start 240 239
check_start 241 239
if "$BIN" 1000000 --start 1000 -o "$WORKDIR/start.txt" >/dev/null 2>&1; then
    printf "FAIL %-40s deberia rechazarse\n" "--start con -o"
    fail=1
else
    printf "OK   %-40s rechazado\n" "--start con -o"
fi


# --- 6: limites pequenos, rechazos y reglas de ajuste (todo instantaneo) ---

# N < 2 y N < 7 (los caminos cortos de main) y un N normal pequeno, en
# conteo, .txt y .db; el ultimo primo del .db contra la ultima linea del .txt.
check_tiny() {
    local n="$1" expected="$2" out c lines dbc
    out=$("$BIN" "$n" 2>&1)
    c=$(echo "$out" | sed -nE 's/.*Done\. ([0-9]+) prime.*/\1/p')
    [ "$c" == "$expected" ] || { printf "FAIL tiny N=%-4s conteo=%s esperado=%s\n" "$n" "$c" "$expected"; fail=1; return; }
    "$BIN" "$n" -o "$WORKDIR/tiny.txt" >/dev/null 2>&1
    lines=$(grep -c . "$WORKDIR/tiny.txt" || true)
    [ "$lines" == "$expected" ] || { printf "FAIL tiny N=%-4s txt=%s esperado=%s\n" "$n" "$lines" "$expected"; fail=1; return; }
    "$BIN" "$n" -o "$WORKDIR/tiny.db" >/dev/null 2>&1
    dbc=$("$NTH_BIN" "$WORKDIR/tiny.db" --count 2>&1)
    [ "$dbc" == "$expected" ] || { printf "FAIL tiny N=%-4s db=%s esperado=%s\n" "$n" "$dbc" "$expected"; fail=1; return; }
    if [ "$expected" -gt 0 ]; then
        local last want
        last=$("$NTH_BIN" "$WORKDIR/tiny.db" "$expected" 2>&1)
        want=$(tail -1 "$WORKDIR/tiny.txt")
        [ "$last" == "$want" ] || { printf "FAIL tiny N=%-4s db[%s]=%s txt=%s\n" "$n" "$expected" "$last" "$want"; fail=1; return; }
    fi
    printf "OK   tiny N=%-4s pi(N)=%-3s (conteo, txt, db)\n" "$n" "$expected"
}
check_tiny 0 0
check_tiny 1 0
check_tiny 2 1
check_tiny 6 3
check_tiny 7 4
check_tiny 30 10
check_tiny 100 25

# Argumentos que deben rechazarse antes de cribar nada.
expect_reject() {
    local label="$1"; shift
    if "$BIN" "$@" >/dev/null 2>&1; then
        printf "FAIL %-40s deberia rechazarse\n" "$label"; fail=1
    else
        printf "OK   %-40s rechazado\n" "$label"
    fi
}
expect_reject "--start = N"              1000000 --start 1000000
expect_reject "--start > N"              1000000 --start 2000000
expect_reject "N > MAX_LIMIT"            18446744073709551615
expect_reject "N = 1e20"                 1e20
expect_reject "N = 2.5 (no entero)"      2.5
expect_reject "sin N"                    -t 2
expect_reject "segundo N posicional"     1000 2000
expect_reject "-t -1"                    1000 -t -1
expect_reject "-s sin valor"             1000 -s
expect_reject "opcion desconocida"       1000 --bogus
expect_reject "--tune desconocido"       1000 --tune foo=1
expect_reject "--tune sparse=2/1"        1000 --tune sparse=2/1
expect_reject "--tune sparse=0"          1000 --tune sparse=0
expect_reject "--zstd-level abc"         1000 --zstd-level abc
expect_reject "--zstd-level 99"          1000 --zstd-level 99

# Reglas de ajuste del segmento, leidas en el log de arranque sobre una cola
# de 1e6 (instantanea): con --l1-bytes/--l2-bytes forzados las decisiones no
# dependen de la maquina. Lo que sysfs decide (cuota real de L2, hilos por
# nucleo) no se comprueba aqui.
expect_log() {
    local label="$1" pattern="$2"; shift 2
    local out
    out=$("$BIN" "$@" 2>&1 >/dev/null)
    if echo "$out" | grep -qE "$pattern"; then
        printf "OK   %-40s %s\n" "$label" "$pattern"
    else
        printf "FAIL %-40s no aparece '%s'\n" "$label" "$pattern"
        echo "$out" | grep -E "^Starting|^  " | sed 's/^/     /'
        fail=1
    fi
}
T13="10000000000000 --start 9999999000000 -t 2"
T15="1000000000000000 --start 999999999000000 -t 2"
# base 4 MiB from an 8 MiB L2, capped to 32 x 4 KiB = 128 KiB (3932160 numbers)
expect_log "cap 32 x L1d (sysfs mentiroso)"  "cap: 32 x L1d"                   $T13 --l1-bytes 4096 --l2-bytes 8388608
expect_log "cap: segmento de 128 KiB"         "segment=3932160,"                $T13 --l1-bytes 4096 --l2-bytes 8388608
# 1 MiB base from a 2 MiB L2, doubled in the sparse regime, back to the L2 (1 MiB) by the ceiling
expect_log "tope: L2 por hilo en regimen sparse" "ceiling: the L2 per thread"   $T15 --l1-bytes 32768 --l2-bytes 2097152
expect_log "tope: segmento de 1 MiB"          "segment=31457280,"               $T15 --l1-bytes 32768 --l2-bytes 2097152
# an explicit 1.5 MiB -s rounded down to the power of 2 the sparse tier needs
expect_log "-s redondeado a potencia de 2"    "a power of 2 for the sparse tier" $T15 -s 47185920
expect_log "-s redondeado: 1 MiB"             "segment=31457280,"               $T15 -s 47185920
expect_log "-s respetado sin tier sparse"     "segment=47185920,"               $T13 -s 47185920
expect_log "--tune sparse en el log"          "sparse cutoff: 1/2 of the segment \(--tune sparse\)" $T15 --tune sparse=1/2
expect_log "--tune medium_nta forzado"        "medium-tier prefetchnta: no \(forced" $T13 --tune medium_nta=0
# med64 = the whole segment on a 256 KiB L2 (tuning.hpp's small-L2 rule), 1/6 on 512 KiB; --tune med64 overrides.
# Few base primes (1e10: 9,592) and one thread per core: the base is half the whole-L2 width,
# on any L2 (128 KiB from 256 KiB; 768 KiB from the 1.5 MiB that 32 x 48 KiB caps a 2 MiB L2 to);
# with more base primes (1e12 tail: 78,498) the whole width, 1.5 MiB.
expect_log "media L2: L2 chica y pocos primos"  "segment: half the whole-L2 width \(one thread per core, 9,592 base primes <= 40,000\)" 10000000000 -t 1 --l1-bytes 32768 --l2-bytes 262144
expect_log "media L2: segmento de 128 KiB"      "segment=3932160," 10000000000 -t 1 --l1-bytes 32768 --l2-bytes 262144
expect_log "media L2: L2 grande y pocos primos" "segment=23592960," 10000000000 -t 1 --l1-bytes 49152 --l2-bytes 2097152
expect_log "L2 entera: L2 grande y muchos primos" "segment: whole L2 per thread" 1000000000000 --start 999999000000 -t 1 --l1-bytes 49152 --l2-bytes 2097152
expect_log "L2 entera: segmento de 1.5 MiB"     "segment=47185920," 1000000000000 --start 999999000000 -t 1 --l1-bytes 49152 --l2-bytes 2097152
expect_log "med64 entero con L2 de 256 KiB"      "med64 cutoff: the whole segment \(L2 of 256 KiB or less\)" $T13 --l1-bytes 32768 --l2-bytes 262144
expect_log "med64 entero: poblacion"             "med64, 0 medium" $T13 --l1-bytes 32768 --l2-bytes 262144
expect_log "med64 1/6 con L2 de 512 KiB"      "^Starting.*[1-9][0-9]* medium" $T13 --l1-bytes 32768 --l2-bytes 524288
# small = 1/2 of the sub-block on a 256 KiB L2 (tuning.hpp's small-L2 rule): 990 primes below 8K
# instead of 526 below 4K with the 16 KiB sub-block of --l1-bytes 32768; --tune small overrides.
expect_log "small 1/2 con L2 de 256 KiB"      "small cutoff: 1/2 of the sub-block \(L2 of 256 KiB or less\)" $T13 --l1-bytes 32768 --l2-bytes 262144
expect_log "small 1/2: poblacion"             "^Starting.* 990 small base primes" $T13 --l1-bytes 32768 --l2-bytes 262144
expect_log "small 1/4 con L2 de 512 KiB"      "^Starting.* 526 small base primes" $T13 --l1-bytes 32768 --l2-bytes 524288
expect_log "--tune small manda sobre la regla" "^Starting.* 526 small base primes" $T13 --l1-bytes 32768 --l2-bytes 262144 --tune small=1/4

# The L3 gate (tuning.hpp): 1/4 from 4 MiB of L3 per active thread, also
# below the sparse regime when an octave of base primes lands in the sparse
# tier. Needs the machine's own L3 (sysfs), so only where it is >= 4 MiB:
# at -t 1 the whole L3 is one thread's.
L3_KB=$(cat /sys/devices/system/cpu/cpu0/cache/index3/size 2>/dev/null | tr -d 'K')
if [ -n "$L3_KB" ] && [ "$(cat /sys/devices/system/cpu/cpu0/cache/index3/level 2>/dev/null)" = "3" ] && [ "$L3_KB" -ge 4096 ]; then
    expect_log "corte 1/4 por L3 por hilo activo (-t 1)" "sparse cutoff: 1/4 of the segment \(L3 per active thread >= 4 MiB\)" 10000000000000 --start 9999999000000 -t 1
    expect_log "sin tier sparse a 1e12 (-t 1, margen de una octava)" "^Starting 1 threads.* 0 sparse" 1000000000000 --start 999999000000 -t 1
else
    echo "SKIP corte 1/4 por L3 por hilo activo (L3 de cpu0 no detectado o < 4 MiB)"
fi

# --- 7: un tramo del regimen sparse real (la ultima 1e8 bajo 1e15, 1.8M
# primos base, el corte automatico de esta maquina) contra primecount, y los
# errores que nth_prime debe detectar ---
check_start 1000000000000000 999999900000000 -t 2

DB7="$WORKDIR/n1e5.db"
"$BIN" 100000 -o "$DB7" >/dev/null 2>&1
count7=$("$NTH_BIN" "$DB7" --count)
expect_nth_reject() {
    local label="$1"; shift
    if "$NTH_BIN" "$@" >/dev/null 2>&1; then
        printf "FAIL nth_prime %-30s deberia fallar\n" "$label"; fail=1
    else
        printf "OK   nth_prime %-30s rechazado\n" "$label"
    fi
}
expect_nth_reject "N=0"                 "$DB7" 0
expect_nth_reject "N > total"           "$DB7" $((count7 + 1))
expect_nth_reject "N no numerico"       "$DB7" abc
cp "$WORKDIR/n1e5.blk" "$WORKDIR/n1e5.blk.orig"
truncate -s -1 "$WORKDIR/n1e5.blk"
expect_nth_reject ".blk truncado"       "$DB7" 1
mv "$WORKDIR/n1e5.blk" "$WORKDIR/n1e5.blk.gone"
expect_nth_reject ".blk ausente"        "$DB7" 1
mv "$WORKDIR/n1e5.blk.orig" "$WORKDIR/n1e5.blk"
if [ "$("$NTH_BIN" "$DB7" "$count7")" == "99991" ]; then
    printf "OK   nth_prime %-30s 99991\n" "ultimo primo < 1e5"
else
    printf "FAIL nth_prime %-30s\n" "ultimo primo < 1e5"; fail=1
fi
if [ "$fail" -eq 0 ]; then
    echo "Todas las pruebas OK."
else
    echo "Alguna prueba fallo." >&2
fi
exit "$fail"
