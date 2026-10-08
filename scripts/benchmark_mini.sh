#!/bin/bash
# Short diagnostic for a machine the auto-tuning has never seen (old or odd
# hardware, a VM with a strange topology): what sysfs says about the caches,
# what the CLI chose from it, and a sweep of the knobs that choice drives
# (segment width, tier cutoffs, medium-tier prefetchnta). Every
# configuration is paired with its own primesieve run on the same window,
# so a machine whose state drifts during the sweep (another load, a host
# change, a thermal step) shows up as a drift in primesieve's times instead
# of as a fake loss; the auto configuration is repeated at the end as the
# control. One N, a short window, single runs: a few minutes' answer to "is
# it the auto-tuning or the CPU?", compact enough to photograph. It is NOT
# a table for BENCHMARK.md (benchmark_tails.sh is).
#
# Usage: make benchmark-mini   (or: bash scripts/benchmark_mini.sh)
# Env overrides:
#   N         top of the window, integer or 1eX (default: 1e13)
#   WIDTH     window width, integer or 1eX (default: 1e10; both must fit
#             2^63). Pick it so a run lasts a few seconds: 1e10 on an old
#             2-4 thread machine, 1e11 on a modern desktop or server.
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
    echo "primesieve no esta en el PATH -- instalalo (docs/ISSUES.md: la misma version en todas las maquinas)." >&2
    exit 1
fi
if [ ! -x "$BIN" ]; then
    echo "No existe $BIN -- compila antes (make)." >&2
    exit 1
fi

source scripts/lib.sh # to_dec, num_lt
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
    startup=$(echo "$out" | grep -E '^Starting|^  (segment|sub-block|sparse cutoff|sparse ring|small cutoff|med64 cutoff|medium-tier prefetchnta)' \
        | sed -E 's/^Starting [0-9]+ threads, limit=[0-9]+, (segment=[0-9]+), wheel mod [0-9]+ \([0-9]+ primes\), ([0-9]+ small[^,]*\(sub-block [^)]*\))?.*/\1 \2/')
}
ratio() { awk -v a="$1" -v b="$2" 'BEGIN{ if (b > 0) printf "%.2fx", a / b; else printf "?" }'; }

summary=()
r=""
# pair LABEL ENVSTRING ARGS...: one eratostenes run, then one primesieve run,
# the ratio between the two -> r, plus a line on stdout and a summary row.
pair() {
    local label=$1 envs=$2; shift 2
    run_e "$envs" "$@"
    run_ps
    local ok="ok"; [ "$c_e" = "$c_p" ] || ok="MISMATCH($c_e vs $c_p)"
    r=$(ratio "$t_e" "$t_p")
    echo "  $label: ${t_e}s vs ${t_p}s $r [$ok]"
    summary+=("| $label | ${t_e}s | ${t_p}s | $r |")
}

echo "== eratostenes auto, then primesieve ($THREADS threads each)"
pair "auto" ""
echo "$startup" | sed 's/^ */    /'
if num_lt "$t_p" 2; then
    echo "  (runs this short are noise on this machine: use a wider window, e.g. WIDTH=1e11 make benchmark-mini)"
fi
best_r=$r; best_s=""; best_label="auto"

echo "== segment sweep (-s, numbers per segment), each paired with primesieve"
for s in $SEGMENTS; do
    kib=$(( s / 30 / 1024 ))
    pair "-s $s (${kib} KiB)" "" -s "$s"
    extra=$(echo "$startup" | grep -E 'segment:' | sed -E 's/^ *segment: /    -> /' | head -1 || true)
    [ -n "$extra" ] && echo "$extra"
    if num_lt "${r%x}" "${best_r%x}"; then best_r=$r; best_s=$s; best_label="-s $s"; fi
done

seg_args=()
[ -n "$best_s" ] && seg_args=(-s "$best_s")
echo "== cutoff and prefetch on the best segment so far ($best_label), each paired with primesieve"
for cfg in "--tune sparse=1/1" "--tune sparse=1/2" "--tune sparse=1/4" "--tune med64=1/12" "--tune med64=1/4" "--tune small=1/2" "--tune medium_nta=1" "--tune medium_nta=0"; do
    # shellcheck disable=SC2086
    pair "$best_label $cfg" "" "${seg_args[@]}" $cfg
done

echo "== control: auto again (drift check against the first pair)"
pair "auto (again)" ""

echo
echo "last ${WIDTH_IN} below ${N_IN}, $THREADS threads, single runs, each config paired with its own primesieve run"
echo
echo "| config | eratostenes | primesieve | ratio |"
echo "|---|---:|---:|---:|"
printf '%s\n' "${summary[@]}"
