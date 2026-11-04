#!/bin/bash
# Two-line machine identity printed above the benchmark tables
# (benchmark.sh, benchmark_tails.sh), so a pasted table says where it was
# measured: CPU model, threads, caches, RAM, primesieve version, commit and
# date; those two scripts add the clock measured during the round to NOTE.
# Ratios from different CPUs or primesieve versions aren't comparable, and a
# cloud VM can land on a different CPU from one session to the next.
# Usage: scripts/machine_info.sh THREADS [NOTE]
cd "$(dirname "$0")/.."

cpu=$(lscpu 2>/dev/null | sed -nE 's/^Model name: *//p' | head -1)
caches=$(lscpu 2>/dev/null | sed -nE 's/^(L1d|L2|L3)( cache)?: *(.*)$/\1 \3/p' | paste -sd ';' - | sed 's/;/; /g')
tpc=$(lscpu 2>/dev/null | sed -nE 's/^Thread\(s\) per core: *//p')
ram=$(awk '/^MemTotal:/ {printf "%.1f GiB", $2 / 1048576}' /proc/meminfo 2>/dev/null)
ps_ver=$(primesieve --version 2>/dev/null | head -1 | sed 's/,.*//')
# Inside the dev container there is no .git: the Makefile's docker-* targets
# pass the host's `git describe` in ERATOSTENES_COMMIT instead.
commit=${ERATOSTENES_COMMIT:-$(git -c safe.directory='*' describe --always --dirty 2>/dev/null || echo "?")}

echo "${cpu:-unknown CPU}, $1 threads (${tpc:-?} per core); ${caches:-caches ?}; RAM ${ram:-?}"
echo "${ps_ver:-primesieve ?}; eratostenes $commit; $(date +%F)${2:+; $2}"
