#!/bin/bash
# `eratostenes --print` against `primesieve --print`: every prime up to each N
# as text, timed with bash's `time` (wall, and CPU = user + sys) to three
# targets:
#   null  /dev/null: the programs alone, no I/O
#   file  a file under DIR: the text goes into the page cache
#   sync  the same file flushed to the disk (`sync` inside the timing)
# eratostenes is built here (make) and copied, like the output, into a fresh
# directory under DIR, so both programs run from and write to the Linux
# filesystem (a /mnt/... Windows mount is refused). Every file run starts
# from a synced page cache and no file, so no run waits on the previous one's
# writeback. The two programs' files must be identical, or the script stops.
#
# Usage: bash scripts/benchmark_print.sh   (or: make benchmark-print)
# Env overrides:
#   NS       N list, integers or 1eX (default: "1e8 1e9 1e10")
#   TARGETS  any of null, file, sync (default: "null file sync")
#   THREADS  thread count for both programs (default: nproc)
#   REPS     runs per program, N and target, as interleaved pairs alternating
#            which one goes first; the table shows the means (default: 3)
#   DIR      where the work directory goes (default: /tmp; at 1e10 the
#            check holds both programs' files, ~9.5 GB). Where /tmp is tmpfs
#            (RAM) the sync target is dropped; DIR=$HOME measures a disk.
set -euo pipefail
cd "$(dirname "$0")/.."

NS="${NS:-1e8 1e9 1e10}"
TARGETS="${TARGETS:-null file sync}"
THREADS="${THREADS:-$(nproc)}"
REPS="${REPS:-3}"
DIR="${DIR:-/tmp}"

if ! command -v primesieve >/dev/null 2>&1; then
    echo "primesieve is not on the PATH -- install it (docs/ISSUES.md: the same version on every machine)." >&2
    exit 1
fi
case "$(cd "$DIR" && pwd -P)" in
    /mnt/*) echo "DIR=$DIR is a Windows mount: use a directory on the Linux filesystem (default /tmp)." >&2; exit 1 ;;
esac

echo "Building eratostenes..." >&2
make -s eratostenes >/dev/null 2>"$DIR/benchmark_print_build.log" || { cat "$DIR/benchmark_print_build.log" >&2; exit 1; }
rm -f "$DIR/benchmark_print_build.log"
WORK=$(mktemp -d "$DIR/eratostenes-print.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cp ./eratostenes "$WORK/eratostenes"
BIN="$WORK/eratostenes"
OUT="$WORK/primes.txt"
ERR="$WORK/stderr.txt"

source scripts/lib.sh # to_dec, mean_of

# Room for the largest N's check (both files at once): pi(N) ~ N / ln N
# lines of up to digits(N) + 1 bytes each, twice.
if [ "$TARGETS" != null ]; then
    need=0
    for n in $NS; do
        b=$(awk -v n="$(to_dec "$n")" 'BEGIN { printf "%.0f", 2 * n / log(n) * (length(n) + 1) }')
        (( b > need )) && need=$b
    done
    avail=$(( $(df -Pk "$WORK" | awk 'NR == 2 { print $4 }') * 1024 ))
    if (( avail < need )); then
        echo "$DIR has $((avail / 1000000000)) GB free, the largest N needs ~$((need / 1000000000)) GB: use a smaller NS or another DIR." >&2
        exit 1
    fi
fi
FS=$(df -PT "$WORK" | awk 'NR == 2 { print $2 }')
# tmpfs is RAM: nothing for sync to flush, the row would repeat `file`.
if [ "$FS" = tmpfs ] && [[ " $TARGETS " == *" sync "* ]]; then
    echo "$DIR is tmpfs (RAM): no 'sync' target (DIR=\$HOME for a disk)." >&2
    TARGETS=$(echo " $TARGETS " | sed 's/ sync / /' | xargs)
fi

TIMEFORMAT='%R %U %S'
# run PROGRAM N TARGET -> sets wall, cpu
run() {
    local prog=$1 n=$2 target=$3 dest=/dev/null t
    local cmd=("$BIN" "$n" --print -t "$THREADS")
    [ "$prog" = ps ] && cmd=(primesieve "$n" --print -t "$THREADS")
    if [ "$target" != null ]; then
        dest=$OUT
        rm -f "$OUT"
        sync
    fi
    if [ "$target" = sync ]; then
        t=$( { time { "${cmd[@]}" > "$dest" 2>"$ERR"; sync; }; } 2>&1 ) || { cat "$ERR" >&2; exit 1; }
    else
        t=$( { time "${cmd[@]}" > "$dest" 2>"$ERR"; } 2>&1 ) || { cat "$ERR" >&2; exit 1; }
    fi
    local user sys
    read -r wall user sys <<< "$t"
    cpu=$(awk -v u="$user" -v s="$sys" 'BEGIN { printf "%.3f", u + s }')
}

declare -A WALL_E CPU_E WALL_P CPU_P SIZE
for n in $NS; do
    for target in $TARGETS; do
        we=() ce=() wp=() cp=()
        for ((r = 1; r <= REPS; r++)); do
            # The first file pair of each N is kept (as $OUT.era, $OUT.ps) and compared.
            check=0
            [ "$target" != null ] && [ -z "${SIZE[$n]:-}" ] && check=1
            if (( r % 2 )); then order="era ps"; else order="ps era"; fi
            for prog in $order; do
                run "$prog" "$n" "$target"
                if [ "$prog" = era ]; then we+=("$wall"); ce+=("$cpu"); else wp+=("$wall"); cp+=("$cpu"); fi
                [ "$check" = 1 ] && mv "$OUT" "$OUT.$prog"
            done
            if [ "$check" = 1 ]; then
                if ! cmp -s "$OUT.era" "$OUT.ps"; then
                    echo "N=$n: eratostenes and primesieve printed different primes" >&2
                    exit 1
                fi
                SIZE[$n]=$(stat -c%s "$OUT.ps")
                rm -f "$OUT.era" "$OUT.ps"
            fi
            echo "  N=$n $target rep=$r eratostenes=${we[-1]}s primesieve=${wp[-1]}s" >&2
        done
        WALL_E[$n,$target]=$(mean_of 3 "${we[@]}"); CPU_E[$n,$target]=$(mean_of 2 "${ce[@]}")
        WALL_P[$n,$target]=$(mean_of 3 "${wp[@]}"); CPU_P[$n,$target]=$(mean_of 2 "${cp[@]}")
    done
done

echo
bash scripts/machine_info.sh "$THREADS" "--print, mean of $REPS, pairs interleaved; files in $DIR ($FS)"
echo
echo "| N | text | target | eratostenes | primesieve | ratio |"
echo "|---|---:|---|---:|---:|---:|"
for n in $NS; do
    size=${SIZE[$n]:-}
    [ -n "$size" ] && size=$(awk -v b="$size" 'BEGIN { if (b >= 1e9) printf "%.2f GB", b / 1e9; else printf "%.0f MB", b / 1e6 }')
    for target in $TARGETS; do
        case $target in null) label="/dev/null" ;; file) label="file" ;; sync) label="file + sync" ;; esac
        e=${WALL_E[$n,$target]}; p=${WALL_P[$n,$target]}
        ratio=$(awk -v a="$e" -v b="$p" 'BEGIN { printf "%.2f", a / b }')
        printf "| %s | %s | %s | %ss (CPU %ss) | %ss (CPU %ss) | %sx |\n" \
            "$n" "${size:--}" "$label" "$e" "${CPU_E[$n,$target]}" "$p" "${CPU_P[$n,$target]}" "$ratio"
    done
done
