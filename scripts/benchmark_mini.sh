#!/bin/bash
# Short diagnostic for a machine the auto-tuning has never seen (old or odd
# hardware, a VM with a strange topology): what sysfs says about the caches,
# what the CLI chose from it, and a sweep of the knobs that choice drives
# (segment width, sparse cutoff, med64/medium prefetchnta), every run against
# primesieve on the same window. One N, a short window, single runs: a
# 3-minute answer to "is it the auto-tuning or the CPU?", compact enough to
# photograph. It is NOT a table for BENCHMARK.md (benchmark_tails.sh is).
#
# Usage: make benchmark-mini   (or: bash scripts/benchmark_mini.sh)
# Env overrides:
#   N         top of the window, integer or 1eX (default: 1e13)
#   WIDTH     window width, integer or 1eX (default: 1e10; both must fit 2^63)
#   THREADS   thread count for both programs (default: nproc)
#   SEGMENTS  -s values to sweep, in numbers (default: 128 KiB .. 1 MiB)
set -euo pipefail
cd "$(dirname "$0")/.."

BIN=./eratostenes
THREADS="${THREADS:-$(nproc)}"
N_IN="${N:-1e13}"
WIDTH_IN="${WIDTH:-1e10}"
SEGMENTS="${SEGMENTS:-3932160 7864320 15728640 31457280}"

if ! command -v primesieve >/dev/null 2>&1; then
    echo "primesieve no esta en el PATH -- instalalo (apt-get install primesieve)." >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "No existe $BIN -- compila antes (make)." >&2
    exit 1
fi

to_dec() { # "1e13" / "100000" -> plain digits
    local d
    if [[ "$1" =~ ^([0-9]+)[eE]([0-9]+)$ ]]; then
        d=${BASH_REMATCH[1]}$(printf '%*s' "${BASH_REMATCH[2]}" '' | tr ' ' 0)
    elif [[ "$1" =~ ^[0-9]+$ ]]; then
        d=$1
    else
        echo "valor no valido: $1 (usa un entero o 1eX)" >&2
        return 1
    fi
    d=$(echo "$d" | sed 's/^0*//')
    echo "${d:-0}"
}
N=$(to_dec "$N_IN")
WIDTH=$(to_dec "$WIDTH_IN")
if (( N <= WIDTH )); then echo "WIDTH=$WIDTH_IN no cabe por debajo de N=$N_IN" >&2; exit 1; fi
# --start rounds down to a multiple of 240 (64 wheel indices); give
# primesieve the same window so the counts can be compared.
START=$(( (N - WIDTH) / 240 * 240 ))

# ---------------------------------------------------------------- header
echo "== machine"
bash scripts/machine_info.sh "$THREADS" "last ${WIDTH_IN} below ${N_IN}"
free -h 2>/dev/null | sed -nE 's/^(Mem|Swap):/  \1:/p' || true
echo "== sysfs caches, cpu0 (level type size shared_cpu_list)"
for d in /sys/devices/system/cpu/cpu0/cache/index*; do
    [ -r "$d/level" ] || continue
    printf '  %s L%s %-12s %-8s cpus %s\n' "$(basename "$d")" "$(cat "$d/level")" \
        "$(cat "$d/type" 2>/dev/null)" "$(cat "$d/size" 2>/dev/null)" \
        "$(cat "$d/shared_cpu_list" 2>/dev/null)"
done

# ---------------------------------------------------------------- runs
t_p=""; c_p=""
run_ps() { # -> t_p, c_p
    local out
    out=$(primesieve "$START" "$N" --count -t "$THREADS" --time -q 2>&1) || { echo "$out" >&2; exit 1; }
    t_p=$(echo "$out" | sed -nE 's/^Seconds: *([0-9.]+)$/\1/p')
    c_p=$(echo "$out" | grep -oE '^[0-9]+$' | head -1 || true)
}
t_e=""; c_e=""; startup=""
run_e() { # ENVSTRING ARGS... -> t_e, c_e, startup (the config lines of the log)
    local envs=$1; shift
    local out
    out=$(env $envs "$BIN" "$N" --start "$START" -t "$THREADS" "$@" 2>&1) || { echo "$out" >&2; exit 1; }
    t_e=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
    c_e=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
    startup=$(echo "$out" | grep -E '^Starting|^  (segment|sub-block|sparse cutoff|medium-tier prefetchnta)' \
        | sed -E 's/^Starting [0-9]+ threads, limit=[0-9]+, (segment=[0-9]+), wheel mod [0-9]+ \([0-9]+ primes\), ([0-9]+ small[^,]*\(sub-block [^)]*\))?.*/\1 \2/')
}
ratio() { awk -v a="$1" -v b="$2" 'BEGIN{ if (b > 0) printf "%.2fx", a / b; else printf "?" }'; }
check() { [ "$1" = "$c_p" ] && echo "ok" || echo "MISMATCH($1 vs $c_p)"; }

summary=()
add_row() { summary+=("| $1 | ${2}s | ${t_p}s | $(ratio "$2" "$t_p") |"); }

echo "== primesieve, $THREADS threads"
run_ps
echo "  ${t_p}s ($c_p primes)"
if awk -v t="$t_p" 'BEGIN{exit !(t < 2)}'; then
    echo "  (runs this short are noise on this machine: use WIDTH=1e11, e.g. WIDTH=1e11 make benchmark-mini)"
fi

echo "== eratostenes auto"
run_e ""
echo "$startup" | sed 's/^ */    /'
echo "  ${t_e}s $(ratio "$t_e" "$t_p") [$(check "$c_e")]"
add_row "auto" "$t_e"
best_t=$t_e; best_s=""; best_label="auto"

echo "== segment sweep (-s, numbers per segment)"
for s in $SEGMENTS; do
    run_e "" -s "$s"
    kib=$(( s / 30 / 1024 ))
    extra=$(echo "$startup" | grep -E 'segment:' | sed -E 's/^ *segment: /; /' | head -1 || true)
    echo "  -s $s (${kib} KiB): ${t_e}s $(ratio "$t_e" "$t_p") [$(check "$c_e")]${extra}"
    add_row "-s $s (${kib} KiB)" "$t_e"
    if awk -v a="$t_e" -v b="$best_t" 'BEGIN{exit !(a < b)}'; then best_t=$t_e; best_s=$s; best_label="-s $s"; fi
done

seg_args=()
[ -n "$best_s" ] && seg_args=(-s "$best_s")
echo "== cutoff and prefetch, on the best segment so far ($best_label)"
for cfg in "--tune sparse=1/1" "--tune sparse=1/2" "--tune sparse=1/4" "--tune medium_nta=1" "--tune medium_nta=0"; do
    # shellcheck disable=SC2086
    run_e "" "${seg_args[@]}" $cfg
    echo "  $cfg: ${t_e}s $(ratio "$t_e" "$t_p") [$(check "$c_e")]"
    add_row "$best_label $cfg" "$t_e"
done
run_e "ERATOSTENES_MED64_NTA=0" "${seg_args[@]}"
echo "  ERATOSTENES_MED64_NTA=0: ${t_e}s $(ratio "$t_e" "$t_p") [$(check "$c_e")]"
add_row "$best_label med64 NTA off" "$t_e"

echo
echo "last ${WIDTH_IN} below ${N_IN}, $THREADS threads, single runs"
echo
echo "| config | eratostenes | primesieve | ratio |"
echo "|---|---:|---:|---:|"
printf '%s\n' "${summary[@]}"
