#!/bin/bash
# Interleaved A/B of two eratostenes runs on the tail of one or more N (the
# last WIDTH numbers below each): A B A B ... REPS times per N, then B against
# A. A and B are each a binary plus CLI options (see eratostenes --help: -s,
# --tune key=value, --l1-bytes, ...), so this compares two builds, two
# configurations, or both. Where perf works it measures perf stat's cycles:u
# and instructions:u (what docs/RESEARCH.md trusts over wall time on a busy
# machine) next to the sieve's own `total:` wall time; elsewhere wall time
# only. The interleaving cancels a slow drift of the host, the per-run rows
# show a fast one.
#
# Two configurations of the current build:
#   B="--tune sparse=1/2" make benchmark-ab
#   A="--tune sparse=1/1" B="--tune sparse=1/2" REPS=5 make benchmark-ab
# An ERA_* flag build (sparse_tier.hpp) against the default one:
#   make variant DEFS=-DERA_ACT_IDX=0; BIN_B=./eratostenes_variant make benchmark-ab
# Another commit, built in a worktree so src/ stays untouched:
#   git worktree add -f /tmp/era_A <commit> && make -C /tmp/era_A -B eratostenes
#   BIN_A=/tmp/era_A/eratostenes NS="1e15 1e17" make benchmark-ab
#   git worktree remove --force /tmp/era_A    # when done
#
# Env: BIN_A and BIN_B (default ./eratostenes both), A and B (each
#      side's options, default none: the automatic configuration), NS (one or
#      more N, default 1e13; N works too), WIDTH (default 1e10), THREADS
#      (default nproc), REPS (default 3), EVENTS (default
#      cycles:u,instructions:u, at most two), PERF=0 (wall time even where
#      perf works)
set -uo pipefail
cd "$(dirname "$0")/.."

BIN_A="${BIN_A:-./eratostenes}"
BIN_B="${BIN_B:-./eratostenes}"
A_CFG="${A:-}"
B_CFG="${B:-}"
NS="${NS:-${N:-1e13}}"
WIDTH_IN="${WIDTH:-1e10}"
THREADS="${THREADS:-$(nproc)}"
REPS="${REPS:-3}"
EVENTS="${EVENTS:-cycles:u,instructions:u}"

if [ "$BIN_A" = "$BIN_B" ] && [ "$A_CFG" = "$B_CFG" ]; then
    echo "A and B are the same run: give B (options, e.g. B=\"--tune sparse=1/2\") or BIN_A/BIN_B (another binary)" >&2
    exit 1
fi
for b in "$BIN_A" "$BIN_B"; do
    if [ ! -x "$b" ]; then
        make -s "$b" || { echo "$b not found and make can't build it (build it first: make)." >&2; exit 1; }
    fi
done

source scripts/lib.sh # to_dec
WIDTH=$(to_dec "$WIDTH_IN")

# perf usable with these events on this machine: every event counted.
HAVE_PERF=0
if [ "${PERF:-1}" != 0 ] && command -v perf >/dev/null 2>&1 &&
   perf stat -x, -e "$EVENTS" true 2>&1 | awk -F, 'NF > 2 { n++; if ($1 !~ /^[0-9][0-9.]*$/) bad = 1 } END { exit !(n > 0 && !bad) }'; then
    HAVE_PERF=1
fi
E1=${EVENTS%%,*}
E2=""; [ "$E1" != "$EVENTS" ] && E2=${EVENTS#*,}

bash scripts/machine_info.sh "$THREADS" "last ${WIDTH_IN} below each N, A/B x$REPS, $( [ $HAVE_PERF = 1 ] && echo "perf stat $EVENTS + wall" || echo "wall time only" )"
echo "A: $BIN_A ($(md5sum < "$BIN_A" | cut -c1-8)) ${A_CFG:-auto}"
echo "B: $BIN_B ($(md5sum < "$BIN_B" | cut -c1-8)) ${B_CFG:-auto}"
[ $HAVE_PERF = 1 ] && echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo ?)"

# run_one BIN OPTIONS N START: sets m1 m2 (the events, "-" without perf),
# wall, cnt and startup (the configuration lines of the startup log).
run_one() {
    local bin=$1 cfg=$2 n=$3 start=$4 out vals
    if [ $HAVE_PERF = 1 ]; then
        out=$(perf stat -x, -e "$EVENTS" "$bin" "$n" --start "$start" -t "$THREADS" $cfg 2>&1) || { echo "$out" >&2; return 1; }
        vals=$(echo "$out" | awk -F, 'NF > 2 && $1 ~ /^[0-9][0-9.]*$/ { printf "%s ", $1 }')
        read -r m1 m2 _ <<< "$vals"
    else
        out=$("$bin" "$n" --start "$start" -t "$THREADS" $cfg 2>&1) || { echo "$out" >&2; return 1; }
        m1=-; m2=-
    fi
    m2=${m2:--}
    wall=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
    cnt=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
    [ -n "$cnt" ] || { echo "$out" >&2; return 1; }
    startup=$(echo "$out" | grep -E '^Starting|^  (segment|sub-block|sparse cutoff|sparse ring|small cutoff|med64 cutoff|medium-tier prefetchnta)' \
        | sed -E 's/^Starting [0-9]+ threads, limit=[0-9]+, (segment=[0-9]+), wheel mod [0-9]+ \([0-9]+ primes\), ([0-9]+ small[^,]*\(sub-block [^)]*\))?.*/\1 \2/' \
        | sed 's/^ *//' | paste -sd ';' - | sed 's/;/; /g')
}

# compare LABEL "A values" "B values": B's median and mean against A's, how
# many sorted pairs B wins, A's spread, and whether the runs overlap at all.
compare() {
    awk -v label="$1" -v A="$2" -v B="$3" '
        function sort(arr, n,   i, t, j) { for (i = 2; i <= n; i++) { t = arr[i]; j = i - 1; while (j >= 1 && arr[j] > t) { arr[j+1] = arr[j]; j-- } arr[j+1] = t } }
        function median(arr, n) { return n % 2 ? arr[(n+1)/2] : (arr[n/2] + arr[n/2+1]) / 2 }
        BEGIN {
            na = split(A, a, " "); nb = split(B, b, " ")
            for (i = 1; i <= na; i++) sa += a[i]; for (i = 1; i <= nb; i++) sb += b[i]
            sort(a, na); sort(b, nb)
            ma = median(a, na); mb = median(b, nb)
            wins = 0; for (i = 1; i <= na && i <= nb; i++) if (b[i] < a[i]) wins++
            verdict = b[nb] < a[1] ? "every B run below every A run" : b[1] > a[na] ? "every B run above every A run" : "runs overlap"
            printf "  %-13s B vs A %+.2f%% median, %+.2f%% mean (B lower in %d/%d sorted pairs; A spread %.1f%%; %s)\n",
                   label ":", (mb - ma) / ma * 100, (sb / nb - sa / na) / (sa / na) * 100, wins, na, (a[na] - a[1]) / a[1] * 100, verdict
        }'
}

for N_IN in $NS; do
    N=$(to_dec "$N_IN")
    if (( N <= WIDTH )); then echo "WIDTH=$WIDTH_IN doesn't fit below N=$N_IN" >&2; continue; fi
    START=$(( (N - WIDTH) / 240 * 240 ))
    echo
    if [ $HAVE_PERF = 1 ]; then printf "%-6s %-3s %-3s %16s %16s %8s  %s\n" N bin rep "$E1" "${E2:--}" wall count
    else printf "%-6s %-3s %-3s %8s  %s\n" N bin rep wall count; fi
    a1=(); a2=(); aw=(); b1=(); b2=(); bw=(); ca=""; cb=""
    for ((r = 1; r <= REPS; r++)); do
        for side in A B; do
            if [ $side = A ]; then run_one "$BIN_A" "$A_CFG" "$N" "$START" || exit 1
            else run_one "$BIN_B" "$B_CFG" "$N" "$START" || exit 1; fi
            [ $r -eq 1 ] && echo "  $side config: $startup"
            if [ $HAVE_PERF = 1 ]; then printf "%-6s %-3s %-3s %16s %16s %8s  %s\n" "$N_IN" "$side" "$r" "$m1" "$m2" "$wall" "$cnt"
            else printf "%-6s %-3s %-3s %8s  %s\n" "$N_IN" "$side" "$r" "$wall" "$cnt"; fi
            if [ $side = A ]; then a1+=("$m1"); a2+=("$m2"); aw+=("$wall"); ca=$cnt
            else b1+=("$m1"); b2+=("$m2"); bw+=("$wall"); cb=$cnt; fi
        done
    done
    if [ "$ca" != "$cb" ]; then echo "  COUNT MISMATCH at $N_IN: A $ca vs B $cb" >&2; exit 1; fi
    if [ $HAVE_PERF = 1 ]; then
        compare "$E1" "${a1[*]}" "${b1[*]}"
        [ -n "$E2" ] && compare "$E2" "${a2[*]}" "${b2[*]}"
    fi
    compare "wall" "${aw[*]}" "${bw[*]}"
done
