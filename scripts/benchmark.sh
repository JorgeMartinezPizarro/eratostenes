#!/bin/bash
# Two sweeps, meant to be pasted directly into README.md#benchmarks:
#
#   1. CPU: runs both eratostenes and primesieve at N=1e10..1e13 and prints
#      a markdown table with the ratio between them.
#
#      Requires primesieve on PATH (https://github.com/kimwalisch/primesieve
#      -- `apt-get install primesieve` on Debian/Ubuntu/WSL) for the
#      comparison column; the script aborts with a clear message if it's
#      missing rather than silently only benchmarking eratostenes.
#
#      Only mod 30 (this project's shipped default) is swept -- mod 6 and
#      mod 210 were tried against it up to 1e13 (see the i5-11400F section
#      of README#benchmarks): mod 6 avoids mod 30's old L3-cliff ratio jump
#      but isn't actually faster (less wheel-filtering costs about as much
#      as the cache cliff saves), and mod 210's table grows too fast to be
#      worth it past 1e10. If that ever changes, sweeping other wheels
#      means rebuilding between them (see git history for how earlier
#      versions of this script did that) -- not done here since there's
#      currently only one worth tracking.
#
#      -s is deliberately *not* passed by default: the CLI's own auto
#      default (sized from N and the machine's real L2/L3, see
#      src/arg_parser.hpp) is what this project actually recommends running
#      with, so that's what gets benchmarked. Set SEGMENT to force a
#      specific width instead (e.g. to compare against the auto default).
#
#   2. Disk I/O: builds a real .db per N in 1e8..1e12 and shows a table
#      with file size, bits/prime, write throughput and total time that the
#      binary itself reports (previously scripts/test_io.sh, fused here).
#      WRITE_PATH/KEEP_DB below are only used by this sweep. WRITE_PATH
#      must point at the Linux-native filesystem (not a /mnt/c... mount,
#      much slower) -- default: $HOME/eratostenes-io-bench.
#
# Usage: ./scripts/benchmark.sh
# Env overrides: THREADS (default: nproc, used for both eratostenes and
# primesieve -- an apples-to-apples comparison needs the same thread
# count), SEGMENT (default: unset, i.e. the CLI's own auto -s), REPS
# (default: 1, keeps the fastest of REPS runs per N in the CPU sweep --
# there's real run-to-run noise on this kind of box, see BENCHMARK section
# of the README/commit history), WRITE_PATH/KEEP_DB (see above; KEEP_DB
# defaults to 0 -- each .db is deleted right after it's measured, since
# the ~26 GB the I/O sweep accumulates otherwise (mostly the N=1e12 row at
# ~23 GB) isn't something to leave lying around by default. Set KEEP_DB=1
# to keep them for inspection instead. A trap also cleans up the
# in-progress file if the script is interrupted or errors out partway
# (not on a hard SIGKILL, which can't be trapped -- see git history for
# why that matters here).
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v primesieve >/dev/null 2>&1; then
    echo "primesieve no esta en el PATH -- instalalo (apt-get install primesieve)" >&2
    echo "para poder generar la columna de comparacion." >&2
    exit 1
fi

BIN=./eratostenes
THREADS="${THREADS:-$(nproc)}"
SEGMENT="${SEGMENT:-}"
REPS="${REPS:-1}"
WRITE_PATH="${WRITE_PATH:-$HOME/eratostenes-io-bench}"
KEEP_DB="${KEEP_DB:-0}"

# Cleans up the .db currently being written if this script exits early
# (error, Ctrl-C) instead of leaving a partial file behind -- set right
# before each eratostenes -o call below, cleared right after it succeeds.
# Empty when nothing is in flight, so a clean exit is a no-op here.
CURRENT_IO_FILE=""
cleanup_current_io_file() {
    if [ "$KEEP_DB" = "0" ] && [ -n "$CURRENT_IO_FILE" ]; then
        rm -f "$CURRENT_IO_FILE"
    fi
}
trap cleanup_current_io_file EXIT

echo "Reconstruyendo eratostenes..." >&2
make re >/tmp/benchmark_build.log 2>&1 || { cat /tmp/benchmark_build.log >&2; exit 1; }

# ============================ 1: CPU sweep =================================

NS=(1e10 1e11 1e12 1e13)
# pi(N) for each N above, in the same order -- known values, used to catch
# a silently-wrong build/primesieve mismatch instead of just reporting a
# (meaningless) time. See scripts/test.sh for the same values at other N.
EXPECTED=(455052511 4118054813 37607912018 346065536839)

declare -A ERATO_TIME
declare -A PRIMESIEVE_TIME

for i in "${!NS[@]}"; do
    n="${NS[$i]}"
    expected="${EXPECTED[$i]}"

    best_e=""
    best_p=""
    for ((r = 1; r <= REPS; r++)); do
        # --- eratostenes ---
        seg_args=()
        [ -n "$SEGMENT" ] && seg_args=(-s "$SEGMENT")
        out=$("$BIN" "$n" -t "$THREADS" "${seg_args[@]}" --count-only 2>&1)
        t_e=$(echo "$out" | sed -nE 's/.*primos\), .*total: *([0-9.]+)s.*/\1/p')
        # sed above only matches the "Iniciando..." + "total:" combined
        # blob in edge cases; fall back to a plain total: match.
        [ -z "$t_e" ] && t_e=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
        count_e=$(echo "$out" | sed -nE 's/.*Listo\. ([0-9,]+) primos.*/\1/p' | tr -d ',')
        if [ "$count_e" != "$expected" ]; then
            echo "n=$n rep=$r: eratostenes MAL: obtenido $count_e, esperado $expected" >&2
            exit 1
        fi
        if [ -z "$best_e" ] || awk -v a="$t_e" -v b="$best_e" 'BEGIN{exit !(a<b)}'; then
            best_e="$t_e"
        fi

        # --- primesieve ---
        out=$(primesieve "$n" --count -t "$THREADS" --time -q 2>&1)
        t_p=$(echo "$out" | sed -nE 's/^Seconds: *([0-9.]+)$/\1/p')
        count_p=$(echo "$out" | grep -oE '^[0-9]+$' | head -1)
        if [ "$count_p" != "$expected" ]; then
            echo "n=$n rep=$r: primesieve MAL: obtenido $count_p, esperado $expected" >&2
            exit 1
        fi
        if [ -z "$best_p" ] || awk -v a="$t_p" -v b="$best_p" 'BEGIN{exit !(a<b)}'; then
            best_p="$t_p"
        fi

        echo "  n=$n rep=$r eratostenes=${t_e}s primesieve=${t_p}s [ok]" >&2
    done
    ERATO_TIME["$n"]="$best_e"
    PRIMESIEVE_TIME["$n"]="$best_p"
done

echo >&2
echo "| N | eratostenes | primesieve | ratio |"
printf "|---|---:|---:|---:|\n"
for n in "${NS[@]}"; do
    te="${ERATO_TIME[$n]}"
    tp="${PRIMESIEVE_TIME[$n]}"
    ratio=$(awk -v a="$te" -v b="$tp" 'BEGIN{printf "%.1f", a/b}')
    printf "| %s | %ss | %ss | %sx |\n" "$n" "$te" "$tp" "$ratio"
done

# ========================== 2: Disk I/O sweep ===============================

case "$(cd "$(dirname "$WRITE_PATH")" 2>/dev/null && pwd)/$(basename "$WRITE_PATH")" in
    /mnt/*)
        echo "Aviso: WRITE_PATH=$WRITE_PATH esta bajo /mnt (filesystem de Windows montado via 9p/DrvFs)." >&2
        echo "       Eso ralentiza la E/S y falsea los tiempos de escritura medidos aqui." >&2
        echo "       Usa un directorio en el filesystem nativo de Linux, p.ej. \$HOME." >&2
        ;;
esac

mkdir -p "$WRITE_PATH"

# N -> pi(N) conocido, misma escala x10 que el barrido manual (1k..1t).
IO_SIZES=(100m 1b 10b 100b 1t)
IO_LIMITS=(100000000 1000000000 10000000000 100000000000 1000000000000)
IO_EXPECTED=(5761455 50847534 455052511 4118054813 37607912018)

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

declare -A IO_SIZE_BYTES IO_WRITE_S IO_TOTAL_S IO_COUNT

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

    count=$(echo "$out" | sed -nE 's/.*Listo\. ([0-9,]+) primos.*/\1/p' | tr -d ',')
    write_s=$(echo "$out" | sed -nE 's/.*escritura: *([0-9.]+)s.*/\1/p')
    total_s=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')

    if [ "$count" != "$expected" ]; then
        echo "FAIL N=$n: pi(N) esperado=$expected obtenido=${count:-<sin salida>}" >&2
        echo "$out" >&2
        fail=1
        break
    fi
    if [ -z "$write_s" ] || [ -z "$total_s" ]; then
        echo "FAIL N=$n: no se pudo parsear el tiempo de escritura/total de la salida" >&2
        echo "$out" >&2
        fail=1
        break
    fi

    bytes=$(stat -c%s "$file" 2>/dev/null || stat -f%z "$file")

    IO_SIZE_BYTES[$n]="$bytes"
    IO_WRITE_S[$n]="$write_s"
    IO_TOTAL_S[$n]="$total_s"
    IO_COUNT[$n]="$count"

    echo "  pi(N)=$count  tamano=$(human_size "$bytes")  escritura=${write_s}s  total=${total_s}s" >&2

    if [ "$KEEP_DB" = "0" ]; then
        rm -f "$file"
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
printf "| %-6s | %14s | %-10s | %9s | %6s | %8s |\n" \
    "limit" "pi(N)" "db size" "bit/prime" "MB/s" "total(s)"
printf "|--------|---------------:|------------|----------:|-------:|---------:|\n"
for i in "${!IO_SIZES[@]}"; do
	n="${IO_SIZES[$i]}"
    limit="${IO_LIMITS[$i]}"
    bytes="${IO_SIZE_BYTES[$n]}"
    write_s="${IO_WRITE_S[$n]}"
    total_s="${IO_TOTAL_S[$n]}"
    count="${IO_COUNT[$n]}"

    # LIMITS son siempre 1 seguido de ceros (potencias de 10 exactas), asi
    # que el exponente es solo el largo del string menos 1 -- notacion "1Ek"
    # en vez de las comas de mil, que en 1t (13 digitos) desalineaban la
    # columna frente al resto de filas.
    limit_exp="1E$(( ${#limit} - 1 ))"

    bitpp=$(awk -v b="$bytes" -v c="$count" 'BEGIN{printf "%.2f", b*8/c}')
    mbps=$(awk -v b="$bytes" -v s="$write_s" 'BEGIN{printf "%.1f", (s>0)?(b/1e6/s):0}')

    printf "|%-6s | %14s | %-10s | %9s | %6s | %8s |\n" \
        "$limit_exp" "$(commas "$count")" \
        "$(human_size "$bytes")" "$bitpp" "$mbps" "$total_s"
done
