#!/bin/bash
# Every compile-time experiment (the ERA_* knobs in segment_sieve.hpp /
# erat_small.hpp) against the default binary, on one window, in one go: for
# each entry of FLAGS, build ./eratostenes_variant with those defines (make
# variant) and run scripts/benchmark_ab.sh with it as B. Prints each A/B's
# verdict and a summary table. The knobs are all measured and off by default
# (docs/RESEARCH.md); this is how a new machine re-asks the question.
#
# Usage: make benchmark-flags   (or: bash scripts/benchmark_flags.sh)
# Env: FLAGS (space-separated list of -D... entries, one variant each; a
#      variant needing two defines joins them with a comma, e.g.
#      "-DERA_BIG_PF=0,-DERA_BLK_BYTES=8192"), N (default 1e15: the sparse
#      knobs need the sparse tier), WIDTH (default 1e10), THREADS (default
#      nproc), REPS (default 2)
set -euo pipefail
cd "$(dirname "$0")/.."

FLAGS="${FLAGS:--DERA_BIG_PF=0 -DERA_BLK_BYTES=1024 -DERA_BLK_BYTES=8192}"
export N="${N:-1e15}"
export WIDTH="${WIDTH:-1e10}"
export THREADS="${THREADS:-$(nproc)}"
export REPS="${REPS:-2}"

[ -x ./eratostenes ] || make -s
bash scripts/machine_info.sh "$THREADS" "last ${WIDTH} below ${N}, A/B x$REPS per flag"

summary=()
for f in $FLAGS; do
    defs="${f//,/ }"
    echo "== $defs"
    make -s variant DEFS="$defs" >/dev/null 2>&1 || { echo "  build failed"; summary+=("| $defs | build failed | |"); continue; }
    out=$(BIN_B=./eratostenes_variant bash scripts/benchmark_ab.sh 2>&1)
    echo "$out" | grep -E "^  [AB] rep|^B vs A|every|overlap" | sed 's/^/  /'
    delta=$(echo "$out" | sed -nE 's/^B vs A: (.*)$/\1/p')
    verdict=$(echo "$out" | grep -E "^every|^runs overlap" | head -1)
    summary+=("| $defs | $delta | $verdict |")
done

echo
echo "last ${WIDTH} below ${N}, $THREADS threads, each variant vs the default binary, A/B x$REPS"
echo
echo "| flags | B vs A | verdict |"
echo "|---|---:|---|"
printf '%s\n' "${summary[@]}"
