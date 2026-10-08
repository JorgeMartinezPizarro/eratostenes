#!/bin/bash
# Regression test, in eight parts:
#   1. pi(N) against primecount (--nth-prime/plain, independent of this
#      project -- see https://github.com/kimwalisch/primecount) for
#      N = 1e8..1e11, without -o (count mode, no disk I/O).
#   2. A real .db at N=1e10: a handful of anchor positions plus a few
#      hundred random ones, all against primecount --nth-prime (see the
#      README, section ".db output").
#   3. Several combinations of -t/-s/--l1-bytes/--l2-bytes/--db-block-size/
#      --zstd-level/--tune at once (not one by one) at a small N (1e7),
#      each without -o (count mode) and as .db: none should change the
#      result, only how it is computed or packed.
#   4. Text vs .db round trip at N=1e5..1e7: the same pi(N) and, position by
#      position (all of them at the smallest N, a random sample at the
#      others -- every position through nth_prime is a process per query,
#      expensive from hundreds of thousands of primes up), the same prime
#      in both formats.
#   5. Count mode at N=1e10 against primecount on the paths that only show
#      up at a large N or a small -s (forced sparse tier; no med64; lowered
#      cutoffs, combined), and --start over several ranges (the count is
#      pi(N) - pi(N0 - 1)), one of them above 2^53 (exact parsing of N).
#   6. Small limits (N < 2, N < 7) in all three modes, arguments that must
#      be rejected, and the segment sizing rules read from the startup log
#      with forced --l1-bytes/--l2-bytes (deterministic).
#   7. A range in the real sparse regime (the last 1e8 below 1e15) against
#      primecount, and the errors nth_prime must catch (a truncated or
#      missing .blk, positions out of range).
#   8. Ranges with -o (--start): .txt and .db of several tails of [0, 2e6]
#      against the tail of the full .txt, nth_prime's queries (position,
#      --next, --count X Y, --range, --slice, --info) at block and range
#      edges, their rejections, and a tail of 1e15 against primecount.
# The expected values in 1, 2, 3, 5 and 8 come from primecount, not from
# hardcoded constants -- it must be installed (Debian/Ubuntu: package
# primecount-bin; see docker/Dockerfile, stage "dev").
# Meant for `make test`, but it also runs on its own (./scripts/test.sh)
# once the binaries are built.
set -u
cd "$(dirname "$0")/.."

BIN=./eratostenes
NTH_BIN=./nth_prime
PRIMECOUNT=${PRIMECOUNT:-primecount}
THREADS=${THREADS:-$(nproc 2>/dev/null || echo 4)}

if [ ! -x "$BIN" ] || [ ! -x "$NTH_BIN" ]; then
    echo "Error: $BIN or $NTH_BIN not built. Run 'make' first." >&2
    exit 1
fi
if ! command -v "$PRIMECOUNT" >/dev/null 2>&1; then
    echo "Error: '$PRIMECOUNT' not found (package primecount-bin) -- this test's" >&2
    echo "       expected values come from it, not from hardcoded constants." >&2
    exit 1
fi

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT

fail=0

random_positions() {
    local count="$1" n="$2"
    shuf -i "1-$n" -n "$count" 2>/dev/null || \
        awk -v n="$n" -v s="$count" 'BEGIN { srand(); for (i = 0; i < s; i++) print int(rand() * n) + 1 }'
}

# Runs eratostenes N without -o (count mode, with extra args, e.g. -t/-s) and
# compares against primecount. Used by parts 1 and 3.
check_count_only() {
    local n="$1" expected="$2" label="$3"; shift 3
    local output actual
    output=$("$BIN" "$n" "$@" 2>&1)
    actual=$(echo "$output" | sed -nE 's/Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
    if [ "$actual" == "$expected" ]; then
        printf "OK   %-40s pi(N)=%s\n" "$label" "$actual"
    else
        printf "FAIL %-40s expected=%s got=%s\n" "$label" "$expected" "${actual:-<no output>}"
        echo "--- full output ---"
        echo "$output"
        echo "-------------------"
        fail=1
    fi
}

# Checks `sample` random positions (plus any in `anchors`) of a .db against
# primecount --nth-prime, and the total --count. One summary line (not one
# per position -- that would be hundreds), details only when something
# fails. Used by parts 2 and 3.
check_db_positions() {
    local db="$1" expected_count="$2" sample="$3" label="$4"; shift 4
    local anchors=("$@")

    local actual_count
    actual_count=$("$NTH_BIN" "$db" --count 2>&1)
    if [ "$actual_count" != "$expected_count" ]; then
        printf "FAIL %-30s --count expected=%s got=%s\n" "$label" "$expected_count" "${actual_count:-<no output>}"
        fail=1
        return
    fi

    local positions=("${anchors[@]}")
    while IFS= read -r pos; do positions+=("$pos"); done < <(random_positions "$sample" "$expected_count")

    local checked=0 mismatch=0
    for pos in "${positions[@]}"; do
        local expected actual
        expected=$("$PRIMECOUNT" "$pos" --nth-prime)
        actual=$("$NTH_BIN" "$db" "$pos" 2>&1)
        checked=$((checked + 1))
        if [ "$expected" != "$actual" ]; then
            echo "  position $pos: primecount=$expected nth_prime=$actual"
            mismatch=1
        fi
    done

    if [ "$mismatch" -eq 0 ]; then
        printf "OK   %-30s --count=%s, %s positions (vs primecount)\n" "$label" "$actual_count" "$checked"
    else
        printf "FAIL %-30s --count=%s, %s positions checked\n" "$label" "$actual_count" "$checked"
        fail=1
    fi
}

# --- 1: pi(N) without -o (count mode), against primecount ---
NS=(100000000 1000000000 10000000000 100000000000)

for n in "${NS[@]}"; do
    expected=$("$PRIMECOUNT" "$n")
    start=$(date +%s.%N)
    check_count_only "$n" "$expected" "N=$n" -t "$THREADS"
    end=$(date +%s.%N)
    awk -v a="$start" -v b="$end" -v n="$n" 'BEGIN { printf "       (%.2fs)\n", b - a }'
done

# --- 2: .db at N=1e10, anchor positions + 300 random ones against primecount ---
DB="$WORKDIR/n1e10.db"
"$BIN" 10000000000 -t "$THREADS" -o "$DB" >/dev/null 2>&1
expected_pi_1e10=$("$PRIMECOUNT" 10000000000)
check_db_positions "$DB" "$expected_pi_1e10" 300 ".db N=1e10" 1 1000 10000 200000000 "$expected_pi_1e10"
rm -f "$DB"

# --- 3: parameter combinations at a small N (1e7) -- not a larger N, but a
# good variety of -t/-s/--l1-bytes/--l2-bytes/--db-block-size/--zstd-level
# combinations, ALL AT ONCE (not one by one), that must still give the right
# result both without -o (count mode) and as .db. One line per combination.
N3=10000000
expected_pi_1e7=$("$PRIMECOUNT" "$N3")

# Runs one combination of flags without -o (count mode) and as .db (with 10
# random positions through nth_prime), both against primecount; one result
# line per combination.
check_combo() {
    local label="$1"; shift
    local out actual db actual_count ok=1

    out=$("$BIN" "$N3" "$@" 2>&1)
    actual=$(echo "$out" | sed -nE 's/Done\. ([0-9,]+) primes.*/\1/p' | tr -d ',')
    [ "$actual" == "$expected_pi_1e7" ] || ok=0

    db="$WORKDIR/combo.db"
    "$BIN" "$N3" "$@" -o "$db" >/dev/null 2>&1
    actual_count=$("$NTH_BIN" "$db" --count 2>&1)
    [ "$actual_count" == "$expected_pi_1e7" ] || ok=0

    if [ "$ok" -eq 1 ]; then
        while IFS= read -r pos; do
            local expected got
            expected=$("$PRIMECOUNT" "$pos" --nth-prime)
            got=$("$NTH_BIN" "$db" "$pos" 2>&1)
            [ "$expected" == "$got" ] || ok=0
        done < <(random_positions 10 "$expected_pi_1e7")
    fi
    rm -f "$db"

    if [ "$ok" -eq 1 ]; then
        printf "OK   %s\n" "$label"
    else
        printf "FAIL %s (count-only=%s .db-count=%s)\n" "$label" "$actual" "$actual_count"
        fail=1
    fi
}

check_combo "combo default"                    -t "$THREADS"
check_combo "combo 1t/small cache/small block" -t 1 -s 2000 --l1-bytes 16384 --l2-bytes 131072 --db-block-size 200 --zstd-level 19
check_combo "combo 2t/large segment/large block" -t 2 -s 5000000 --db-block-size 500000 --zstd-level 1
check_combo "combo large forced cache"         -t "$THREADS" --l1-bytes 1048576 --l2-bytes 8388608 --db-block-size 65536 --zstd-level 9
check_combo "combo smallest segment"           -t "$THREADS" -s 64 --db-block-size 100 --zstd-level 1
check_combo "combo sparse"                     -t 3 -s 2000 --db-block-size 1000
check_combo "combo lowered cutoffs"            -t "$THREADS" -s 100000 --tune small=1/2 --tune med64=1/8 --tune sparse=1/2
check_combo "combo no med64/sparse"            -t 2 -s 2000 --tune med64=0 --zstd-level 3
check_combo "combo forced medium NTA"          -t "$THREADS" -s 100000 --tune medium_nta=1
check_combo "combo largest block (2^22)"       -t 2 --db-block-size 4194304
check_combo "combo one-prime block"            -t 2 -s 100000 --db-block-size 1

# --- 4: text vs .db round trip, position by position ---
NS2=(100000 1000000 10000000)
SAMPLES=(0 300 300)

for i in "${!NS2[@]}"; do
    n="${NS2[$i]}"
    sample="${SAMPLES[$i]}"
    txt="$WORKDIR/rt$n.txt"
    db="$WORKDIR/rt$n.db"

    "$BIN" "$n" -t "$THREADS" -o "$txt" >/dev/null 2>&1
    "$BIN" "$n" -t "$THREADS" -o "$db"  >/dev/null 2>&1

    mapfile -t lines < "$txt"
    expected_count=${#lines[@]}
    actual_count=$("$NTH_BIN" "$db" --count)

    if [ "$actual_count" != "$expected_count" ]; then
        printf "FAIL round-trip N=%-12s total: txt=%s db=%s\n" "$n" "$expected_count" "$actual_count"
        fail=1
        continue
    fi

    if [ "$sample" -eq 0 ]; then
        positions=$(seq 1 "$expected_count")
    else
        positions=$(random_positions "$sample" "$expected_count")
    fi

    mismatch=0
    checked=0
    for pos in $positions; do
        expected="${lines[$((pos - 1))]}"
        actual=$("$NTH_BIN" "$db" "$pos")
        checked=$((checked + 1))
        if [ "$expected" != "$actual" ]; then
            echo "  position $pos: txt=$expected db=$actual"
            mismatch=1
        fi
    done

    if [ "$mismatch" -eq 0 ]; then
        printf "OK   round-trip N=%-12s pi(N)=%-10s positions checked=%s\n" "$n" "$expected_count" "$checked"
    else
        printf "FAIL round-trip N=%-12s pi(N)=%-10s positions checked=%s\n" "$n" "$expected_count" "$checked"
        fail=1
    fi
done


# --- 5: large-N paths and --start, count mode, against primecount ---
N5=10000000000
expected_pi_1e10=$("$PRIMECOUNT" "$N5")
check_count_only "$N5" "$expected_pi_1e10" "forced sparse"                 -t "$THREADS" -s 100000
check_count_only "$N5" "$expected_pi_1e10" "no med64, forced sparse"       -t 5 -s 500000 --tune med64=0
check_count_only "$N5" "$expected_pi_1e10" "small/med64/sparse cutoffs"    -t "$THREADS" --tune small=1/8 --tune med64=1/6 --tune sparse=1/4
check_count_only "$N5" "$expected_pi_1e10" "forced medium NTA on"          -t "$THREADS" --tune medium_nta=1
check_count_only "$N5" "$expected_pi_1e10" "forced medium NTA off"         -t "$THREADS" -s 100000 --tune medium_nta=0
check_count_only "$N5" "$expected_pi_1e10" "sparse ring: huge pages on"    -t "$THREADS" -s 100000 --tune huge=1
check_count_only "$N5" "$expected_pi_1e10" "sparse ring: huge pages off"   -t 2 -s 100000 --tune huge=0

# --start N0: the count is the range [N0, N]'s, pi(N) - pi(N0 - 1).
check_start() {
    local n="$1" n0="$2"; shift 2
    local expected=$(( $("$PRIMECOUNT" "$n") - $("$PRIMECOUNT" $(( n0 - 1 ))) ))
    check_count_only "$n" "$expected" "--start $n0 (N=$n) $*" --start "$n0" "$@"
}
check_start "$N5" 9000000000 -t "$THREADS"
check_start "$N5" 9999000001 -t 3 -s 100000
check_start 1000000 2 -t "$THREADS"
check_start 100000000 7 -t 2 -s 20000
# Above 2^53 (~9.007e15): 9007199254740997 is prime and = 1 mod 4, so reading
# it through a double rounded it to ...996 and the range lost that prime.
check_start 9007199254740997 9007199254739761 -t "$THREADS"
# A start not aligned to 64 wheel indices (19999900000000000 = 10 mod 30):
# split_ranges rounds it down, and the primes of that head must not count.
check_start 20000000000000000 19999900000000000 -t 2
check_start 20000000000000000 19999900000000000 -t 1
# A starting prime whose wheel index is 63 mod 64 (239 -> 63, 2399 -> 639)
# must be in the first chunk; 240 with --start 239 is not an empty range.
check_start 1000000 239 -t 2
check_start 1000000 2399 -t 1
check_start 240 239
check_start 241 239
# The same case with a large N0 = 240j - 1 (prime), and with N = N0 + 1.
check_start 1000000099599 999999999599 -t "$THREADS"
check_start 240000000 239999999 -t 2


# --- 6: small limits, rejections and sizing rules (all instant) ---

# N < 2 and N < 7 (main's short paths) and a small regular N, as a count,
# .txt and .db; the .db's last prime against the .txt's last line.
check_tiny() {
    local n="$1" expected="$2" out c lines dbc
    out=$("$BIN" "$n" 2>&1)
    c=$(echo "$out" | sed -nE 's/.*Done\. ([0-9]+) prime.*/\1/p')
    [ "$c" == "$expected" ] || { printf "FAIL tiny N=%-4s count=%s expected=%s\n" "$n" "$c" "$expected"; fail=1; return; }
    "$BIN" "$n" -o "$WORKDIR/tiny.txt" >/dev/null 2>&1
    lines=$(grep -c . "$WORKDIR/tiny.txt" || true)
    [ "$lines" == "$expected" ] || { printf "FAIL tiny N=%-4s txt=%s expected=%s\n" "$n" "$lines" "$expected"; fail=1; return; }
    "$BIN" "$n" -o "$WORKDIR/tiny.db" >/dev/null 2>&1
    dbc=$("$NTH_BIN" "$WORKDIR/tiny.db" --count 2>&1)
    [ "$dbc" == "$expected" ] || { printf "FAIL tiny N=%-4s db=%s expected=%s\n" "$n" "$dbc" "$expected"; fail=1; return; }
    if [ "$expected" -gt 0 ]; then
        local last want
        last=$("$NTH_BIN" "$WORKDIR/tiny.db" "$expected" 2>&1)
        want=$(tail -1 "$WORKDIR/tiny.txt")
        [ "$last" == "$want" ] || { printf "FAIL tiny N=%-4s db[%s]=%s txt=%s\n" "$n" "$expected" "$last" "$want"; fail=1; return; }
    fi
    printf "OK   tiny N=%-4s pi(N)=%-3s (count, txt, db)\n" "$n" "$expected"
}
check_tiny 0 0
check_tiny 1 0
check_tiny 2 1
check_tiny 6 3
check_tiny 7 4
check_tiny 30 10
check_tiny 100 25

# Arguments that must be rejected before anything is sieved.
expect_reject() {
    local label="$1"; shift
    if "$BIN" "$@" >/dev/null 2>&1; then
        printf "FAIL %-40s should be rejected\n" "$label"; fail=1
    else
        printf "OK   %-40s rejected\n" "$label"
    fi
}
expect_reject "--start = N"              1000000 --start 1000000
expect_reject "--start > N"              1000000 --start 2000000
expect_reject "N > MAX_LIMIT"            18446744073709551615
expect_reject "N = 1e20"                 1e20
expect_reject "N = 2.5 (not whole)"      2.5
expect_reject "no N"                     -t 2
expect_reject "second positional N"      1000 2000
expect_reject "-t -1"                    1000 -t -1
expect_reject "-s without a value"       1000 -s
expect_reject "unknown option"           1000 --bogus
expect_reject "unknown --tune key"       1000 --tune foo=1
expect_reject "--tune sparse=2/1"        1000 --tune sparse=2/1
expect_reject "--tune sparse=0"          1000 --tune sparse=0
expect_reject "--zstd-level abc"         1000 --zstd-level abc
expect_reject "--zstd-level 99"          1000 --zstd-level 99

# Segment sizing rules, read from the startup log of a short tail (instant):
# with --l1-bytes/--l2-bytes forced the decisions don't depend on the
# machine. What sysfs decides (the real L2 share, threads per core) is not
# checked here.
expect_log() {
    local label="$1" pattern="$2"; shift 2
    local out
    out=$("$BIN" "$@" 2>&1 >/dev/null)
    if echo "$out" | grep -qE -- "$pattern"; then
        printf "OK   %-40s %s\n" "$label" "$pattern"
    else
        printf "FAIL %-40s missing '%s'\n" "$label" "$pattern"
        echo "$out" | grep -E "^Starting|^  " | sed 's/^/     /'
        fail=1
    fi
}
# An N or a --start past 64 bits names the largest N, not the 64-bit bound.
MAXN="at most 18446744004990074879"
expect_log "N = 1e20: names the largest N"         "N too large: 1e20 \($MAXN"        1e20
expect_log "N = 2^64: names the largest N"         "N too large: 18446744073709551616 \($MAXN" 18446744073709551616
expect_log "N = largest + 1: names the largest N"  "N too large: 18446744004990074880 \($MAXN" 18446744004990074880
expect_log "--start 1e20: names the largest N"     "start too large: 1e20 .*$MAXN"  1e15 --start 1e20
# --db-block-size outside [1, 2^22]: rejected while reading the arguments.
expect_log "--db-block-size 0"                      "db-block-size out of range: 0 \(1 to 4194304"    1000 -o /dev/null.db --db-block-size 0
expect_log "--db-block-size 1e12"                   "db-block-size out of range: 1e12 \(1 to 4194304" 1000 -o /dev/null.db --db-block-size 1e12
expect_log "--db-block-size 2^22 + 1"               "db-block-size out of range: 4194305"             1000 -o /dev/null.db --db-block-size 4194305
# The widest segment the tiers support is 16 MiB: -s above it is rejected,
# so are --l1-bytes/--l2-bytes above 1 GiB, and an automatic segment wider
# than 16 MiB is cut down to it (and counts right). -s at the maximum, with
# sparse primes (1.4e8^2 < 2e16), counts right.
expect_log "-s 1e9"                                 "-s too large: 1e9 \(at most 503316480"           1000 -s 1e9
expect_log "--l1-bytes 1e18"                        "--l1-bytes too large: 1e18 \(at most 1073741824" 1000 --l1-bytes 1e18
expect_log "--l2-bytes 2g"                          "--l2-bytes too large: 2g"                        1000 --l2-bytes 2g
expect_log "1 GiB caches: 16 MiB segment"           "16384 KiB instead of .*the widest the tiers support" 1e10 --start 9999000000 --l1-bytes 1g --l2-bytes 1g
check_start 10000000000 9999000000 -t 2 --l1-bytes 1g --l2-bytes 1g
check_start 20000000000000000 19999900000000000 -t 2 -s 503316480
# Memory budget (--max-mem): at 1e15 each thread takes ~33 MB (1.95M base
# primes x 8 B + 16 MiB) and ~66 MB are shared, so 150m leaves 2 threads of
# 4, and 50m not even one (it warns and runs on 1). Fewer threads don't
# change the count.
expect_log "--max-mem: fewer threads"         "memory: 2 threads instead of 4"   1e15 --start 999999999990000 -t 4 --max-mem 150m
expect_log "--max-mem: not even one thread"   "WARNING: .*may not fit"           1e15 --start 999999999990000 -t 4 --max-mem 50m
check_start 1000000000000000 999999999990000 -t 4 --max-mem 150m
T13="10000000000000 --start 9999999000000 -t 2"
T15="1000000000000000 --start 999999999000000 -t 2"
# base 4 MiB from an 8 MiB L2, capped to 32 x 4 KiB = 128 KiB (3932160 numbers)
expect_log "cap 32 x L1d (lying sysfs)"       "cap: 32 x L1d"                   $T13 --l1-bytes 4096 --l2-bytes 8388608
expect_log "cap: 128 KiB segment"             "segment=3932160,"                $T13 --l1-bytes 4096 --l2-bytes 8388608
# 1 MiB base from a 2 MiB L2, doubled in the sparse regime, back to the L2 (1 MiB) by the ceiling
expect_log "ceiling: L2 per thread (sparse)"  "ceiling: the L2 per thread"      $T15 --l1-bytes 32768 --l2-bytes 2097152
expect_log "ceiling: 1 MiB segment"           "segment=31457280,"               $T15 --l1-bytes 32768 --l2-bytes 2097152
# an explicit 1.5 MiB -s rounded down to the power of 2 the sparse tier needs
expect_log "-s rounded to a power of 2"       "a power of 2 for the sparse tier" $T15 -s 47185920
expect_log "-s rounded: 1 MiB"                "segment=31457280,"               $T15 -s 47185920
expect_log "-s kept without a sparse tier"    "segment=47185920,"               $T13 -s 47185920
expect_log "--tune sparse in the log"         "sparse cutoff: 1/2 of the segment \(--tune sparse\)" $T15 --tune sparse=1/2
expect_log "--tune medium_nta forced"         "medium-tier prefetchnta: no \(forced" $T13 --tune medium_nta=0
# med64 = the whole segment on a 256 KiB L2 (tuning.hpp's small-L2 rule), 1/6 on 512 KiB; --tune med64 overrides.
# Few base primes (1e10: 9,592) and one thread per core: the base is half the whole-L2 width,
# on any L2 (128 KiB from 256 KiB; 768 KiB from the 1.5 MiB that 32 x 48 KiB caps a 2 MiB L2 to);
# with more base primes (1e12 tail: 78,498) the whole width, 1.5 MiB.
expect_log "half L2: small L2, few primes"    "segment: half the whole-L2 width \(one thread per core, 9,592 base primes <= 40,000\)" 10000000000 -t 1 --l1-bytes 32768 --l2-bytes 262144
expect_log "half L2: 128 KiB segment"         "segment=3932160," 10000000000 -t 1 --l1-bytes 32768 --l2-bytes 262144
expect_log "half L2: large L2, few primes"    "segment=23592960," 10000000000 -t 1 --l1-bytes 49152 --l2-bytes 2097152
expect_log "whole L2: large L2, many primes"  "segment: whole L2 per thread" 1000000000000 --start 999999000000 -t 1 --l1-bytes 49152 --l2-bytes 2097152
expect_log "whole L2: 1.5 MiB segment"        "segment=47185920," 1000000000000 --start 999999000000 -t 1 --l1-bytes 49152 --l2-bytes 2097152
expect_log "whole-segment med64, 256 KiB L2"  "med64 cutoff: the whole segment \(L2 of 256 KiB or less\)" $T13 --l1-bytes 32768 --l2-bytes 262144
expect_log "whole-segment med64: population"  "med64, 0 medium" $T13 --l1-bytes 32768 --l2-bytes 262144
expect_log "med64 1/6 on a 512 KiB L2"        "^Starting.*[1-9][0-9]* medium" $T13 --l1-bytes 32768 --l2-bytes 524288
# small = 1/2 of the sub-block on a 256 KiB L2 (tuning.hpp's small-L2 rule): 990 primes below 8K
# instead of 526 below 4K with the 16 KiB sub-block of --l1-bytes 32768; --tune small overrides.
expect_log "small 1/2 on a 256 KiB L2"        "small cutoff: 1/2 of the sub-block \(L2 of 256 KiB or less\)" $T13 --l1-bytes 32768 --l2-bytes 262144
expect_log "small 1/2: population"            "^Starting.* 990 small base primes" $T13 --l1-bytes 32768 --l2-bytes 262144
expect_log "small 1/4 on a 512 KiB L2"        "^Starting.* 526 small base primes" $T13 --l1-bytes 32768 --l2-bytes 524288
expect_log "--tune small overrides the rule"  "^Starting.* 526 small base primes" $T13 --l1-bytes 32768 --l2-bytes 262144 --tune small=1/4

# The L3 gate (tuning.hpp): 1/4 from 4 MiB of L3 per active thread, also
# below the sparse regime when an octave of base primes lands in the sparse
# tier. Needs the machine's own L3 (sysfs), so only where it is >= 4 MiB:
# at -t 1 the whole L3 is one thread's.
L3_KB=$(cat /sys/devices/system/cpu/cpu0/cache/index3/size 2>/dev/null | tr -d 'K')
if [ -n "$L3_KB" ] && [ "$(cat /sys/devices/system/cpu/cpu0/cache/index3/level 2>/dev/null)" = "3" ] && [ "$L3_KB" -ge 4096 ]; then
    expect_log "1/4 cutoff by L3 per active thread (-t 1)" "sparse cutoff: 1/4 of the segment \(L3 per active thread >= 4 MiB\)" 10000000000000 --start 9999999000000 -t 1
    expect_log "no sparse tier at 1e12 (-t 1, one-octave margin)" "^Starting 1 threads.* 0 sparse" 1000000000000 --start 999999000000 -t 1
else
    echo "SKIP 1/4 cutoff by L3 per active thread (cpu0's L3 undetected or < 4 MiB)"
fi

# --- 7: a range in the real sparse regime (the last 1e8 below 1e15, 1.8M
# base primes, this machine's automatic cutoff) against primecount, and the
# errors nth_prime must catch ---
check_start 1000000000000000 999999900000000 -t 2
# A narrow window: almost no base prime has a multiple in it, and those
# that don't are not filed into the ring (set_range_end).
check_start 1000000000000000 999999999990000 -t 3
check_start 1000000000000000 999999999990000 -t 1

DB7="$WORKDIR/n1e5.db"
"$BIN" 100000 -o "$DB7" >/dev/null 2>&1
count7=$("$NTH_BIN" "$DB7" --count)
expect_nth_reject() {
    local label="$1"; shift
    if "$NTH_BIN" "$@" >/dev/null 2>&1; then
        printf "FAIL nth_prime %-30s should fail\n" "$label"; fail=1
    else
        printf "OK   nth_prime %-30s rejected\n" "$label"
    fi
}
expect_nth_reject "N=0"                 "$DB7" 0
expect_nth_reject "N > total"           "$DB7" $((count7 + 1))
expect_nth_reject "non-numeric N"       "$DB7" abc
cp "$WORKDIR/n1e5.blk" "$WORKDIR/n1e5.blk.orig"
truncate -s -1 "$WORKDIR/n1e5.blk"
expect_nth_reject "truncated .blk"      "$DB7" 1
mv "$WORKDIR/n1e5.blk" "$WORKDIR/n1e5.blk.gone"
expect_nth_reject "missing .blk"        "$DB7" 1
mv "$WORKDIR/n1e5.blk.orig" "$WORKDIR/n1e5.blk"
if [ "$("$NTH_BIN" "$DB7" "$count7")" == "99991" ]; then
    printf "OK   nth_prime %-30s 99991\n" "last prime < 1e5"
else
    printf "FAIL nth_prime %-30s\n" "last prime < 1e5"; fail=1
fi

# --- 8: ranges with -o (--start) and nth_prime's queries ---
# Reference: the full .txt up to 2e6 (its primes are already compared with
# primecount in parts 1-4); every range as .txt and as .db (blocks of 1000
# primes, to cross block boundaries) must be exactly its tail.
ok8() { printf "OK   range %-46s %s\n" "$1" "$2"; }
fail8() { printf "FAIL range %-46s %s\n" "$1" "$2"; fail=1; }
FULL8="$WORKDIR/full8.txt"
"$BIN" 2000000 -o "$FULL8" >/dev/null 2>&1
for s8 in 2 7 239 1000000 1999993; do
    exp8="$WORKDIR/exp8.txt"
    awk -v s="$s8" '$1 >= s' "$FULL8" > "$exp8"
    n8=$(wc -l < "$exp8")
    "$BIN" 2000000 --start "$s8" -t 3 -o "$WORKDIR/t8.txt" >/dev/null 2>&1
    "$BIN" 2000000 --start "$s8" -t 3 -o "$WORKDIR/t8.db" --db-block-size 1000 >/dev/null 2>&1
    if cmp -s "$exp8" "$WORKDIR/t8.txt"; then ok8 "[$s8, 2e6] .txt" "$n8 primes"; else fail8 "[$s8, 2e6] .txt" "differs from the full .txt's tail"; fi
    c8=$("$NTH_BIN" "$WORKDIR/t8.db" --count 2>&1)
    if [ "$c8" == "$n8" ] && "$NTH_BIN" "$WORKDIR/t8.db" --slice 1 "$n8" 2>&1 | cmp -s "$exp8" -; then
        ok8 "[$s8, 2e6] .db --count and --slice 1..$n8" "$n8 primes"
    else
        fail8 "[$s8, 2e6] .db --count and --slice 1..$n8" "count=$c8 expected=$n8"
    fi
    # Queries at the block edges (positions 1000/1001) and the range's.
    bad=""
    for pos in 1 2 999 1000 1001 2000 2001 "$n8"; do
        [ "$pos" -le "$n8" ] || continue
        want=$(sed -n "${pos}p" "$exp8")
        got=$("$NTH_BIN" "$WORKDIR/t8.db" "$pos" 2>&1)
        [ "$got" == "$want" ] || bad="$bad pos$pos($got!=$want)"
        got=$("$NTH_BIN" "$WORKDIR/t8.db" --next "$want" 2>&1)
        [ "$got" == "$want $pos" ] || bad="$bad next$want($got)"
        prev=$([ "$pos" -gt 1 ] && sed -n "$((pos - 1))p" "$exp8" || echo 0)
        if [ "$want" -gt "$s8" ] && [ "$prev" -lt $((want - 1)) ]; then # --next of a non-prime just below
            got=$("$NTH_BIN" "$WORKDIR/t8.db" --next $((want - 1)) 2>&1)
            [ "$got" == "$want $pos" ] || bad="$bad next$((want - 1))($got)"
        fi
    done
    for xy in "$s8 2000000" "$s8 $s8" "$((s8 + 1)) $((s8 + 50000))" "1999000 2000000" "1999994 1999996"; do
        set -- $xy
        [ "$1" -le "$2" ] && [ "$1" -ge "$s8" ] && [ "$2" -le 2000000 ] || continue
        want=$(awk -v a="$1" -v b="$2" '$1 >= a && $1 <= b' "$exp8" | wc -l)
        got=$("$NTH_BIN" "$WORKDIR/t8.db" --count "$1" "$2" 2>&1)
        [ "$got" == "$want" ] || bad="$bad count[$1,$2]($got!=$want)"
        if ! "$NTH_BIN" "$WORKDIR/t8.db" --range "$1" "$2" 2>&1 | cmp -s - <(awk -v a="$1" -v b="$2" '$1 >= a && $1 <= b' "$exp8"); then
            bad="$bad range[$1,$2]"
        fi
    done
    if [ -z "$bad" ]; then ok8 "[$s8, 2e6] .db position/--next/--count/--range" "block and range edges"; else fail8 "[$s8, 2e6] .db queries" "$bad"; fi
done
# Rejections: outside the stored range [1000000, 2e6], invalid positions.
"$BIN" 2000000 --start 1000000 -o "$WORKDIR/t8.db" >/dev/null 2>&1
n8=$("$NTH_BIN" "$WORKDIR/t8.db" --count)
expect_nth_reject "--next below the range"       "$WORKDIR/t8.db" --next 999999
expect_nth_reject "--next with no prime after"   "$WORKDIR/t8.db" --next 1999999
expect_nth_reject "--count past the limit"       "$WORKDIR/t8.db" --count 1000000 2000001
expect_nth_reject "--range below"                "$WORKDIR/t8.db" --range 5 1000000
expect_nth_reject "--slice from 0"               "$WORKDIR/t8.db" --slice 0 3
expect_nth_reject "--slice past the total"       "$WORKDIR/t8.db" --slice 1 $((n8 + 1))
expect_nth_reject "unknown option"               "$WORKDIR/t8.db" --bogus
if "$NTH_BIN" "$WORKDIR/t8.db" --info | grep -q "range: *\[1,000,000, 2,000,000\]"; then
    ok8 "--info" "range [1,000,000, 2,000,000]"
else
    fail8 "--info" "$("$NTH_BIN" "$WORKDIR/t8.db" --info 2>&1 | head -3 | tr '\n' ' ')"
fi
# A tail in the sparse regime: the last 1e7 below 1e15, against primecount.
N8=1000000000000000; S8=999999990000000
"$BIN" "$N8" --start "$S8" -t "$THREADS" -o "$WORKDIR/t15.db" >/dev/null 2>&1
"$BIN" "$N8" --start "$S8" -t "$THREADS" -o "$WORKDIR/t15.txt" >/dev/null 2>&1
want=$(( $("$PRIMECOUNT" "$N8") - $("$PRIMECOUNT" $((S8 - 1))) ))
c8=$("$NTH_BIN" "$WORKDIR/t15.db" --count 2>&1); l8=$(wc -l < "$WORKDIR/t15.txt")
X8=999999995000000; Y8=999999997500000
wantxy=$(( $("$PRIMECOUNT" "$Y8") - $("$PRIMECOUNT" $((X8 - 1))) ))
gotxy=$("$NTH_BIN" "$WORKDIR/t15.db" --count "$X8" "$Y8" 2>&1)
if [ "$c8" == "$want" ] && [ "$l8" == "$want" ] && [ "$gotxy" == "$wantxy" ] &&
   "$NTH_BIN" "$WORKDIR/t15.db" --slice 1 "$c8" | cmp -s - "$WORKDIR/t15.txt"; then
    ok8 "[1e15 - 1e7, 1e15] .db/.txt vs primecount" "$want primes, [X, Y] $wantxy"
else
    fail8 "[1e15 - 1e7, 1e15]" "db=$c8 txt=$l8 primecount=$want; [X,Y] $gotxy vs $wantxy"
fi

if [ "$fail" -eq 0 ]; then
    echo "All tests OK."
else
    echo "Some test failed." >&2
fi
exit "$fail"
