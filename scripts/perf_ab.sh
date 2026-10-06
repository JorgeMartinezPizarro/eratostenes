#!/bin/bash
# Interleaved A/B of two eratostenes BINARIES on the tails of several N, with
# perf stat's cycles:u and instructions:u when the machine has a PMU and perf
# (the measure docs/RESEARCH.md trusts over wall time), the sieve's own
# `total:` wall time otherwise. A B A B ... REPS times per N; per-run rows,
# then B's median against A's and how many sorted pairs B wins.
#
# Build A from any commit in a worktree, so src/ stays untouched:
#   git worktree add -f /tmp/era_A <commit> && make -C /tmp/era_A -B eratostenes
#   BIN_A=/tmp/era_A/eratostenes bash scripts/perf_ab.sh
#   git worktree remove --force /tmp/era_A    # when done
# Or an ERA_* flag build: make variant DEFS=-DERA_BIG_PF=8; BIN_A=./eratostenes_variant ...
#
# Env: BIN_A (required), BIN_B (default ./eratostenes), NS (default
#      "1e14 1e15 1e16 1e17"), WIDTH (default 1e10), THREADS (default nproc),
#      REPS (default 3), EVENTS (default cycles:u,instructions:u)
set -uo pipefail
cd "$(dirname "$0")/.."

BIN_A="${BIN_A:?BIN_A: the binary to compare against (see the header)}"
BIN_B="${BIN_B:-./eratostenes}"
NS="${NS:-1e14 1e15 1e16 1e17}"
WIDTH_IN="${WIDTH:-1e10}"
THREADS="${THREADS:-$(nproc)}"
REPS="${REPS:-3}"
EVENTS="${EVENTS:-cycles:u,instructions:u}"
for b in "$BIN_A" "$BIN_B"; do [ -x "$b" ] || { echo "no existe o no es ejecutable: $b" >&2; exit 1; }; done

source scripts/lib.sh # to_dec
WIDTH=$(to_dec "$WIDTH_IN")

# perf usable with these events on this machine?
HAVE_PERF=0
if command -v perf >/dev/null 2>&1 && perf stat -x, -e "$EVENTS" true 2>&1 | grep -qvE '<not supported>|<not counted>|Error|not found'; then
    perf stat -x, -e "$EVENTS" true 2>&1 | grep -qE '^[0-9]' && HAVE_PERF=1
fi

bash scripts/machine_info.sh "$THREADS" "last ${WIDTH_IN} below each N, A/B x$REPS, $( [ $HAVE_PERF = 1 ] && echo "perf stat $EVENTS" || echo "wall time only (no perf)" )"
echo "A: $BIN_A ($(md5sum "$BIN_A" | cut -c1-8))"
echo "B: $BIN_B ($(md5sum "$BIN_B" | cut -c1-8))"
echo "paranoid: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo ?)"
echo

# run BIN N START -> prints "metric1 metric2 wall count" (metrics: cycles instructions, or wall wall)
run_one() {
    local bin=$1 n=$2 start=$3 out m1 m2 wall cnt
    if [ $HAVE_PERF = 1 ]; then
        out=$(perf stat -x, -e "$EVENTS" "$bin" "$n" --start "$start" -t "$THREADS" 2>&1)
        m1=$(echo "$out" | awk -F, '/cycles/ {print $1; exit}')
        m2=$(echo "$out" | awk -F, '/instructions/ {print $1; exit}')
    else
        out=$("$bin" "$n" --start "$start" -t "$THREADS" 2>&1)
    fi
    wall=$(echo "$out" | sed -nE 's/.*total: *([0-9.]+)s.*/\1/p')
    cnt=$(echo "$out" | sed -nE 's/.*Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
    [ -n "$cnt" ] || { echo "$out" >&2; return 1; }
    [ $HAVE_PERF = 1 ] || { m1=$wall; m2=$wall; }
    echo "$m1 $m2 $wall $cnt"
}

# median and B-vs-A of two space-separated lists
compare() { # LABEL listA listB
    awk -v label="$1" -v A="$2" -v B="$3" '
        function median(arr, n,   i, t, j) { for (i = 2; i <= n; i++) { t = arr[i]; j = i - 1; while (j >= 1 && arr[j] > t) { arr[j+1] = arr[j]; j-- } arr[j+1] = t }
                                             return n % 2 ? arr[(n+1)/2] : (arr[n/2] + arr[n/2+1]) / 2 }
        BEGIN {
            na = split(A, a, " "); nb = split(B, b, " ")
            ma = median(a, na); mb = median(b, nb)   # sorts in place
            wins = 0; for (i = 1; i <= na && i <= nb; i++) if (b[i] < a[i]) wins++
            spread = (a[na] - a[1]) / a[1] * 100
            printf "  %-13s B vs A %+.2f%% (B lower in %d/%d sorted pairs; A spread %.1f%%)\n", label ":", (mb - ma) / ma * 100, wins, na, spread
        }'
}

if [ $HAVE_PERF = 1 ]; then printf "%-6s %-3s %-3s %16s %16s %8s  %s\n" N bin rep cycles:u instr:u wall count
else printf "%-6s %-3s %-3s %8s  %s\n" N bin rep wall count; fi
for N_IN in $NS; do
    N=$(to_dec "$N_IN")
    if (( N <= WIDTH )); then echo "WIDTH=$WIDTH_IN no cabe por debajo de N=$N_IN" >&2; continue; fi
    START=$(( (N - WIDTH) / 240 * 240 ))
    a1=(); b1=(); a2=(); b2=(); ca=""; cb=""
    for ((r = 1; r <= REPS; r++)); do
        for which in A B; do
            if [ $which = A ]; then bin=$BIN_A; else bin=$BIN_B; fi
            read -r m1 m2 wall cnt < <(run_one "$bin" "$N" "$START") || exit 1
            if [ $HAVE_PERF = 1 ]; then printf "%-6s %-3s %-3s %16s %16s %8s  %s\n" "$N_IN" "$which" "$r" "$m1" "$m2" "$wall" "$cnt"
            else printf "%-6s %-3s %-3s %8s  %s\n" "$N_IN" "$which" "$r" "$wall" "$cnt"; fi
            if [ $which = A ]; then a1+=("$m1"); a2+=("$m2"); ca=$cnt; else b1+=("$m1"); b2+=("$m2"); cb=$cnt; fi
        done
    done
    if [ "$ca" != "$cb" ]; then echo "  COUNT MISMATCH at $N_IN: A $ca vs B $cb" >&2; exit 1; fi
    if [ $HAVE_PERF = 1 ]; then
        compare "cycles:u" "${a1[*]}" "${b1[*]}"
        compare "instructions" "${a2[*]}" "${b2[*]}"
    else
        compare "wall" "${a1[*]}" "${b1[*]}"
    fi
    echo
done
