#!/bin/bash
# med64: is it bound by L1<->L2 traffic? Run from the repo root.
# perf stat (totals) + perf record (med64's share of each event), last W below N.
# Env: N (default 1e12), W (window, 1e11), T (threads, nproc).
set -euo pipefail
N=${N:-1000000000000}; W=${W:-100000000000}; T=${T:-$(nproc)}
make -s eratostenes
D=$(mktemp -d "${TMPDIR:-/tmp}/m64.XXXXXX"); cp eratostenes "$D/era"; cd "$D"
avail=" $(perf list --raw-dump 2>/dev/null | tr '\n' ' ') "
pick() { for e in "$@"; do [[ "$avail" == *" $e "* ]] && { echo "$e"; return; }; done; echo -; }
roles="cyc ins brm st rep wb pend pcyc fbf l2s sl1 sb"
names=(cycles instructions branch-misses
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
    map+="$r=$n:u "; ev+="${ev:+,}$n:u"
done
S=$((N - W))
perf stat -x, -e "$ev" -o stat.csv ./era "$N" --start "$S" -t "$T" >/dev/null 2>log.txt
grep -E "^Starting|^  " log.txt || true
perf record -q -F 499 -e "$ev" -o rec.data ./era "$N" --start "$S" -t "$T" >/dev/null 2>&1
perf report -i rec.data --no-children --sort sym --stdio 2>/dev/null > rep.txt
awk -v map="$map" -v T="$T" '
BEGIN { n = split(map, a, " "); for (i = 1; i <= n; i++) { split(a[i], kv, "="); role[kv[2]] = kv[1] } }
FILENAME == "stat.csv" { split($0, f, ","); if (f[3] in role && f[1] ~ /^[0-9.]+$/) tot[role[f[3]]] = f[1]; next }
/^# Samples:/ { e = $NF; gsub(/\047/, "", e); cur = (e in role) ? role[e] : ""; next }
/^ +[0-9.]+%/ && cur != "" {
    t = $0 ~ /med64/ ? "med64" : $0 ~ /cross_off_medium/ ? "medium" : $0 ~ /cross_off</ ? "small" : \
        $0 ~ /Presieve/ ? "presieve" : $0 ~ /process_big|SparseTier/ ? "sparse" : "other"
    sh[cur, t] += $1 / 100
}
END {
    printf "cycles share: med64 %.1f%%, medium %.1f%%, small %.1f%%, presieve %.1f%%, sparse %.1f%%\n",
        100*sh["cyc","med64"], 100*sh["cyc","medium"], 100*sh["cyc","small"], 100*sh["cyc","presieve"], 100*sh["cyc","sparse"]
    for (r in tot) m[r] = tot[r] * sh[r, "med64"]
    c = m["cyc"]; h = m["st"]
    printf "med64: %.2fG cycles, %.2fG stores (~hits), %.2f cycles/hit, IPC %.2f\n", c/1e9, h/1e9, c/h, m["ins"]/c
    if ("rep" in m) printf "  L1 fills/hit %.2f", m["rep"]/h
    if ("wb" in m) printf ", L1 writebacks/hit %.2f", m["wb"]/h
    if ("rep" in m) printf ", L1<->L2 bytes/cycle %.1f%s\n", 64*(m["rep"] + (("wb" in m) ? m["wb"] : m["rep"]))/c, ("wb" in m) ? "" : " (writebacks taken = fills)"
    if ("pend" in m && "pcyc" in m) printf "  miss pending %.0f%% of cycles, %.2f misses in flight\n", 100*m["pcyc"]/c, m["pend"]/m["pcyc"]
    if ("fbf" in m) printf "  fill buffers full %.0f%% of cycles\n", 100*m["fbf"]/c
    if ("l2s" in m) printf "  waiting on L2 resources %.0f%% of cycles\n", 100*m["l2s"]/c
    if ("sl1" in m) printf "  stalled on an L1 miss %.0f%% of cycles\n", 100*m["sl1"]/c
    if ("sb" in m) printf "  store buffer full %.0f%% of cycles\n", 100*m["sb"]/c
    printf "  branch misses/hit %.3f\n", m["brm"]/h
}' stat.csv rep.txt
cd / && rm -rf "$D"
