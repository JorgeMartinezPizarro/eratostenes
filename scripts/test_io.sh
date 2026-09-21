#!/bin/bash
# Sweep de E/S real: construye un .db por cada N en 1k..1t (misma escala x10
# que el barrido manual original) y mide, para cada uno, el tamano final del
# fichero y los tiempos que el propio binario reporta (conteo/escritura/
# total -- ver el bloque "Pass 2" de main.cpp para is_db_output). No hay
# truco de restar tiempos aqui: "escritura" es el tiempo real de la segunda
# pasada (sieve + gap-encode + zstd + INSERT en SQLite), reportado por el
# binario mismo.
#
# Pensado para `make test-io`, pero tambien se puede correr suelto
# (./scripts/test_io.sh) siempre que el binario ya este compilado.
#
# Usage: ./scripts/test_io.sh
# Env overrides:
#   THREADS     (default: nproc)
#   SEGMENT     (default: 4194304, ancho numerico de -s)
#   WRITE_PATH  (default: $HOME/eratostenes-io-bench) directorio donde
#               quedan los .db. Tiene que estar en el filesystem nativo de
#               Linux (ext4 de WSL, no un /mnt/c... montado por 9p) -- ese
#               mount es mucho mas lento y falsearia los tiempos de
#               escritura que esta prueba mide.
#   KEEP_DB     (default: 1, conserva los .db tras medir; KEEP_DB=0 los
#               borra al terminar cada tamano -- util si el disco no tiene
#               espacio para los ~26 GB acumulados de 1k..1t, sobre todo el
#               de 1t (~23 GB) que domina el total)
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
if [ ! -x "$BIN" ]; then
    echo "Error: no se encuentra $BIN compilado. Ejecuta 'make' primero." >&2
    exit 1
fi

THREADS="${THREADS:-$(nproc)}"
SEGMENT="${SEGMENT:-4194304}"
WRITE_PATH="${WRITE_PATH:-$HOME/eratostenes-io-bench}"
KEEP_DB="${KEEP_DB:-1}"

case "$(cd "$(dirname "$WRITE_PATH")" 2>/dev/null && pwd)/$(basename "$WRITE_PATH")" in
    /mnt/*)
        echo "Aviso: WRITE_PATH=$WRITE_PATH esta bajo /mnt (filesystem de Windows montado via 9p/DrvFs)." >&2
        echo "       Eso ralentiza la E/S y falsea los tiempos de escritura medidos aqui." >&2
        echo "       Usa un directorio en el filesystem nativo de Linux, p.ej. \$HOME." >&2
        ;;
esac

mkdir -p "$WRITE_PATH"

# N -> pi(N) conocido, misma escala x10 que el barrido manual (1k..1t).
#SIZES=(1k 10k 100k 1m 10m 100m 1b 10b 100b)
#LIMITS=(1000 10000 100000 1000000 10000000 100000000 1000000000 10000000000 100000000000)
#EXPECTED=(168 1229 9592 78498 664579 5761455 50847534 455052511 4118054813)

SIZES=(1k 10k 100k 1m 10m 100m 1b 10b 100b 1t)
LIMITS=(1000 10000 100000 1000000 10000000 100000000 1000000000 10000000000 100000000000 1000000000000)
EXPECTED=(168 1229 9592 78498 664579 5761455 50847534 455052511 4118054813 37607912018)

# bytes -> "X.XX UUU" (KiB/MiB/GiB/TiB), sin depender de numfmt.
human_size() {
    awk -v b="$1" 'BEGIN {
        split("B KiB MiB GiB TiB", units, " ");
        u = 1; v = b;
        while (v >= 1024 && u < 5) { v /= 1024; u++ }
        printf "%.2f %s", v, units[u]
    }'
}

# N -> "N con comas de mil", sin depender del locale (printf "%'d" solo
# agrupa si el locale activo lo soporta, que no esta garantizado en toda
# maquina/distro).
commas() {
    printf '%d' "$1" | sed -E ':a; s/([0-9])([0-9]{3})(,|$)/\1,\2\3/; ta'
}

declare -A SIZE_BYTES COUNT_S WRITE_S TOTAL_S COUNT

fail=0
for i in "${!SIZES[@]}"; do
    n="${SIZES[$i]}"
    limit="${LIMITS[$i]}"
    expected="${EXPECTED[$i]}"
    file="$WRITE_PATH/primes-$n.db"

    echo "== N=$n (limite $limit) ==" >&2
    out=$("$BIN" "$n" -t "$THREADS" -s "$SEGMENT" -o "$file" 2>&1) || {
        echo "$out" >&2
        echo "eratostenes fallo para N=$n" >&2
        exit 1
    }

    count=$(echo "$out" | sed -nE 's/.*Listo\. ([0-9,]+) primos.*/\1/p' | tr -d ',')
    count_s=$(echo "$out" | sed -nE 's/.*conteo: *([0-9.]+)s.*/\1/p')
    write_s=$(echo "$out" | sed -nE 's/.*escritura: *([0-9.]+)s.*/\1/p')
    total_s=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')

    if [ "$count" != "$expected" ]; then
        echo "FAIL N=$n: pi(N) esperado=$expected obtenido=${count:-<sin salida>}" >&2
        echo "$out" >&2
        fail=1
        break
    fi
    if [ -z "$count_s" ] || [ -z "$write_s" ] || [ -z "$total_s" ]; then
        echo "FAIL N=$n: no se pudo parsear el tiempo de conteo/escritura/total de la salida" >&2
        echo "$out" >&2
        fail=1
        break
    fi

    bytes=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file")

    SIZE_BYTES[$n]="$bytes"
    COUNT_S[$n]="$count_s"
    WRITE_S[$n]="$write_s"
    TOTAL_S[$n]="$total_s"
    COUNT[$n]="$count"

    echo "  pi(N)=$count  tamano=$(human_size "$bytes")  conteo=${count_s}s  escritura=${write_s}s  total=${total_s}s" >&2

    if [ "$KEEP_DB" = "0" ]; then
        rm -f "$file"
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "Barrido de E/S abortado." >&2
    exit 1
fi

# Anchos ajustados al contenido real (N=1k..1t, ver arriba) en vez de
# columnas sobredimensionadas -- con esas el ancho total de fila pasaba de
# 120 columnas y se envolvia feo en una terminal normal. Cabecera, separador
# y filas usan exactamente los mismos anchos por columna para que la tabla
# quede alineada.
echo
printf "| %-6s | %14s | %-10s | %9s | %9s | %7s | %6s | %8s |\n" \
    "limite" "pi(N)" "tam .db" "bit/primo" "conteo(s)" "escr(s)" "MB/s" "total(s)"
printf "|--------|---------------:|------------|----------:|----------:|--------:|-------:|---------:|\n"
for i in "${!SIZES[@]}"; do
	n="${SIZES[$i]}"
    limit="${LIMITS[$i]}"
    bytes="${SIZE_BYTES[$n]}"
    count_s="${COUNT_S[$n]}"
    write_s="${WRITE_S[$n]}"
    total_s="${TOTAL_S[$n]}"
    count="${COUNT[$n]}"

    # LIMITS son siempre 1 seguido de ceros (potencias de 10 exactas), asi
    # que el exponente es solo el largo del string menos 1 -- notacion "1Ek"
    # en vez de las comas de mil, que en 1t (13 digitos) desalineaban la
    # columna frente al resto de filas.
    limit_exp="1E$(( ${#limit} - 1 ))"

    bitpp=$(awk -v b="$bytes" -v c="$count" 'BEGIN{printf "%.2f", b*8/c}')
    mbps=$(awk -v b="$bytes" -v s="$write_s" 'BEGIN{printf "%.1f", (s>0)?(b/1e6/s):0}')

    printf "|%-6s | %14s | %-10s | %9s | %9s | %7s | %6s | %8s |\n" \
        "$limit_exp" "$(commas "$count")" \
        "$(human_size "$bytes")" "$bitpp" "$count_s" "$write_s" "$mbps" "$total_s"
done
