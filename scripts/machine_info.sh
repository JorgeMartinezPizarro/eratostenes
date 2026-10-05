#!/bin/bash
# Two-line machine identity printed above the benchmark tables
# (benchmark.sh, benchmark_tails.sh), so a pasted table says where it was
# measured: CPU model, threads, caches, primesieve version, commit and date.
# Ratios from different CPUs or primesieve versions aren't comparable, and a
# cloud VM can land on a different CPU from one session to the next.
# Usage: scripts/machine_info.sh THREADS [NOTE]
cd "$(dirname "$0")/.."

cpu=$(lscpu 2>/dev/null | sed -nE 's/^Model name: *//p' | head -1)
caches=$(lscpu 2>/dev/null | sed -nE 's/^(L1d|L2|L3)( cache)?: *(.*)$/\1 \3/p' | paste -sd ';' - | sed 's/;/; /g')
tpc=$(lscpu 2>/dev/null | sed -nE 's/^Thread\(s\) per core: *//p')
ps_ver=$(primesieve --version 2>/dev/null | head -1 | sed 's/,.*//')
# Inside the dev container there is no .git: the Makefile's docker-* targets
# pass the host's `git describe` in ERATOSTENES_COMMIT instead.
commit=${ERATOSTENES_COMMIT:-$(git -c safe.directory='*' describe --always --dirty 2>/dev/null || echo "?")}

echo "${cpu:-CPU desconocida}, $1 threads (${tpc:-?} per core); ${caches:-caches ?}"
echo "${ps_ver:-primesieve ?}; eratostenes $commit; $(date +%F)${2:+; $2}"
