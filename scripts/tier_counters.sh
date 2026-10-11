#!/bin/bash
# Cache and store counters of the small and med64 tiers: perf stat totals
# times each tier's share of every event in perf record, on the last W
# numbers below N. Two runs: one thread per core, then both SMT siblings of
# every core (skipped without SMT); on a hybrid CPU pinned to the P-cores
# and counted on their PMU. Run from the repo root.
# Env: N (default 1e12), W (window, default 1e11).
set -uo pipefail
fail() { echo "tier_counters: $*" >&2; exit 1; }
N=${N:-1000000000000}; W=${W:-100000000000}
command -v perf >/dev/null || fail "perf is not on the PATH"
command -v taskset >/dev/null || fail "taskset is not on the PATH"
make -s eratostenes || fail "make failed"
D=$(mktemp -d "${TMPDIR:-/tmp}/tiers.XXXXXX") || fail "mktemp failed"
trap 'rm -rf "$D"' EXIT
cp eratostenes "$D/era" && cd "$D" || fail "can't set up $D"

# The CPUs: the P-cores on a hybrid CPU (its cpu_core PMU), all of them
# elsewhere; one per core = the first SMT sibling of each.
expand() { local IFS=, part; for part in $1; do
    if [[ $part == *-* ]]; then seq "${part%-*}" "${part#*-}"; else echo "$part"; fi; done; }
PMU=""; cpus=$(cat /sys/devices/system/cpu/online)
if [ -r /sys/bus/event_source/devices/cpu_core/cpus ]; then
    PMU=cpu_core; cpus=$(cat /sys/bus/event_source/devices/cpu_core/cpus)
fi
all=(); first=()
for c in $(expand "$cpus"); do
    all+=("$c")
    sib=$(cat "/sys/devices/system/cpu/cpu$c/topology/thread_siblings_list" 2>/dev/null || echo "$c")
    [ "$(expand "$sib" | head -1)" = "$c" ] && first+=("$c")
done

ev_name() { if [ -n "$PMU" ]; then echo "$PMU/$1/u"; else echo "$1:u"; fi; }
works() { local out; out=$(perf stat -x, -e "$1" true 2>&1 >/dev/null) && [[ "$out" != *"not supported"* ]]; }
pick() { local e; for e in "$@"; do works "$(ev_name "$e")" && { ev_name "$e"; return; }; done; echo -; }
works "$(ev_name cycles)" || { perf stat -e "$(ev_name cycles)" true >/dev/null; fail "perf can't count $(ev_name cycles) (above)"; }
roles="cyc ins brm st rep wb pend pcyc fbf l2s sl1 sb"
names=("$(ev_name cycles)" "$(ev_name instructions)" "$(ev_name branch-misses)"
    "$(pick mem_inst_retired.all_stores mem_uops_retired.all_stores)"
    "$(pick l1d.replacement)"
    "$(pick l2_trans.l1d_wb l2_l1d_wb_rqsts.all)"
    "$(pick l1d_pend_miss.pending)" "$(pick l1d_pend_miss.pending_cycles)"
    "$(pick l1d_pend_miss.fb_full)" "$(pick l1d_pend_miss.l2_stall)"
    "$(pick cycle_activity.stalls_l1d_miss cycle_activity.stalls_l1d_pending)"
    "$(pick resource_stalls.sb)")
map=""; ev=""; i=0
for r in $roles; do
    n=${names[$i]}; i=$((i + 1))
    [ "$n" = - ] && continue
    map+="$r=$n "; ev+="${ev:+,}$n"
done
echo "events: $ev"
S=$((N - W))

# run LABEL CPU... : both perf passes pinned to those CPUs, one thread each.
run() {
    local label=$1; shift
    local list; list=$(IFS=,; echo "$*")
    local t=$#
    echo "== $label: $t threads on CPUs $list"
    taskset -c "$list" perf stat -x, -e "$ev" -o stat.csv ./era "$N" --start "$S" -t "$t" >/dev/null 2>log.txt ||
        { cat log.txt >&2; fail "perf stat failed (above)"; }
    grep -E "^Starting" log.txt | cut -c1-140
    taskset -c "$list" perf record -q -F 499 -e "$ev" -o rec.data ./era "$N" --start "$S" -t "$t" >/dev/null 2>log.txt ||
        { cat log.txt >&2; fail "perf record failed (above)"; }
    perf report -i rec.data --no-children --sort sym --stdio 2>log.txt > rep.txt ||
        { cat log.txt >&2; fail "perf report failed (above)"; }
    awk -v map="$map" '
    BEGIN { n = split(map, a, " "); for (i = 1; i <= n; i++) { k = index(a[i], "="); role[substr(a[i], k + 1)] = substr(a[i], 1, k - 1) } }
    FILENAME == "stat.csv" { split($0, f, ","); if (f[3] in role && f[1] ~ /^[0-9.]+$/) tot[role[f[3]]] = f[1]; next }
    /^# Samples:/ { e = $NF; gsub(/\047/, "", e); cur = (e in role) ? role[e] : ""; next }
    /^ +[0-9.]+%/ && cur != "" {
        t = $0 ~ /med64/ ? "med64" : $0 ~ /cross_off_medium/ ? "medium" : $0 ~ /cross_off</ ? "small" : \
            $0 ~ /Presieve/ ? "presieve" : $0 ~ /process_big|SparseTier/ ? "sparse" : "other"
        sh[cur, t] += $1 / 100
    }
    END {
        printf "  cycles: med64 %.1f%%, small %.1f%%, medium %.1f%%, presieve %.1f%%, sparse %.1f%%\n",
            100*sh["cyc","med64"], 100*sh["cyc","small"], 100*sh["cyc","medium"], 100*sh["cyc","presieve"], 100*sh["cyc","sparse"]
        split("small med64", tiers, " ")
        for (k = 1; k <= 2; k++) {
            t = tiers[k]
            for (r in tot) m[r] = tot[r] * sh[r, t]
            c = m["cyc"]; h = m["st"]
            if (c <= 0 || h <= 0) { printf "  %s: no samples\n", t; continue }
            printf "  %-5s %.2f cycles/hit, %.2f hits/cycle, IPC %.2f, branch misses/hit %.3f\n", t, c/h, h/c, m["ins"]/c, m["brm"]/h
            printf "        L1 fills/hit %.2f", m["rep"]/h
            if ("wb" in m) printf ", writebacks/hit %.2f", m["wb"]/h
            printf ", L1<->L2 bytes/cycle %.1f%s\n", 64*(m["rep"] + (("wb" in m) ? m["wb"] : m["rep"]))/c, ("wb" in m) ? "" : " (writebacks taken = fills)"
            printf "       "
            if ("pend" in m && "pcyc" in m && m["pcyc"] > 0) printf " miss pending %.0f%%, %.2f in flight;", 100*m["pcyc"]/c, m["pend"]/m["pcyc"]
            if ("fbf" in m) printf " fill buffers full %.0f%%;", 100*m["fbf"]/c
            if ("l2s" in m) printf " L2 resources %.0f%%;", 100*m["l2s"]/c
            if ("sl1" in m) printf " stalled on L1 miss %.0f%%;", 100*m["sl1"]/c
            if ("sb" in m) printf " store buffer full %.0f%%", 100*m["sb"]/c
            printf "\n"
        }
    }' stat.csv rep.txt
}

run "one thread per core" "${first[@]}"
if [ ${#all[@]} -gt ${#first[@]} ]; then run "two threads per core" "${all[@]}"; fi
