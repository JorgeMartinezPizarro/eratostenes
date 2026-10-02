#!/bin/bash
# Disk I/O sweep: builds a real .db per N in 1e8..1e12 and prints a table
# (meant for README.md#database) with file size, bits/prime, effective
# throughput and total time that the binary itself reports. .db output is a
# single sieve+encode+write pass (no separate count pre-pass, see git
# history), so "MB/s" here is throughput over that whole pass, not an
# isolated write phase -- there isn't a separate one to isolate any more.
# Split out of scripts/benchmark.sh (the count-only comparison against
# primesieve), so either runs on its own.
#
# Usage: ./scripts/benchmark_io.sh   (or: make benchmark-io)
# Env overrides: THREADS (default: nproc), SEGMENT (default: 4194304),
# WRITE_PATH (default: $HOME/eratostenes-io-bench; must be on the
# Linux-native filesystem, not a /mnt/c... mount, much slower), KEEP_DB
# (default: 0 -- each .db is deleted right after it's measured, since the
# several GB this sweep accumulates (mostly the N=1e12 row) isn't something
# to leave lying around by default; KEEP_DB=1 keeps them for inspection). A
# trap also cleans up the in-progress file if the script is interrupted or
# errors out partway (not on a hard SIGKILL, which can't be trapped -- see
# git history for why that matters here).
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
THREADS="${THREADS:-$(nproc)}"
SEGMENT="${SEGMENT:-}"
WRITE_PATH="${WRITE_PATH:-$HOME/eratostenes-io-bench}"
KEEP_DB="${KEEP_DB:-0}"

# Cleans up the .db currently being written if this script exits early
# (error, Ctrl-C) instead of leaving a partial file behind -- set right
# before each eratostenes -o call below, cleared right after it succeeds.
# Empty when nothing is in flight, so a clean exit is a no-op here.
CURRENT_IO_FILE=""
cleanup_current_io_file() {
    if [ "$KEEP_DB" = "0" ] && [ -n "$CURRENT_IO_FILE" ]; then
        rm -f "$CURRENT_IO_FILE" "${CURRENT_IO_FILE%.db}.blk"
    fi
}
trap cleanup_current_io_file EXIT

echo "Reconstruyendo eratostenes..." >&2
make re >/tmp/benchmark_build.log 2>&1 || { cat /tmp/benchmark_build.log >&2; exit 1; }

case "$(cd "$(dirname "$WRITE_PATH")" 2>/dev/null && pwd)/$(basename "$WRITE_PATH")" in
    /mnt/*)
        echo "Aviso: WRITE_PATH=$WRITE_PATH esta bajo /mnt (filesystem de Windows montado via 9p/DrvFs)." >&2
        echo "       Eso ralentiza la E/S y falsea los tiempos de escritura medidos aqui." >&2
        echo "       Usa un directorio en el filesystem nativo de Linux, p.ej. \$HOME." >&2
        ;;
esac

mkdir -p "$WRITE_PATH"

# N -> pi(N) conocido, misma escala x10 que el barrido manual (1k..1t).
IO_SIZES=(100m 1b 10b 100b 1t 10t)
IO_LIMITS=(100000000 1000000000 10000000000 100000000000 1000000000000 10000000000000)
IO_EXPECTED=(5761455 50847534 455052511 4118054813 37607912018 346065536839)

# bytes -> "X.XX UUU" (KiB/MiB/GiB/TiB), sin depender de numfmt.
human_size() {
    awk -v b="$1" 'BEGIN {
        split("B KiB MiB GiB TiB", units, " ");
        u = 1; v = b;
        while (v >= 1024 && u < 5) { v /= 1024; u++ }
        printf "%.2f %s", v, units[u]
    }'
}


declare -A IO_SIZE_BYTES IO_TOTAL_S IO_COUNT

fail=0
for i in "${!IO_SIZES[@]}"; do
    n="${IO_SIZES[$i]}"
    limit="${IO_LIMITS[$i]}"
    expected="${IO_EXPECTED[$i]}"
    file="$WRITE_PATH/primes-$n.db"

    echo "== N=$n (limite $limit) ==" >&2
    CURRENT_IO_FILE="$file"
    out=$("$BIN" "$n" -t "$THREADS" -s "${SEGMENT:-4194304}" -o "$file" 2>&1) || {
        echo "$out" >&2
        echo "eratostenes fallo para N=$n" >&2
        exit 1
    }

    count=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
    total_s=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')

    if [ "$count" != "$expected" ]; then
        echo "FAIL N=$n: pi(N) esperado=$expected obtenido=${count:-<sin salida>}" >&2
        echo "$out" >&2
        fail=1
        break
    fi
    if [ -z "$total_s" ]; then
        echo "FAIL N=$n: no se pudo parsear el tiempo total de la salida" >&2
        echo "$out" >&2
        fail=1
        break
    fi

    # .db (index) plus its .blk sidecar (the blocks): the table reports both together.
    bytes=$(( $(stat -c%s "$file") + $(stat -c%s "${file%.db}.blk") ))

    IO_SIZE_BYTES[$n]="$bytes"
    IO_TOTAL_S[$n]="$total_s"
    IO_COUNT[$n]="$count"

    echo "  pi(N)=$count  tamano=$(human_size "$bytes")  total=${total_s}s" >&2

    if [ "$KEEP_DB" = "0" ]; then
        rm -f "$file" "${file%.db}.blk"
    fi
    CURRENT_IO_FILE=""
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
bash scripts/machine_info.sh "$THREADS" "segment ${SEGMENT:-4194304}"
echo
printf "| %-6s | %-10s | %9s | %6s | %8s |\n" \
    "limit" "db size" "bit/prime" "MB/s" "total(s)"
printf "|--------|------------|----------:|-------:|---------:|\n"
for i in "${!IO_SIZES[@]}"; do
	n="${IO_SIZES[$i]}"
    limit="${IO_LIMITS[$i]}"
    bytes="${IO_SIZE_BYTES[$n]}"
    total_s="${IO_TOTAL_S[$n]}"
    count="${IO_COUNT[$n]}"

    # LIMITS son siempre 1 seguido de ceros (potencias de 10 exactas), asi
    # que el exponente es solo el largo del string menos 1 -- notacion "1Ek"
    # en vez de las comas de mil, que en 1t (13 digitos) desalineaban la
    # columna frente al resto de filas.
    limit_exp="1E$(( ${#limit} - 1 ))"

    bitpp=$(awk -v b="$bytes" -v c="$count" 'BEGIN{printf "%.2f", b*8/c}')
    mbps=$(awk -v b="$bytes" -v s="$total_s" 'BEGIN{printf "%.1f", (s>0)?(b/1e6/s):0}')

    printf "|%-6s | %-10s | %9s | %6s | %8s |\n" \
        "$limit_exp" \
        "$(human_size "$bytes")" "$bitpp" "$mbps" "$total_s"
done