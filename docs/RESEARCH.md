# Research log: tried, measured, reverted

This collects every optimization attempt this project has tried, benchmarked, and
either kept or reverted -- pulled out of the source files' own comments so those
files stay readable, while the actual measurements and reasoning behind each
decision stay somewhere. Organized by source file, in the order the ideas appear
there. Design rationale for the code as it stands today still lives next to the
code (see [ALGORITHM.md](ALGORITHM.md)); this file is the "what else was tried and
why it didn't stick" record.

**Knobs named below:** until 2026-10-01 the A/B switches were `ERATOSTENES_*`
environment variables. They're gone now: the benchmark ones became flags
(`ERATOSTENES_START` -> `--start`, `ERATOSTENES_DEBUG_IDLE` -> `--debug-idle`;
`ERATOSTENES_TAIL_CHUNKS_PER_THREAD` is automatic, 8 chunks per thread with
`--start`), the still-useful ones became `--tune` keys (`SMALL_NUM/_DEN` ->
`small=a/b`, `MED64_NUM/_DEN` -> `med64=a/b`, `SPARSE_NUM/_DEN` -> `sparse=a/b`,
`BIG_2310` -> `big2310=0|1`), `DB_JOURNAL` was removed after its server A/B
lost (see the `journal_mode=OFF` entry), and the ones whose
decision was settled were removed together with the code they switched off
(`MED64_210` and the mod-30 med64 tier, `MED64_NTA`, `MEDIUM_NTA`'s override,
`NARROW_EARLY`, `MIN_SEGS_PER_CHUNK`). The entries keep the names they were
measured with.

Unless noted otherwise, measurements are from the project's dev PC (i5-11400F, 6C/
12T, no E-cores) using `perf stat cycles:u` (not wall-clock -- see the project's own
lesson on why cycles:u is trusted over wall-clock on contended machines, referenced
throughout below).

## Index

- [erat_small.hpp](#erat_smallhpp)
  - [Small tier's unrolled loop applied to medium-hit-count primes (measured, not adopted)](#small-tiers-unrolled-loop-applied-to-medium-hit-count-primes-measured-not-adopted)
  - [`cross_off`: narrowing locals to uint32_t (tried, reverted, 2026-09-25)](#cross_off-narrowing-locals-to-uint32_t-tried-reverted-2026-09-25)
  - [`cross_off_medium`: mod-210 multiplier stepping (2026-09-24)](#cross_off_medium-mod-210-multiplier-stepping-2026-09-24)
  - [`cross_off_medium`: mod-2310 stepping, considered, not implemented (2026-09-25, external review, Opus 5.5)](#cross_off_medium-mod-2310-stepping-considered-not-implemented-2026-09-25-external-review-opus-55)
  - [`cross_off_medium`: 4-way interleaved stepping (tried, reverted)](#cross_off_medium-4-way-interleaved-stepping-tried-reverted)
  - [`cross_off_medium`: class-specialized layout (kept, 2026-09-24)](#cross_off_medium-class-specialized-layout-kept-2026-09-24)
  - [`cross_off_medium`: 2-ahead software prefetch (tried, reverted, 2026-09-25, follow-up session)](#cross_off_medium-2-ahead-software-prefetch-tried-reverted-2026-09-25-follow-up-session)
  - [med64: segment-byte prefetch K * qp ahead (tried, reverted, 2026-10-06)](#med64-segment-byte-prefetch-k--qp-ahead-tried-reverted-2026-10-06)
  - [`cross_off_medium`: EratMedium-style 64-list restructuring](#cross_off_medium-eratmedium-style-64-list-restructuring)
  - [med64: mod-210 stepping, two variants (tried, both reverted, 2026-09-26, external review, Opus 5.5)](#med64-mod-210-stepping-two-variants-tried-both-reverted-2026-09-26-external-review-opus-55)
  - [Small tier: mod-210 stepping as 7 unrolled mod-30 copies (tried, reverted, 2026-09-27)](#small-tier-mod-210-stepping-as-7-unrolled-mod-30-copies-tried-reverted-2026-09-27)
  - [`cross_off`: branchless tail for the small and med64 tiers (tried, reverted, 2026-09-27)](#cross_off-branchless-tail-for-the-small-and-med64-tiers-tried-reverted-2026-09-27)
  - [`cross_off_medium`: byte positions + doubled tables (kept, 2026-09-27)](#cross_off_medium-byte-positions--doubled-tables-kept-2026-09-27)
  - [med64: EratMedium-style checked loop, `cross_off_checked` (kept, 2026-09-29)](#med64-eratmedium-style-checked-loop-cross_off_checked-kept-2026-09-29)
  - [`cross_off_medium`: struct-of-arrays state + gated `prefetchnta` (kept, 2026-09-29)](#cross_off_medium-struct-of-arrays-state--gated-prefetchnta-kept-2026-09-29)
  - [`cross_off_medium`: per-hit phase-wrap test dropped when the plan bounds the hits (tried, neutral, reverted, 2026-10-05)](#cross_off_medium-per-hit-phase-wrap-test-dropped-when-the-plan-bounds-the-hits-tried-neutral-reverted-2026-10-05)
  - [`cross_off_medium`: `qp` as 1-byte deltas (kept, 2026-09-30)](#cross_off_medium-qp-as-1-byte-deltas-kept-2026-09-30)
  - [med64: mod-210 stepping on the checked loop, `cross_off_checked210` (kept, 2026-09-30)](#med64-mod-210-stepping-on-the-checked-loop-cross_off_checked210-kept-2026-09-30)
- [wheel.hpp](#wheelhpp)
  - [Wheel size: mod 6 vs. mod 30 vs. mod 210 (historical, pre-tiered-marking architecture)](#wheel-size-mod-6-vs-mod-30-vs-mod-210-historical-pre-tiered-marking-architecture)
  - [`ONFLY_CORRECTION`/`GAP_K`: shared table replacing a per-prime `delta[]` (kept)](#onfly_correctiongap_k-shared-table-replacing-a-per-prime-delta-kept)
- [wheel210_big.hpp](#wheel210_bighpp)
  - [`GAP_K210`/`ONFLY_CORRECTION210` table shape: chained-index vs. flat arrays](#gap_k210onfly_correction210-table-shape-chained-index-vs-flat-arrays)
- [segment_sieve.hpp](#segment_sievehpp)
  - [Medium tier: 64-list restructuring, retry with a block-pool allocator (2026-09-24, idea 2 from an external review, Opus 5.5, second round)](#medium-tier-64-list-restructuring-retry-with-a-block-pool-allocator-2026-09-24-idea-2-from-an-external-review-opus-55-second-round)
  - [Medium tier: 64-list restructuring scoped to a bounded sub-band (`med64_primes`, KEPT, 2026-09-26)](#medium-tier-64-list-restructuring-scoped-to-a-bounded-sub-band-med64_primes-kept-2026-09-26)
  - [`small_limit` re-tuned jointly with `med64_limit` (KEPT, 2026-09-26)](#small_limit-re-tuned-jointly-with-med64_limit-kept-2026-09-26)
  - [dTLB pressure at large N: investigated, ruled out (2026-09-25, external review, Opus 5.5)](#dtlb-pressure-at-large-n-investigated-ruled-out-2026-09-25-external-review-opus-55)
  - [Sparse tier: EratBig-style rewrite (adopted, 2026-09-24, isolated test of point 1 from an external review, Opus 5.5)](#sparse-tier-eratbig-style-rewrite-adopted-2026-09-24-isolated-test-of-point-1-from-an-external-review-opus-55)
  - [Sparse tier (original design, before the EratBig rewrite above): stepping-math attempts 1-5](#sparse-tier-original-design-before-the-eratbig-rewrite-above-stepping-math-attempts-1-5)
  - [Sparse tier: `process_big`/`process_sparse_bucket` split into its own noinline function](#sparse-tier-process_bigprocess_sparse_bucket-split-into-its-own-noinline-function)
  - [Sparse tier design, current: fixed-size pooled blocks (attempt 6)](#sparse-tier-design-current-fixed-size-pooled-blocks-attempt-6)
  - [`SPARSE_BLOCK_ENTRIES` tuning: 1024 vs. 128](#sparse_block_entries-tuning-1024-vs-128)
  - [Sparse tier: prefetch the next block of the chain, once per block (kept, 2026-09-27)](#sparse-tier-prefetch-the-next-block-of-the-chain-once-per-block-kept-2026-09-27)
  - [Sparse tier attempts 7-10 (all tried, reverted)](#sparse-tier-attempts-7-10-all-tried-reverted)
  - [Attempt 11: shrinking the live entry from 8 to 7 bytes (tried, reverted, 2026-09-27)](#attempt-11-shrinking-the-live-entry-from-8-to-7-bytes-tried-reverted-2026-09-27)
  - [Segment processed in parts for the sparse tier (tried, reverted, 2026-09-29)](#segment-processed-in-parts-for-the-sparse-tier-tried-reverted-2026-09-29)
  - [Sparse tier: `process_big` loads per hit, ~12 -> 5 (kept, 2026-09-29)](#sparse-tier-process_big-loads-per-hit-12---5-kept-2026-09-29)
  - [med64: re-filing without `std::vector::push_back` (tried, reverted, 2026-09-29)](#med64-re-filing-without-stdvectorpush_back-tried-reverted-2026-09-29)
  - [med64: `prefetchnta` on the state stream (kept, 2026-09-29)](#med64-prefetchnta-on-the-state-stream-kept-2026-09-29)
  - [Sparse tier: mod-2310 multiplier wheel (kept, 2026-09-30)](#sparse-tier-mod-2310-multiplier-wheel-kept-2026-09-30)
  - [Sparse tier: next-block prefetch spread over the current block (kept, 2026-10-04)](#sparse-tier-next-block-prefetch-spread-over-the-current-block-kept-2026-10-04)
  - [Sparse tier: `process_big` is issue-bound at 12 threads; the ring's wrap mask and the spills are what is left (open, 2026-10-05)](#sparse-tier-process_big-is-issue-bound-at-12-threads-the-rings-wrap-mask-and-the-spills-are-what-is-left-open-2026-10-05)
  - [Sparse tier: `process_big` in groups of 4 entries, no per-iteration edge tests (kept, 2026-10-06)](#sparse-tier-process_big-in-groups-of-4-entries-no-per-iteration-edge-tests-kept-2026-10-06)
  - [Activation at the top of N on old cores: 83 ns per prime on Nehalem, two flags to split it (open, 2026-10-04)](#activation-at-the-top-of-n-on-old-cores-83-ns-per-prime-on-nehalem-two-flags-to-split-it-open-2026-10-04)
  - [Sparse activation from the bitmap index: 18% fewer instructions per prime (kept, 2026-10-05)](#sparse-activation-from-the-bitmap-index-18-fewer-instructions-per-prime-kept-2026-10-05)
  - [i5-3470 profile at 1e12: the med64 tier over the whole-L2 segment is 59% of the cycles (open, 2026-10-04)](#i5-3470-profile-at-1e12-the-med64-tier-over-the-whole-l2-segment-is-59-of-the-cycles-open-2026-10-04)
  - [i5-3470 follow-up: med64 gate at the tails, the +6% that was not code, and the 1e12 window profile (2026-10-04)](#i5-3470-follow-up-med64-gate-at-the-tails-the-6-that-was-not-code-and-the-1e12-window-profile-2026-10-04)
- [gap_encoding.hpp](#gap_encodinghpp)
  - [Gap encoding: wheel-index deltas](#gap-encoding-wheel-index-deltas)
  - [`.db` extraction in wheel indices: `GapBlockSink::write_k` (kept, 2026-10-01)](#db-extraction-in-wheel-indices-gapblocksinkwrite_k-kept-2026-10-01)
- [sqlite_prime_store.hpp](#sqlite_prime_storehpp)
  - [`journal_mode=OFF` for the bulk load (tried, reverted, 2026-10-01)](#journal_modeoff-for-the-bulk-load-tried-reverted-2026-10-01)
  - [Write-pipeline knobs: `BATCH`, `--db-block-size`, `wal_autocheckpoint` (all measured, kept at their defaults)](#write-pipeline-knobs-batch---db-block-size-wal_autocheckpoint-all-measured-kept-at-their-defaults)
  - [`PRAGMA cache_size` increase (tried, reverted, 2026-09-25)](#pragma-cache_size-increase-tried-reverted-2026-09-25)
  - [`blocks`/`block_data` table split (kept)](#blocksblock_data-table-split-kept)
  - [Page size (kept)](#page-size-kept)
- [main.cpp](#maincpp)
  - [`SUB_BLOCK_BYTES`: per-thread vs. machine-wide sizing (kept, uniform-with-margin wins)](#sub_block_bytes-per-thread-vs-machine-wide-sizing-kept-uniform-with-margin-wins)
  - [`sieve_chunk`: one `SegmentSieve` per worker instead of per chunk (tried, reverted -- neutral on cycles:u, 2026-09-26)](#sieve_chunk-one-segmentsieve-per-worker-instead-of-per-chunk-tried-reverted----neutral-on-cyclesu-2026-09-26)
  - [`run_parallel_chunks`: chunk-granularity idle-time investigation (2026-09-25, external review, Opus 5.5)](#run_parallel_chunks-chunk-granularity-idle-time-investigation-2026-09-25-external-review-opus-55)
  - [Chunk-width floor: at least 4 segments per chunk (kept, 2026-09-27)](#chunk-width-floor-at-least-4-segments-per-chunk-kept-2026-09-27)
  - [i5-13500 server gap vs primesieve: medium-tier call count, sparse cutoff 1/2 gated on per-thread L2 (2026-09-28)](#i5-13500-server-gap-vs-primesieve-medium-tier-call-count-sparse-cutoff-12-gated-on-per-thread-l2-2026-09-28)
  - [`small_limit` cutoff tuning](#small_limit-cutoff-tuning)
  - [Small cutoff 1/2 of the sub-block on a 256 KiB L2 core (kept, 2026-10-06)](#small-cutoff-12-of-the-sub-block-on-a-256-kib-l2-core-kept-2026-10-06)
  - [Cache-topology sizing: per-CPU-minimum step (kept)](#cache-topology-sizing-per-cpu-minimum-step-kept)
  - [Medium/sparse cutoff raised above `seg_k_width` (tried, reverted, 2026-09-27)](#mediumsparse-cutoff-raised-above-seg_k_width-tried-reverted-2026-09-27)
  - [EratBig-style sparse tier: forcing a power-of-2 segment width, and `sparse_limit = seg_k_width/4` (all attempts reverted)](#eratbig-style-sparse-tier-forcing-a-power-of-2-segment-width-and-sparse_limit--seg_k_width4-all-attempts-reverted)
  - [`--start`: the primes in the rounded-down head of the first word were counted (bug, fixed 2026-10-04)](#--start-the-primes-in-the-rounded-down-head-of-the-first-word-were-counted-bug-fixed-2026-10-04)
  - [`--start`: the start itself was dropped when its wheel index was 63 mod 64 (bug, fixed 2026-10-05)](#--start-the-start-itself-was-dropped-when-its-wheel-index-was-63-mod-64-bug-fixed-2026-10-05)
  - [`ByteCounter`: digit count without `to_chars` (tried, tie, reverted, 2026-10-05)](#bytecounter-digit-count-without-to_chars-tried-tie-reverted-2026-10-05)
  - [Half the whole-L2 width with few base primes, on every one-per-core machine (kept, 2026-10-06)](#half-the-whole-l2-width-with-few-base-primes-on-every-one-per-core-machine-kept-2026-10-06)
- [arg_parser.hpp](#arg_parserhpp)
  - [`--zstd-level` default: 1 (kept, 2026-09-28)](#--zstd-level-default-1-kept-2026-09-28)
  - [Sub-block size: half the L1d, not all of it (kept, 2026-09-27)](#sub-block-size-half-the-l1d-not-all-of-it-kept-2026-09-27)
  - [Sub-block: the whole L1d when each thread has a core to itself (kept, 2026-10-01)](#sub-block-the-whole-l1d-when-each-thread-has-a-core-to-itself-kept-2026-10-01)
  - [Auto segment width: dropping the `isqrt(limit)` cap (kept)](#auto-segment-width-dropping-the-isqrtlimit-cap-kept)
  - [Segment width doubled once the sparse tier exists (kept, 2026-09-27)](#segment-width-doubled-once-the-sparse-tier-exists-kept-2026-09-27)
  - [Narrow segment for the chunks below narrow² (kept, 2026-09-28)](#narrow-segment-for-the-chunks-below-narrow-kept-2026-09-28)
  - [`seg_k_width_from_l2_bytes`'s extra /2 margin, applied on top of an already-per-thread L2 share (kept, counterintuitive)](#seg_k_width_from_l2_bytess-extra-2-margin-applied-on-top-of-an-already-per-thread-l2-share-kept-counterintuitive)
- [presieve.hpp](#presievehpp)
  - [Period-sized tables: fill in 4 KiB chunks with wraparound (kept, 2026-09-29)](#period-sized-tables-fill-in-4-kib-chunks-with-wraparound-kept-2026-09-29)
  - [`fill()`: skip the `self_k` correction loop when it can't possibly match (kept, 2026-09-26)](#fill-skip-the-self_k-correction-loop-when-it-cant-possibly-match-kept-2026-09-26)
  - [Extending pre-sieve coverage past prime 163 (tried three ways, all reverted)](#extending-pre-sieve-coverage-past-prime-163-tried-three-ways-all-reverted)
- [Makefile](#makefile)
  - [PGO training set: a natural 1e13 pass (tried, reverted, 2026-09-25, follow-up session)](#pgo-training-set-a-natural-1e13-pass-tried-reverted-2026-09-25-follow-up-session)
  - [Two power regimes on every machine: burst and sustained (open, 2026-10-04)](#two-power-regimes-on-every-machine-burst-and-sustained-open-2026-10-04)
  - [PGO overall: measured on the dev PC, not adopted on the production server](#pgo-overall-measured-on-the-dev-pc-not-adopted-on-the-production-server)

## erat_small.hpp

### Small tier's unrolled loop applied to medium-hit-count primes (measured, not adopted)

Foundational reasoning behind the small/medium tier split itself (not a later
attempt): `cross_off`'s unrolled 8-hits-per-cycle loop is ~2 instructions/hit,
vastly cheaper than the medium tier's generic one-hit-per-iteration stepping
(~9-12 instructions/hit) -- but only once its per-prime entry/exit cost (an
unpredictable jump into the switch, plus an unpredictable exit point) is
amortized over enough hits. Measured directly on medium-tier-range primes at
N=1e11: ~5.6x the branch misses of the generic loop, a net regression despite
38% fewer instructions retired. This is why the tier boundary (`small_limit`)
exists at all, not just a difference in per-hit cost -- see `main.cpp`'s own
`small_limit` cutoff-tuning entry (below) for where that boundary is actually
set.

### `cross_off`: narrowing locals to uint32_t (tried, reverted, 2026-09-25)

Every local in `cross_off` (qp, p, the `o[j]`s, b, end) is genuinely bounded well
under 2^20 regardless of N -- qp by `small_limit` (an L1-cache-derived constant,
never N-dependent), the rest by the segment's own byte width (`seg_k_width/8`,
capped under 2^30 by `SegmentSieve`'s own constructor check) -- so narrowing every
local from `uint64_t` to `uint32_t` looked like a free win (shorter x86-64 encoding,
no REX prefix) with no range risk.

Measured the opposite: perf stat cycles:u, natural auto -s, two reps each --
N=1e11: 118.44G/118.50G -> 120.19G/120.24G cycles:u (+1.47% both reps); N=1e12:
1.4604T/1.4645T -> 1.4742T/1.4805T cycles:u (+0.95%/+1.09%). instructions:u rose too
(+3.2% at 1e11, +2.5% at 1e12, identically across reps -- deterministic, not
noise), the opposite of the instruction-count savings the shorter encoding was
expected to give.

Root cause not isolated further (would need `perf annotate` to see exactly which
instructions the compiler added), but the practical takeaway holds regardless: on
this compiler/target, 64-bit locals for pointer-offset arithmetic on x86-64
apparently let GCC's optimizer do something it can't when 32/64-bit types mix, even
though every value involved provably fits in 32 bits. Reverted to `uint64_t`
throughout.

### `cross_off_medium`: mod-210 multiplier stepping (2026-09-24)

Every medium-tier prime is always > 163 (presieve's `{7,23,37}` group,
`presieve.hpp`, always covers 7 first), so any hit whose multiplier is a multiple
of 7 is redundant -- already marked composite by 7's own presieve pattern. Stepping
through only the 48/210 multiplier phases coprime to 210 instead of the 8/30
coprime to 30 (`GAP_K210`/`ONFLY_CORRECTION210`, `wheel210_big.hpp`) skips ~14% of
candidate hits in this tier. This is the same trick already validated for the
sparse tier's own big-wheel table.

A first version indexed a single combined table by a "next" field loaded from the
previous lookup (mirroring the sparse tier's own `big::TABLE`) and measured a
cycles:u REGRESSION despite real instruction savings -- the load-to-use chain
through that field serializes one table load behind the previous one every hit.
See `wheel210_big.hpp`'s section below for the numbers and the fix (plain
register-arithmetic index, like this file's own `j`/`w`).

Measured after that fix (perf stat cycles:u, single run at a time, natural auto
-s):
- N=1e11: 121.118G -> 117.970G cycles:u (-2.6%), cache-refs 1.803B -> 1.825B
  (flat), cache-misses 12.52M -> 10.55M (-15.7%).
- N=1e12: 1.5105T -> 1.4751T cycles:u (-2.3%), cache-refs 25.38B -> 20.40B
  (-19.6%), cache-misses 450.6M -> 422.8M (-6.2%).
- N=1e13: 19.801T -> 19.776T cycles:u (-0.13%, noise-level) -- NOT the growing win
  the original prediction expected. Root cause: a same-day, separate experiment
  (`main.cpp`, "EXPERIMENT IN PROGRESS" below) shrinks `seg_k_width` to the nearest
  power of 2 once `isqrt(limit)` reaches it, which happens by N=1e13 on this
  machine (2687 small / 152886 medium / 72036 sparse, vs 0 sparse at 1e12) -- that
  shift moves a growing share of large medium-tier primes into the sparse tier
  instead, so the medium tier's own population doesn't keep growing with N here the
  way the original reasoning assumed.

Kept anyway: never measured worse than flat at any N tried, no memory/layout cost
paid, and a real win at the N most runs actually spend most of their time at. If
the sparse/medium split changes again (segment-width tuning, hardware), re-measure
at 1e13+ before assuming this still helps there.

### `cross_off_medium`: mod-2310 stepping, considered, not implemented (2026-09-25, external review, Opus 5.5)

The obvious next step past mod-210 is mod-2310 (skip 7, 11 AND 13's redundant
multiples, all three already covered by `PRESIEVE_GROUPS`) -- 480 phases instead of
48, ~9% fewer candidate hits in both this tier and the sparse one.

Checked the actual table cost before writing any code: `wheel210_big.hpp`'s `Entry`
is 8 bytes; the sparse tier's flat per-(class,phase) table would grow from 384
entries (3KB) to 8*480=3840 (30KB) -- not the review's own ~15KB estimate, which
this file's git history has no matching derivation for; this tier's own `idx`
already needs 12 bits instead of 9 to address it. This file's two tables
(`GAP_K210` + `ONFLY_CORRECTION210`) would grow from ~1.7KB to ~17.3KB. Both land at
or past the 32KiB L1d the review itself flags on the server's E-cores -- and that's
each table ALONE, before counting whatever else (DenseState arrays, the segment bit
array) needs L1 at the same time.

Same conclusion `wheel.hpp` already reached for the mod-2310 BASE wheel, for the
same underlying reason (a wheel's table cost grows with the product of its primes;
the candidate reduction only grows with their sum of reciprocals) -- this is that
argument applying a second time, one level down, to the stepping tables instead of
the whole-program layout. Not implemented: the review's own estimate was a modest
1-3% gain, likely optimistic given the corrected table sizes, against a real risk
of blowing L1 on the actual target hardware.

A second, independent reason kills the SPARSE tier's half of this outright, past
just cache pressure: `DenseState.qw` is a `uint32_t`, and mod-210 there already
spends 9 bits on (class, phase) (8*48=384, needs 9), leaving 23 for `qp` -- max
representable prime ~251.66M (qp_max*30), comfortably past `isqrt(1e15)~31.62M`,
this project's own declared E15 target, with ~8x headroom. Mod-2310 needs 12 bits
for (class, phase) (8*480=3840), leaving only 20 for `qp` -- max representable
prime ~31.46M, which is BELOW `isqrt(1e15)`. Past that point `qp` silently wraps and
the sieve produces wrong results with no error -- this isn't a performance tradeoff
against a modest gain any more, it's incompatible with a goal this project has
already committed to, short of a bigger restructuring of `DenseState`'s packing than
this idea was ever meant to be.

(Aside, found while checking this: `base_prime_max` -- what the sparse tier's own
`qp` actually has to fit, not `seg_k_width` -- has no runtime assertion today, under
the CURRENT mod-210 packing either; harmless at E15 given the 8x headroom above, but
worth a real check if this project's own target ever moves past roughly
limit=6.3e16.)

The MEDIUM tier's mod-2310 half doesn't have this problem (its primes stay under
`seg_k_width`, orders of magnitude below this ceiling either way) -- it's still just
the cache-pressure argument above for that tier, not a hard rejection.

**Update 2026-09-30:** both objections to the sparse half are gone (the entry is one
64-bit word now, and a 32-bit row makes the table 15 KiB) -- implemented and kept, see
[Sparse tier: mod-2310 multiplier wheel](#sparse-tier-mod-2310-multiplier-wheel-kept-2026-09-30).

**Update 2026-10-01: the medium half, tried, reverted.** Profiling dev PC tails put the
medium tier at 40% of cycles at 1e13, 37% at 1e14 and 27% at 1e15, the largest tier
there, so -9.1% medium hits looked like up to ~3.5%. Implemented without a table of
its own: `cross_off_medium<PR, NTA, true>` stepped through the sparse tier's
`big::TABLE2310` (one hot 15 KiB table shared by both tiers), w counted 0..479 in a
register with an in-place wrap, state packed `(pos << 9) | w` (pos checked against
2^23, wider segments stayed on mod-210), activation on `M2310`/`NEXT_W2310`, behind
`--tune medium2310`. Counts identical at 1e10 (plain, `med64=0`, `-s 100000`), 1e11
and a 1e12 tail. Same binary, knob 1 vs 0, `-t 12`:

| | mod-2310 | mod-210 | cycles | instructions |
|---|---:|---:|---:|---:|
| 1e13 last 0.1% (3 reps) | 17.33G | 17.44G | -0.6% | -1.4% |
| 1e14 last 0.05% (2 reps) | 127.5G | 126.1G | +1.1% | -1.3% |
| 1e12 full (2 reps) | 1123.8G | 1112.4G | +1.0% | -0.9% |

A tier at 40% of the cycles losing 9% of its hits moved cycles by noise: this tier
is bound by the loop-exit mispredict of each prime (2-10 hits per segment, a trip
count that varies prime to prime), not by its hits -- the same finding as the
2026-09-28 server TopdownL1 (bad speculation, medium-tier call count). The bigger
table is a small net cost where medium is small (1e12). Don't retry the medium
half for hit count alone; only something that removes per-prime exits would move it.

### `cross_off_medium`: 4-way interleaved stepping (tried, reverted)

Each prime's own chain (k -> next k) is a serial dependency, but four DIFFERENT
primes' chains are independent, so the idea was to give out-of-order execution
other ready work while one lane stalls on a branch-misprediction recovery or
dependent load, instead of stalling through each prime fully before starting the
next (the actual bit-set is a strided-free scatter, so there was nothing for the
compiler to auto-vectorize the way `presieve.hpp`'s `fill()` does -- this was meant
as latency-hiding via interleaving, not SIMD; AVX-512 gather/scatter was ruled out
up front too, since the target server, i5-13500/Raptor Lake, has AVX-512 fused off
for having E-cores, unlike the dev PC).

Measured with perf stat cycles:u (not wall-clock): +13.2% at N=1e11 (141.2G ->
159.8G), +14.3% at N=1e12 (1639.1G -> 1873.2G). IPC went up both times (1.37->1.52,
1.41->1.61) but instruction count rose even more (+25.6%/+30.8%) and branch-miss
rate barely moved (7.97%->7.75%, 6.95%->6.45%) -- the hypothesized latency-hiding
either didn't happen or didn't matter, while the real, measured cost was
structural: the inner while loop runs until the LAST of the 4 lanes finishes, so a
lane with fewer hits this segment still pays an `if (aN)` check every remaining
iteration instead of retiring early like the scalar version's single while does per
prime. Reverted.

### `cross_off_medium`: class-specialized layout (kept, 2026-09-24)

Matching primesieve's own `EratMedium` split into `crossOff_7/11/13/.../31`, one per
residue class: PR is now a compile-time template parameter, one list per class
(`medium_[8]` in `segment_sieve.hpp`, same shape as the small tier's `small_[8]`)
instead of one flat list carrying a runtime `ri`. `big::ONFLY_CORRECTION210[PR]` is
now a compile-time-constant row offset (foldable into the load's displacement)
instead of a per-prime runtime-computed pointer -- removes one multiply-by-row-size
per prime per segment (not per hit; the real per-hit cost, `qp*gap_k[w]`, is
unavoidable here -- primesieve's own `EratMedium` avoids it by precomputing distinct
per-prime deltas, but that's sized for its 8-phase mod-30 wheel; with 48 phases
here, precomputing all of them costs more than the 1-3 hits/segment typical of this
tier would recoup).

This DOES split `medium_` into 8 lists, the same shape as the reverted 64-list
attempt below that lost to cache-footprint growth -- but 8 lists is a much smaller
fragmentation than 64, and each still holds every prime of its OWN class
permanently (no per-segment migration between lists, since a prime's class never
changes) -- structurally identical to how `small_[8]` already works without issue.
This tier's population saturates at its permanent max once `sqrt(N)` exceeds
`seg_k_width`, somewhere between N=1e12 and N=1e13 on the dev PC -- this tier's own
history has produced opposite-signed results at those two N more than once (see the
64-list attempts below), so always measure at both before trusting either alone.

### `cross_off_medium`: 2-ahead software prefetch (tried, reverted, 2026-09-25, follow-up session)

Two variants, both regressions. `perf annotate` at N=1e13 (natural auto -s) found
66-67% of this loop's own sampled cycles landing on the instruction right after the
`words[k>>6] |= ...` RMW store (sampling skid attributes stalls to the next retired
insn) -- consistent with an L1-dcache-load-miss diagnosis (~30% miss rate here). The
address for hit N+1 is pure register arithmetic (`GAP_K210`/`ONFLY_CORRECTION210`
lookups, no load from `words`), so hit N+2's address is knowable a full extra
iteration before it's needed -- deeper lookahead than the plain loop already gets
"for free" from computing k's own next value before looping back.

- Variant 1: maintained k/k1/k2 (current/+1/+2), chaining k2 off k1 with a fresh
  `GAP_K210[w1]`/`ONFLY_CORRECTION210[w1]` lookup each iteration.
- Variant 2: added combined 2-step tables (`TWO_GAP_K210`/`TWO_ONFLY_CORRECTION210`,
  `wheel210_big.hpp`) so `k2 = k + qp*TWO_GAP_K210[w] + TWO_ONFLY_CORRECTION210[PR][w]`
  is computable directly from `(k, w)` -- no k1 chain, no extra rotation state,
  independently computable from the k-advance right below it.

Correctness held for both (`make test` green). Measured (perf stat cycles:u,
natural auto -s, clean same-session baseline immediately before each):
- Variant 1 -- N=1e12: cycles:u 1.4730T -> 1.6215T (+10.1%), instructions:u
  +34.9%, L1-miss rate 34.31%->27.60% (-6.7pp). N=1e13: cycles:u 19.201T ->
  21.469T (+11.8%), L1-miss rate 30.28%->23.13% (-7.2pp).
- Variant 2 -- all four of N=1e10/1e11/1e12/1e13: cycles:u +5.8%, +9.3%, +11.4%,
  +11.2% respectively; L1-miss rate dropped MORE than variant 1 at every N
  (37.85%->31.34%, 36.31%->26.31%, 34.29%->23.47%, 30.28%->20.67% -- up to
  -10.8pp, the biggest miss-rate win either variant produced), yet the net cycles
  regression stayed essentially the same ~10-11% magnitude as variant 1, not
  smaller.

So variant 2 answers the "can this be done cheaper" question directly: removing the
k1 dependency chain and the extra rotation state (a structurally cleaner prefetch,
matching what any reasonable "cache instead of recompute" fix would look like)
reduces the L1-miss rate EVEN FURTHER than the naive version, but doesn't reduce the
net cycle cost at all. The one extra table lookup pair per hit (2 loads + 1
multiply + 2 adds for `gap_k2[w]`/`corr2[w]`) costs about as much as the memory
latency it hides, regardless of how cleanly it's scheduled -- this isn't an
implementation-quality problem, it's that the fixed per-hit overhead of computing a
lookahead address and the latency it saves are simply close in magnitude on this
hardware. Both variants reverted.

Not tried: prefetch distances other than exactly 2 (a 3-ahead or deeper lookahead
costs even more table-lookup overhead per hit for a diminishing latency-hiding
return, so it's not expected to flip the sign); hardware prefetcher tuning (outside
this codebase's control). This closes off software prefetching for this loop's RMW
store specifically, not just the one implementation shape.

### med64: segment-byte prefetch K * qp ahead (tried, reverted, 2026-10-06)

The dense regime's profile is the med64 kernel (51% of the cycles at 1e11
on the laptop with the Emerald Rapids plan emulated, `--l1-bytes 49152
--l2-bytes 2097152 -t 2`, which reproduces that sandbox's 1.04x and its
`small=1/2` gain, -2.9% 3/3), and the kernel is bound by the segment-byte
RMW missing L1. The medium tier's exact 2-ahead prefetch (above) lost on
its table lookups, so this tried the cheapest possible form: one
`prefetcht0 (s + K * qp, i)` per hit in `cross_off_checked210`, the base
`s + K * qp` precomputed per prime, no lookup -- the mean byte step on the
mod-210 wheel is 7p/48 = 4.4 qp, so K = 9 / 13 / 20 is ~2 / 3 / 5 hits
ahead, give or take the phase. `make variant DEFS=-DERA_M64_PF=K` against
the plain binary, interleaved: Emerald Rapids plan, 1e11 (9e10 window), -t
2, x3: **+14.3% / +12.6% / +8.6%** (every B above every A for 9 and 13);
default plan, 1e12 tail (1e10 window), -t 12, x5: **+12.5% / +19.8% /
+18.6%** (every B above every A for 20). Same verdict as the medium tier,
with a prefetch five times cheaper: the out-of-order core already overlaps
these RMWs by itself, and one more instruction per ~5-instruction hit costs
more than any latency it hides. Software prefetching of the segment byte is
closed for both dense tiers; knob removed.

### `cross_off_medium`: EratMedium-style 64-list restructuring

**Attempt 1** -- reuse `cross_off<PR>` (byte marking, constant masks) instead of the
on-the-fly bit loop, split into `WHEEL_SIZE*WHEEL_SIZE` lists keyed by (class PR,
entry phase J) instead of one flat list, so `switch(j)` becomes a compile-time-
constant jump per list rather than a runtime dispatch. Idea from an external review
(Opus 5.5, 2-vCPU VM, no perf access) predicting this would help MORE as N grows,
since the medium tier's share of total cycles grows with N.

Measured (perf stat cycles:u, natural auto -s):
- N=1e12: cycles:u 1.509T -> 1.335T (-11.5%), instructions:u -39.8% (matching the
  ~2-instructions-per-hit claim for the small tier), cache-miss rate 1.97%->5.16%,
  IPC 1.32->0.90.
- N=1e13: cycles:u 20.671T -> 19.582T (-5.3%), instructions:u -32.7%, cache-miss
  rate 5.57%->12.04%, IPC 1.36->0.97.

A real, reproducible win at both N (independently re-verified: 1e12 reproduced at
-11.2% cycles:u, cache-miss 2.19%->5.09%, near-exact match) -- but the OPPOSITE
trend from the one predicted: the win roughly halves from 1e12 to 1e13 while the
cache-miss rate roughly doubles, because 64 lists (vs one flat array) cost extra
memory footprint/locality that grows with the medium-tier population -- the same
failure mode as the sparse tier's own attempt 3 (below): trading instructions for
cache misses, on a codebase whose actual wins so far have all come from the
opposite trade (reducing cache misses, e.g. L1 sub-block decoupling and the
segment-width fix). Instruction savings stayed roughly flat (-39.8% -> -32.7%)
while the miss-rate cost roughly doubled -- extrapolating that divergence past
1e13 toward this project's actual E14/E15 target range, the miss-rate cost
plausibly overtakes the instruction savings and flips this from a win to a
regression well before reaching the N that matters here. Reverted for that reason
-- not because it measured as a loss at the N actually tested, but because the
trend argues against it holding up at the N this project targets.

**Attempt 2 / CONFIRMED (2026-09-25, follow-up session, fresh implementation)** --
the original attempt 1 above was never committed, so this was rewritten from
scratch, not recovered: re-attempted this exact idea, now with real `perf` access
on the dev PC instead of trusting the trend extrapolation above. New
double-buffered design (`medium64_cur_`/`medium64_next_`, swapped each segment
instead of migrating in place) to sidestep any implementation-specific confound.
Measured (perf stat cycles:u, single clean run each, natural auto -s):
- N=1e12: cycles:u 1.4644T -> 1.3791T (-5.8%), instructions:u -32.1%, cache-miss
  rate (LLC) 4.91%, IPC 1.26->0.91. A real win, smaller than attempt 1's -11.5%
  but the same direction.
- N=1e13: cycles:u 19.412T -> 20.042T (+3.25%, a REGRESSION, not just a smaller
  win), instructions:u still -18.9% (real, substantial), but cache-misses:u nearly
  DOUBLED (20.06B -> 39.29B, +95.8%) and fully consumed the instruction savings.

This directly confirms the trend-based rejection above was correct -- not by
extrapolating from two points this time, but by measuring the actual N=1e13
regression directly. The idea is now closed on real data at the N this project
targets (E13-E14), not just a projection past it. Don't re-attempt without a
fundamentally different fix for the footprint-vs-instruction trade (e.g. shrinking
`DenseState` itself, or bounding how many of the 64 lists can be simultaneously
"hot") -- the trade direction itself (fewer instructions for more cache pressure)
has now failed this same trend check three times in this codebase (see also the
sparse-tier attempt 3 below, and the `sparse_limit/4` cutoff experiment in
`main.cpp` below).

### med64: mod-210 stepping, two variants (tried, both reverted, 2026-09-26, external review, Opus 5.5)

A per-tier profile (VM, emulating the server's cache, 6e10 window at N=1e14 --
external review, Opus 5.5) found small+med64 together at 44% of cycles, and
noted both still step on the mod-30 wheel (8 multiplier phases) while medium
and sparse already skip the ~1/7 of hits that are redundant multiples of 7
(already covered by presieve) via mod-210 stepping (48 phases). Applying the
same trick to small+med64 was estimated at a 6% ceiling, "realistic 2-3% net"
-- flagged up front as the one place this project could beat primesieve
outright, not just match it, since primesieve's own EratSmall/EratMedium are
also mod-30. med64 was picked as the first target (bounded population, no
hardware-dependent memory-bound tier to fight, per the same review).

The proposed difficulty: one 48-hit mod-210 cycle spans 7p bytes (derived
below), vs. one 8-hit mod-30 cycle's p bytes -- the review's suggested fix for
the resulting larger per-call offset-table setup was "5 precomputed registers
(qp*2, qp*4, qp*6, qp*8, qp*10) plus a per-phase constant", since the gaps
between consecutive mod-210-coprime residues take only those 5 values (the
Jacobsthal function of 210 is 10) -- verified directly (Python, brute-force
over all 48 gaps): {2,4,6,8,10}, counts {15,15,14,2,2}, summing to 210.

**Math** (verified by direct simulation before writing any C++, 5000+
randomized cases plus 500 multi-segment fuzz sequences against a from-scratch
reference, `perf stat cycles:u` and `make test` both green at every step
below): generalizing `cross_off`'s mod-30 derivation, for p = 30qp+R[pr] and
multiplier m coprime to 210, byte(m) = qp*m + floor(R[pr]*m/30); one full
210-residue period advances the byte position by exactly 7p (not p) --
qp*210 + R[pr]*7 = 7*(30qp+R[pr]), since 210 is an exact multiple of 30 so the
correction term carries no fractional remainder. `o[w] = qp*(M210[w]-1) +
C210(pr,w)` is the direct mod-210 analogue of `cross_off`'s `o[j]`, needed as
a full 48-entry array (not a running scalar) to keep the steady-state loop's
stores independent-address (`s[b+o[w]]`, no dependency chain), exactly like
`cross_off`'s own `o[0..7]`.

**Variant 1 (`cross_off210`): 47 independent multiplications.** Each `o[w]`
computed directly (`qp * (M210[w]-1) + C210(pr,w)`, all compile-time except
`qp`) -- 47 independent multiply-adds per prime per segment call, vs. the
mod-30 tier's 7. Measured (dev PC, i5-11400F, `perf stat cycles:u`, single
clean run each, natural auto `-s`, `ERATOSTENES_MED64_MOD210=1`):

| N | baseline cycles:u | variant 1 cycles:u | delta | instructions:u delta | cache-misses:u delta |
|---|---:|---:|---:|---:|---:|
| 1e12 | 1.3498T | 1.4770T | **+9.4%** | +24.1% | +43.6% |
| 1e13 | 18.189T | 19.744T | **+8.6%** | +15.2% | +19.6% |

A real, consistent regression at both N (no sign flip) -- despite skipping
~1/7 of this tier's hits, cycles:u went UP, not down. Root cause: med64
primes get few hits per segment call (that's this tier's whole classification
criterion), so the 47-multiplication setup cost is paid on almost every call
without being amortized over enough hits to recoup it -- exactly the
difficulty the review itself flagged, just confirmed with a number instead of
an estimate.

**Variant 2 (`cross_off210b`): the review's own proposed fix -- 5 precomputed
qp-registers, cumulative offsets.** `o[w] = o[w-1] + step[gap(w)] + corr(pr,w)`
using one of 5 precomputed `qp*{2,4,6,8,10}` registers (matching the gap set
above) instead of computing each `o[w]` independently. Measured worse, not
better, than variant 1:

| N | baseline cycles:u | variant 2 cycles:u | delta | instructions:u delta | cache-misses:u delta |
|---|---:|---:|---:|---:|---:|
| 1e12 | 1.3455T | 1.5774T | **+17.2%** | +37.5% | +63.4% |

(N=1e13 not run for variant 2 -- already clearly worse than variant 1 at
1e12 by a wide margin, and variant 1 itself didn't flip sign at 1e13, so a
third ~7-minute run was judged low-value; see presieve.hpp's own precedent
for skipping a confirmation run once the direction is unambiguous.)

Root cause: computing `o[w]` cumulatively trades 47 independent multiplications
for 5 multiplications plus 47 *serially dependent* additions (`o[w]` reads
`o[w-1]`) -- MORE total arithmetic ops than variant 1 (52 vs. 47), and a real
dependency chain besides. Variant 1's independent multiplies, despite higher
per-op latency, let the CPU's out-of-order engine overlap them; variant 2's
cheaper-per-op but serially-chained adds can't overlap at all. This is the
same lesson `wheel210_big.hpp`'s own "chained-index vs. flat arrays" entry
already found in a different shape (a load-to-use chain beats fewer
instructions) -- here confirmed again for a pure-register dependency chain,
not a memory load.

**Verdict: both reverted, code removed.** (Superseded 2026-09-30: once med64
moved to the checked loop, which has no per-call offset table, mod-210 stepping
won -- see
[`cross_off_checked210`](#med64-mod-210-stepping-on-the-checked-loop-cross_off_checked210-kept-2026-09-30).
The original reasoning follows.) If revisited, the setup-cost-vs-few-hits mismatch is
structural to med64 specifically (by definition, its primes have few hits per
segment): a fix would need to amortize the offset table across *multiple
segments*, not just multiple hits within one, which is a materially different
design, not a variant of this one. The small tier (this idea's other
proposed target, not yet attempted) has the opposite population shape (many
hits per prime per sub-block), so this negative result does NOT by itself
rule out mod-210 stepping there -- that would need its own measurement, not
an extrapolation from med64's failure.

### Small tier: mod-210 stepping as 7 unrolled mod-30 copies (tried, reverted, 2026-09-27)

The small-tier half of the med64 mod-210 idea above, with a setup that costs
nothing extra: one 210-multiplier cycle is exactly 7 consecutive mod-30 cycles
(copy c = 0..6, each p bytes further on) with the one hit per copy whose
m = 30c + R[j] is a multiple of 7 left out (copy 3 skips two, one copy skips
none). So `cross_off210<PR>` kept `cross_off`'s same 8 register offsets and
just emitted 7 copies of its 8-store body with 8 of the 56 stores removed at
compile time (`if constexpr`), pending phase u = c*8 + j in the same 6 bits.
pi(N) exact at 1e6-1e11. Split the small tier into a mod-210 list (p below a
threshold, swept via env var) and the unchanged mod-30 list above it.

Codegen took three rounds, each measured:
1. GCC hoisted all 48 `s + c*p + o_j` sums out of the loop as invariants,
   spilled them and reloaded one per store: +9% cycles:u at 1e11. Fixed with
   an empty `asm("" : "+r"(b))` barrier after each `b += p`.
2. Loop checked `end` once per 7 copies, so entry/exit chains averaged ~48
   checked hits instead of ~8 (+2.5%). Rewritten as 7 copy bodies chained with
   gotos, one end check per copy, and the exit reusing the entry chain (halves
   code size).
3. Stores through an opaque `q = s + b` so each is one `or`, not lea + or.

Final state: at 1e11 (12 threads), all small primes on mod-210: stores
-7%, instructions -4.5%, **cycles:u flat** (101.0G vs 100.9G). Single thread at
1e10: stores -11.5%, instructions -6.3%, cycles:u flat. An isolated
microbenchmark (all 763 small primes over a 24KiB buffer) pinned down why:
for p in [167,1000) mod-30 retires **1.04 stores/cycle** (the L1 store-commit
ceiling) and mod-210 only **0.84**, with 16% fewer stores and ~4% *more*
cycles. The mod-30 loop (~30 uops) is served by the Loop Stream Detector (only
3.8G of its 9.5G uops come from the DSB); the 7-copy loop (~217 uops) doesn't
fit the LSD, runs from DSB + MITE (MITE uops 0.10G -> 0.73G, undelivered
slots +45%), and nearly all its stores are indexed RMWs (`orb $m,(%q,%o_j)`,
more fused uops than GCC's base+disp mix in the mod-30 loop). The frontend
loss is about exactly the store saving. Also swept alongside: small_limit and
med64_limit at the 24KiB sub-block (1/4 + 1/12 is still the optimum), and
smaller sub-blocks at fixed small_limit (worse -- ~68 cycles:u fixed cost per
prime per sub-block).

Might behave differently on a core with a bigger LSD/DSB (Golden Cove P-cores
on the i5-13500), but not tested there. Anyone retrying this needs the loop
body under the LSD size, not just fewer stores.

### `cross_off`: branchless tail for the small and med64 tiers (tried, reverted, 2026-09-27)

At 1e11, med64 was ~47% of branch-misses:u (0.31G, ~1.6 per cross_off call, all
conditional) and the small tier ~33%. After the unrolled loop exits, the hits
still below `end` are a prefix of j = 0..6 (offsets grow with j), and the checked
`ERAT_HIT` chain leaves at a data-dependent j -- about one mispredict per call.
Replaced it with 7 unconditional stores, the out-of-range ones redirected to
`s[end + j]` (a pad word past the segment; for a small-tier sub-block, the next
sub-block's first word, which `Presieve::fill` overwrites) and j = number that
fit. `make test` green.

- Both tiers: branch-misses:u -36%, instructions:u +17%, **cycles:u +3.4%** at
  1e11. The small tier is frontend/store-bound (see the mod-210 entry above), so
  ~3.5 wasted stores per call cost more than the mispredict saved.
- med64 only: branch-misses:u -16/-20/-9% at 1e10/1e11/1e12, cycles:u -1.7%,
  +0.4%, 0.0% (3 interleaved reps each) -- noise at the N that matters.

Where med64's cost actually sits, for whoever looks next: 77% of all L1 load
misses at 1e11 (~7.4G, ~85% of its hits), 95.5% of which hit L2 (L3 traffic
negligible) -- every med64 prime sweeps the whole 256KiB segment. ~5.4
cycles:u/hit vs ~2.8 for the small tier.

### `cross_off_medium`: byte positions + doubled tables (kept, 2026-09-27)

Per-tier IPC at 1e12 (cycles:u and instructions:u sampled separately on a `-g`
twin of the release binary, attributed by source line): medium ran 38% of all
instructions at IPC 1.41, against 0.80 for med64 and 0.91 for small -- the one
marking tier that looked instruction-bound rather than memory-bound. Its loop was
~17 instructions per hit, one of them the actual store:

- **v1, byte positions.** Marking bit index k cost `k >> 6`, a variable
  `1 << (k & 63)` and a 64-bit RMW; a byte position with a per-(class, phase)
  mask (like the sparse tier) is `s[pos] |= MASK210[PR][w]`. Step = `qp *
  DM210[w] + CORR210B[PR][w]` bytes, i.e. `big::TABLE`'s own dm/corr/mask split
  into flat arrays indexed by the register `w` (not the chained `next` field,
  and not fused into one struct -- both measured slower before, see
  `wheel210_big.hpp` below). ~14 instructions/hit.
- **v2, no per-hit wrap.** `w = (w + 1 == 48) ? 0 : w + 1` was 4 instructions
  per hit (lea, cmp, mov, cmov). The tables now hold two 48-phase cycles and the
  loop does `if (++w == 96) w = 48` (never taken with med64 on: a medium prime
  then has p >= seg_k_width/12, i.e. under ~40 hits per segment) and folds w
  back once per call. `w` is also `uint64_t` now, dropping a zero-extend.
  ~11 instructions/hit.

`make test` green, pi(N) exact to 1e11, also with `ERATOSTENES_MED64_NUM=0`
(the case where the wrap path runs). cycles:u, dev PC:

| N | base | v1 | v2 |
|---|---:|---:|---:|
| 1e11 | 102.5G | 102.2G (-0.3%) | -- |
| 1e12 | 1.286T | 1.264T (-1.7%) | 1.243T (-3.3%) |
| 1e13 (ABBA) | 17.47T | 17.02T (-2.8%) | **16.87T (-3.4%)** |

instructions:u at 1e13: 19.17T -> 16.91T (-11.8%). The gain grows with N, as it
should: the medium tier's share of the run grows with N (6% of cycles at 1e11,
28% at 1e12, 45% at 1e13 before this change). `GAP_K210`/`ONFLY_CORRECTION210`
(bit-granularity tables) were removed; nothing else used them.

**v3, one packed table (kept, same day).** After v2, per-tier IPC at 1e12 was
still 1.27 for medium, ~2.2 core cycles per hit with 4 loads per hit (mask, dm,
corr, and the store's own read) on a 2-load-port core. Packing `mask | dm << 8 |
corr << 16` into one `PACK210[PR][w]` word makes it 2 loads per hit, paying for
it with a couple of ALU ops (GCC uses `movzbl %ah` for dm). loads:u -11%,
instructions:u +1.9%; cycles:u **-0.37% at 1e12** (3/3 reps) and **-0.5 to -1.1%
at 1e13** (ABBA, both B runs below both A runs; the two A runs differed by 1.4%
from each other). Kept: small but same direction everywhere, and one table
instead of three. This contradicts the earlier fused-`{gap,corr}` regression
under `wheel210_big.hpp` below -- that one was bit-granularity, wall-clock only,
and never had its cause isolated.

**`med64_limit` re-swept after v3** (cycles:u, 1e12, 2 reps each): 1/12 1242G,
1/16 1242G, 1/8 1254G, 1/24 1248G, 1/48 1273G, med64 off 1343G. The cheaper
medium tier doesn't move the optimum; 1/12 stays.

### med64: EratMedium-style checked loop, `cross_off_checked` (kept, 2026-09-29)

The 1e14 server profile put ~+8 of the gap to primesieve (per 100) in med64 (see
[segment_sieve.hpp](#segment-processed-in-parts-for-the-sparse-tier-tried-reverted-2026-09-29)):
same cost per hit as EratMedium despite far more hits per call (12-682 vs 3-214),
so the fixed cost per call was the suspect. Side by side with primesieve's
`EratMedium::crossOff_*`:

| | `cross_off` (was med64's) | `EratMedium::crossOff_*` |
|---|---|---|
| entry | 8 offsets into a stack array, `b = i - o[j]` | straight into the switch with `i` |
| body | checked chain, unchecked 8-hit cycle loop, checked chain | one loop, one check per hit, `i += dist` |
| mispredicts per call | loop exit + data-dependent tail exit, 1.65 measured | loop exit, ~1 |
| exit | `i = b + o[j]`, another array load | direct |

The unchecked cycle saves a compare per hit, but med64 is store-bound and on a
wide core that compare issues beside the store for free. `cross_off_checked<PR>`
copies EratMedium's shape (switch into a `for (;;)`, one check per hit, 8
per-class distances); the small tier keeps `cross_off`, where the unchecked cycle
still pays (frontend-bound, hundreds of hits per sub-block call). Inlining pinned
so the A/B measures only med64 (see the method note in the entry linked above):
`cross_off` and `cross_off_medium` `noinline`, as GCC had laid them out in
`fe6260a`; `process_big` byte-identical.

Dev PC, full runs, ABBA against the `fe6260a` binary, counts exact:

| | old | new | change |
|---|---:|---:|---:|
| 1e12 cycles:u | 1221.2 / 1238.3G | 1198.5 / 1219.6G | -1.7% |
| 1e12 branch-misses:u | 9.28G | 8.21G | -11.5% |
| 1e12 instructions:u | 1215G | 1279G | +5.3% |
| 1e13 cycles:u | 16639 / 16938G | 16327 / 16712G | -1.6% |
| 1e13 branch-misses:u | 123.9G | 114.5G | -7.5% |

Server (i5-13500), `make benchmark` best of 7 vs the previous README best: 1e11 1.62s
(=), 1e12 23.82s vs 23.90s (-0.3%; median of 7, 23.92s, already matches the old
best), 1e13 305.02s vs 308.04s (-1.0%, best of the first 2 reps; primesieve
291.949s in the same session, 1.045x). Single server runs vary up to 5% (24.39s
and 23.17s at 1e12 minutes apart), so only best-of-N or same-session ABBA can
see an effect this size. Small and growing with N, as med64's share does. 1e14/1e15
pending.

### `cross_off_medium`: struct-of-arrays state + gated `prefetchnta` (kept, 2026-09-29)

**Where the medium tier's misses were.** Dev PC, 1e14 1% tail, per-tier
attribution (`perf record` on `mem_load_retired.{l1,l2,l3}_miss:u`, non-precise --
no PEBS under WSL): medium was 43% of cycles, 40% of L2 misses and 50% of L3
misses. `perf annotate` put ~75% of its L2 misses and ~81% of its L3 misses on
`s[pos] |= mask` -- the *segment*, not its own state, which the hardware streamer
already covers. The state (8 bytes per prime, read and rewritten every segment:
~1.1 MB per thread here, ~1 MB per thread x 20 threads on the i5-13500, i.e. its
whole L3) was flushing the segment out of L2 and, across threads, out of L3.

**Change 1, struct of arrays (SoA).** `qp` never changes, but rewriting `pos`/`w`
made the whole 8-byte entry dirty, so every line was written back. Now each class
has `dyn[i] = (pos << 6) | w` (rewritten) and `qps[i] = qp` (read-only): half the
bytes per prime stay clean and are dropped without a writeback. `pos` fits 26 bits
(checked by `SegmentSieve`'s constructor). Not the 7-byte sparse entry (attempt 11,
odd stride, byte packing): two aligned 4-byte arrays, +0.4% instructions.

**Change 2, `prefetchnta`.** Both streams prefetched 32 entries ahead with the NTA
hint, once per *prime* (not per hit, which is what failed before), so they reach
L1 without being allocated in L2.

Correct: `make test` for both variants, pi(1e12) exact, and every row below.
Dev PC (i5-11400F), `perf stat cycles:u`, 3 interleaved reps against `8b0174e`
(control built the same way, byte-identical to the `make` build):

| | control | SoA | SoA + NTA |
|---|---:|---:|---:|
| 1e12 full | 1211.8G | 1212.4G (tie) | 1222.0G (+0.8%) |
| 1e13, 10% tail | 1849.1G | 1811.3G (-2.0%) | 1736.9G (**-6.1%**) |
| 1e14, 5% tail | 14072.9G | 13334.4G (-5.2%) | 12367.7G (**-12.1%**) |
| 1e14, 5% tail, wall | 324.53s | 308.11s | 279.87s (**-13.8%**) |

Every NTA rep beat every control rep at 1e13 and 1e14 (1e14: 12276-12522G vs
13669-14446G). NTA only pays once the state overflows the caches, so it is gated
per tier set: on when medium primes x 8 bytes exceed the per-thread L3 share
(`MEDIUM_NTA_MIN_PRIMES` in main.cpp; 131,072 primes on the dev PC, whose 1e12 has
62,601 and 1e13 197,708). The crossover between those two points isn't measured.
`ERATOSTENES_MEDIUM_NTA=0/1` forces it. Gated binary, 1 rep: 1e12 1205.1G (NTA
off), 1e13 tail 1726.9G (on). Server A/B pending.

### `cross_off_medium`: per-hit phase-wrap test dropped when the plan bounds the hits (tried, neutral, reverted, 2026-10-05)

`perf annotate` of `cross_off_medium<1, true>` at the 1e15 tail (dev PC)
counts the kernel exactly: **22 instructions per prime visit and 13 per
hit**, two of them `cmp $0x60,%rcx; jne` -- the `++w == 96` wrap test the
header comment says is never needed once med64 is on (a medium prime then
has at most seg_k_width / med64_limit + 1 = 7 hits per segment, so w stays
below 96). Tried a `WRAP` template parameter picked at plan time from
`med64_limit` (`SieveConfig::medium_p_min`, `-DERA_MED_NOWRAP=0` for the
A/B). instructions:u on the 9e11-1e12 window 127.6 -> 126.4 G (-0.9%),
cycles:u 125.1 -> 125.3 G (equal); wall interleaved x3: 1e12 window
+1.8%, 1e15 tail +0.9%, both overlapping. The tier is bound by its loop
exit mispredict and the pos -> table -> pos latency chain, not by
instruction count: two of thirteen per hit buy nothing. Reverted; the
annotate's counts stay here as the reference for the medium tier's cost
model (visit 22, hit 13).

### `cross_off_medium`: `qp` as 1-byte deltas (kept, 2026-09-30)

After the SoA split, each medium prime still streams 4 bytes of read-only `qp`
per segment on top of its 4-byte `dyn`. At 1e14 on the dev PC (266,008 medium
primes, sparse cutoff 1/1) that is ~1 MB per thread per segment. It comes from
L3/DRAM, and the i5-11400F's L3 is inclusive. But the list is sorted by p and never
reordered, so `qp` only needs the gap to the previous prime of the same class. Brute
force over every prime < sqrt(1e15): the largest same-class gap is 52 * 30 (39 * 30
below 4.19M), so a byte always holds it. Change: `medium_qd_[pr]` holds `uint8_t`
deltas, the first entry is 0 from `medium_qp_base_[pr]`, and `cross_off_medium`
carries `qp += *qds` from prime to prime (one `movzbl` + `add` per prime, outside
the hit loop). Activation throws if a delta ever exceeded 255. 5 bytes per prime per
segment instead of 8. The hit loop's disassembly is unchanged (`nm`: only the
medium functions, `activate_medium` and `run_medium` changed; the rest is padding).

Correct: `make test`, and identical counts in every run below.

Dev PC, `perf stat`, ABBA against the `c5ec94f` binary:

| | pairs | control | qd | change |
|---|---:|---:|---:|---:|
| 1e14, 0.1% tail, cycles:u (mean) | 6 | 250.7G | 239.5G | **-4.5%** |
| 1e14, 0.1% tail, wall (mean) | 6 | 5.92s | 5.63s | -4.9% |
| 1e15, 0.1% tail, cycles:u (mean / min) | 2 | 3843 / 3636G | 3502 / 3471G | **-8.9% / -4.5%** |
| 1e15, 0.1% tail, wall (mean) | 2 | 95.8s | 83.9s | -12% |
| 1e15, 0.1% tail, `mem_load_retired.l3_miss` | 2 | 0.72G | 0.41G | -44% |
| 1e13, 10% tail, cycles:u | 2 | 1657G | 1637G | -1.2% |
| 1e12 full, cycles:u | 2 | 1152G | 1152G | tie |

At 1e14 the qd mean is below the control mean in all 6 ABBA groups; at 1e15 all 4
qd runs beat all 4 control runs. The control is also far noisier at 1e15
(3636-3995G vs 3471-3539G), the same pattern as med64 NTA: less memory traffic
makes the tier less sensitive to a bad memory state. L2 misses and writebacks
barely move (1e14: 4.69G vs 4.73G misses), so the gain isn't the L2-pollution
mechanism SoA fixed. It shows up as fewer L3 misses and less time waiting on the
stream. `MEDIUM_NTA_MIN_PRIMES` still assumes 8 bytes per prime: recomputing it
at 5 would turn NTA off for the dev PC's 1e13 wide tier set (197,708 primes),
which is a separate, unmeasured change. Server A/B pending.

### med64: mod-210 stepping on the checked loop, `cross_off_checked210` (kept, 2026-09-30)

The 2026-09-26 mod-210 attempts ([above](#med64-mod-210-stepping-two-variants-tried-both-reverted-2026-09-26-external-review-opus-55))
lost because they kept `cross_off`'s unrolled shape, which needs a per-call
offset table -- 48 entries for a 48-phase cycle, built with 47 multiplies or a
47-add dependency chain, on a tier with few hits per call. Since med64 moved to
EratMedium's checked loop ([above](#med64-eratmedium-style-checked-loop-cross_off_checked-kept-2026-09-29)),
that table is gone: each hit only needs the byte step from phase w to w+1,
`qp*dm + corr` with dm in {2,4,6,8,10}. So `cross_off_checked210<PR>` keeps
`qp*2/4/6/8/10` in 5 registers and takes `dm`, `corr` and the mask from
`big::TABLE` as compile-time constants per case: a 48-case switch into a
`for (;;)`, one bounds check per hit, exactly `cross_off_checked`'s shape.
The 1/7 of mod-30 hits whose multiplier is a multiple of 7 (always presieved;
every med64 prime is > 163) are skipped. The lists are keyed by (class, entry
phase w): 8 x 48 = 384, so the entry switch is still shared by every call in an
inner loop. `activate_med64_210` is `activate_medium`'s start derivation filed
under `pr*48 + w`. Cost: `run_med64<*, true>` is ~10.3 KB of code vs ~6.5 KB on
mod-30. `ERATOSTENES_MED64_210=0` goes back to mod-30 (A/B; the mod-30 path is
still compiled).

Correct: `make test`, and identical counts in every run below (pi(1e12) exact).

Dev PC (i5-11400F), `perf stat`, same binary with the knob 1/0 plus the
`11b6c0f` binary as control, order ctl, 1, 0, 0, 1, ctl:

| | control | mod-210 | mod-30 (knob 0) | mod-210 vs mod-30 |
|---|---:|---:|---:|---:|
| 1e12 full, cycles:u | 1203.0 / 1204.5G | 1156.5 / 1151.2G | 1198.8 / 1194.3G | **-3.6%** |
| 1e12 instructions:u | 1285.3G | 1236.2G | 1287.2G | -4.0% |
| 1e12 stores retired | 284.9G | 272.2G | 285.0G | -4.5% |
| 1e13 10% tail, cycles:u | 1697.0 / 1775.8G | 1668.1 / 1694.8G | 1715.2 / 1706.4G | **-1.7%** |
| 1e13 10% tail, instructions:u | 1687.3G | 1627.6G | 1688.2G | -3.6% |
| 1e14 5% tail, cycles:u | 12700.5G | 12763.5 / 13077.0G | 12732.9 / 12722.0G | inconclusive |
| 1e14 5% tail, instructions:u | 11656.3G | 11357.3G | 11660.5G | -2.6% |

Branch misses flat (1e12: 8.34G vs 8.33G). At 1e14 the cycles are inside the
dev PC's known 5-7% tail noise (the 13077G rep ran 342s against ~300s for the
others); instructions and stores still drop as expected. The second 1e13 control
rep (1775.8G, 47s wall against ~37s) is an outlier of the same kind.

Full runs, best of N against the previous README best:

| machine | N | before | after | change |
|---|---|---:|---:|---:|
| dev PC | 1e13 | 326.03s | 323.34s | -0.8% |
| server | 1e12 | 23.00s | 21.14s | **-8.1%** |
| server | 1e13 | 299.80s | 281.98s | **-5.9%** (primesieve 292.554s: 1.02x -> 0.96x) |

The server's 1e10/1e11 also improved (0.14 -> 0.128s, 1.61 -> 1.51s), but the same
README update also removed `benchmark.sh`'s 5s pause between reps, which cost
ramp-up at small N. So only 1e12/1e13 should be attributed to this change. The
server gains ~2-4x more than the dev PC. That fits med64's larger share there
(~33 of 100 at 1e14, see the segment_sieve.hpp entry on the 1e14 gap), but it
hasn't been measured directly: there is no same-session server ABBA with the knob.
Pending: server 1e14 (README 3617.87s predates this change and med64 NTA) and the
1e15 run in progress.

**Cutoffs re-swept with mod-210 med64 (dev PC, 2026-09-30).** mod-210 makes each
med64 hit cheaper, so the band might want to grow: either down (lower
`small_limit`) or up (raise `med64_limit`). `perf stat cycles:u`, `c5ec94f`
binary, 2 reps round-robin (second pass reversed), counts exact:

| config | small / med64 / medium (1e12) | 1e12 full | 1e13 10% tail |
|---|---|---:|---:|
| default (SMALL 1/4, MED64 1/12) | 763 / 15096 / 62601 | 1145.9G | 1655.7G |
| `MED64_DEN=6` | 763 / 29138 / 48559 | 0.0% | -0.2% |
| `MED64_DEN=8` | 763 / 22199 / 55498 | -0.1% | 0.0% |
| `MED64_DEN=16` | 763 / 11450 / 66247 | +1.5% | +0.9% |
| `SMALL_DEN=6` | 526 / 15333 / 62601 | +0.7% | +0.3% |
| `SMALL_DEN=8` | 401 / 15458 / 62601 | +2.0% | +2.7% |
| `SMALL_DEN=8`, `MED64_DEN=8` | 401 / 22561 / 55498 | +1.5% | +2.0% |

Lowering `small_limit` cuts branch misses 10-17% but costs cycles: for primes
below ~6k the small tier's unrolled mod-30 cycle still beats med64's checked
mod-210 loop. Raising `med64_limit` saves 4-6% of instructions and ties on
cycles. Defaults unchanged. Not re-swept on the server, where med64 is a larger
share at 1e14; the mod-30 `med64_limit` sweep there was flat within its 1%-tail
noise (see the i5-13500 entry in main.cpp).

**Follow-up (2026-10-01): skipping presieved multiples of 11, tried, reverted.**
At a 1e13 tail med64 takes 31% of cycles and 60% of all L1-dcache load misses
(medium: 38% / 25%), and `--tune med64=0` (its primes on the table-driven medium
loop) costs +76% instructions but only +21% cycles at 1e11 -- so it looked bound
by its segment misses, where a mod-2310 tier should pay. Instead of a 480-case
switch with 3840 lists, kept the 48-case loop and the 384 lists and carried
`t11 = (m / 210) % 11` in the state (`qw = qp << 10 | t11 << 6 | w`, updated once
per 48-phase cycle); a hit with `(t11 + M210[w]) % 11 == 0` (the multiplier is a
multiple of 11, presieved) stored to an L1-resident scratch byte instead of the
segment. Counts identical (1e8-1e12 incl. forced sparse). Same binary, knob 1/0,
3 reps (2 at 1e12):

| | cycles | instructions | L1 misses |
|---|---:|---:|---:|
| 1e11 `-t 1` | **+11.3%** | +19.5% | -4.6% |
| 1e11 `-t 12` | +2.6% | +17.3% | -8.8% |
| 1e13 last 0.1% `-t 12` | +3.9% | +10.3% | -6.0% |
| 1e12 full `-t 12` | +4.7% | +13.3% | -7.5% |

The misses dropped as intended, but ~2 extra instructions per hit (GCC emitted
the select as a branch, not a cmov; a cmov version would still add a compare and
a select) cost far more than they saved: med64's ~4 instructions per hit are not
free behind its misses. Together with the medium mod-2310 result above: neither
tier pays for removing hits with per-hit work; a mod-2310 med64 would have to be
the 480-case switch (no extra work per hit) to have a chance.

**Follow-up 2 (2026-10-01): the 480-case switch, tried, reverted.**
`cross_off_checked2310<PR>`: `cross_off_checked210`'s shape over the mod-2310
multiplier wheel -- 480 cases generated with nested macros, dm in {2,...,14}
from 7 multiples of qp, mask/dm/corr compile-time constants from
`big::TABLE2310`; state `(qp << 9) | w` filed under 8 x 480 = 3840
(class, phase) lists; same 4 instructions per hit. Counts identical (1e7-1e12,
forced sparse, `med64=1/6`, `small=1/16`). Same binary, `--tune med64w2310`
1 vs 0, 3 reps (2 at 1e12/1e14):

| | cycles | instructions | branch misses |
|---|---:|---:|---:|
| 1e11 `-t 1` | +5.8% | -1.7% | +18% |
| 1e11 `-t 12` | **+18.4%** | -1.3% | +18% |
| 1e13 last 0.1% | +10.7% | -1.0% | +4% |
| 1e14 last 0.05% | +13.0% | -0.9% | +3% |
| 1e12 full | **+17.5%** | -1.1% | +10% |

Instructions barely dropped (the 9% fewer hits are offset by 10x the lists:
more inner loops, more list switches for the entry jump) and cycles blew up,
worst with both SMT threads busy: ~8 x 17 KB of switch code, with the exit
stubs, doesn't fit the frontend the two siblings share, where the 48-case
version did. Closed: med64 stays on mod 210.

### `cross_off_medium`: two primes per iteration (tried, not adopted, 2026-10-04)

The one idea left on the medium tier after the bands: its loop is one
dependent chain per hit (position -> table row -> `qp` multiply -> next
position), and a class's list is sorted by p, so two consecutive primes have
almost the same p and expected hits. `cross_off_medium_pairs` (erat_small.hpp,
`-DERA_MED_PAIRS=1`, `make medpairs` builds `./eratostenes_medpairs`)
interleaves two primes in one loop while both are inside the segment, drains
each alone, and runs the odd prime through the plain loop -- process_big's
pairs, which were -2%, on the medium tier. It pays only if the loop is
latency-bound; it costs one more loop exit per pair.

Dev PC, `BIN_B=./eratostenes_medpairs` A/B x2 against the plain kernel, same
configuration:

| threads | N, window | pairs vs plain |
|---:|---|---:|
| 1 | 1e13, 1e10 | +2.5% (4/4 worse) |
| 1 | 1e12, 1e10 | +5.3% (4/4 worse) |
| 2 | 1e13, 1e10 | +2.8% (overlap) |
| 2 | 1e14, 1e10 | -4.1% (overlap) |
| 12 | 1e13, 1e11 | +3.4% (4/4 worse) |
| 12 | 1e12, 1e11 | +1.9% (overlap) |
| 12 | 1e15, 1e11 | +1.2% (overlap) |

Worse or noise everywhere: the medium loop is bound by its exit mispredict
and the per-call fixed cost, not by the chain's latency, and the drains add
exits. Same verdict as the bands from the other side. The knob stays for
A/Bs on other cores; the default is the plain loop.

## wheel.hpp

### Wheel size: mod 6 vs. mod 30 vs. mod 210 (historical, pre-tiered-marking architecture)

An early version of this project (before the segmented small/medium/sparse tier
split described in [ALGORITHM.md](ALGORITHM.md) existed -- back when marking used
a single per-prime jump table regardless of hit frequency) compared `WHEEL_PRIMES`
configs directly, rebuilding between runs (i5-11400F, `--count-only`):

| N | mod 6 | mod 30 | mod 210 | winner |
|---|---:|---:|---:|---|
| 10^10 | 1.00s | 1.01s | 1.01s | tie |
| 10^11 | 10.51s | 8.01s | **7.02s** | mod 210 |
| 10^12 | 123.58s | **89.54s** | 142.60s | mod 30 |

Mod 210 won at 10^11 but lost badly at 10^12 once its jump table (`phi(210)=48`
entries/prime) outgrew L3; mod 30 was the more consistent winner across the
range this project actually targets, so it's what shipped (`wheel.hpp`).

**These specific numbers are historical and pre-date the current architecture
by a wide margin** -- they were measured before the presieve (§3), the
small/med64/medium/sparse tier split (§6), and the EratBig-style sparse
rewrite all existed; today's mod-30 count-only time at N=1e12 on comparable
hardware is roughly 3-4x faster than the 89.54s above (see the current
[README benchmarks](../README.md#benchmarks)). They're kept here only as the
record of *why* mod 30 was picked over a bigger wheel, not as a current
performance reference -- re-running this comparison on the current codebase
would need its own fresh measurement, not a diff against these numbers.

### `ONFLY_CORRECTION`/`GAP_K`: shared table replacing a per-prime `delta[]` (kept)

Before this shared, `WHEEL_SIZE`-sized table existed, the medium/sparse tiers'
on-the-fly stepping (`delta(p,j) = qp*GAP_K[j] + ONFLY_CORRECTION[pr][j]`, see
`wheel.hpp`'s own derivation next to `make_onfly_correction`) used a per-prime
`delta[]` table instead -- one whose memory footprint grows with how many
primes use it, unlike the shared table (stays L1-resident regardless of tier
size). Measured ~3.25x faster for primes forced through this tier at N=1e11 on
the dev PC (i5-11400F) -- see the current [README benchmarks](../README.md#benchmarks)
for today's numbers on the tiers this feeds into. primesieve's own
EratMedium/WheelFactorization uses the same trick (a small shared wheel table
plus one multiply by the prime itself, its `WheelElement`/`nextMultipleFactor`)
-- this project's version was re-derived from its own `wheel_delta_at`, not
ported from there.

**Removed (2026-09-27).** No tier uses these mod-30 tables any more: medium and
sparse step with `wheel210_big.hpp`'s mod-210 tables, small and med64 with
`erat_small.hpp`'s constant offsets. `ONFLY_CORRECTION`/`GAP_K` were deleted from
`wheel.hpp`; `wheel_delta_at` stays (presieve table construction).

## wheel210_big.hpp

### `GAP_K210`/`ONFLY_CORRECTION210` table shape: chained-index vs. flat arrays

Deliberately kept as two flat arrays indexed by `(ri` fixed per prime, outside the
loop`)` and `w` (a plain incrementing loop variable), NOT as one
struct-with-a-"next"-field table indexed by a `w`/`idx` that's itself loaded from
the previous lookup.

A first version did the chained-index version (mirroring `big::TABLE`/`Entry`,
which the byte-marking sparse tier can afford since it's only ever called once per
prime per *segment*), and it regressed cycles:u despite ~15% fewer instructions:u --
the "next" field's load-to-use chain serializes one table load behind the previous
one every hit, whereas the old mod-30 code's `j = (j + 1) & 7` (and this version's
`w` wraparound) is pure register arithmetic with no such dependency, so independent
loop iterations' table loads can issue without waiting on each other. Measured
(perf stat cycles:u, N=1e12 natural auto -s): "next"-field version 1.5105T ->
1.5453T cycles:u (+2.3%, regression) despite instructions:u 2.002T -> 1.700T
(-15.1%, roughly the expected ~14% hit reduction) -- IPC dropped 1.33 -> 1.10,
confirming a latency, not throughput, problem. Reverted to the two-array form
before it was ever committed.

**Tried again, reverted (2026-09-25, follow-up session)**: a narrower variant of
the same idea -- fuse `GAP_K210[w]` and `ONFLY_CORRECTION210[PR][w]` into one
8-byte `{gap,corr}` struct per (PR, w), keeping `w` exactly as it already is
(loop-local register arithmetic, never loaded from the table), so the chained-
"next"-field problem above doesn't apply here. Reasoned this should be a pure win
(one load instead of two, same cache line) or at worst neutral. Measured the
opposite (wall-clock -- no perf access this session): N=1e12, 2 reps, 33.55s/34.25s
vs a ~30.8-31.5s baseline cluster (same session, same machine) -- consistently
8-10% SLOWER, not neutral. Root cause not isolated (no perf access to check
codegen/cache behavior directly), but the practical takeaway holds regardless:
even a same-index, non-chained fusion of two small per-class tables into one
struct-per-entry array regressed here, not just the chained-index version above.
Reverted; if ever revisited, get `perf` access first to see whether the compiler is
actually emitting one load or two for the struct read before assuming fusing
helps.

## segment_sieve.hpp

### Medium tier: 64-list restructuring, retry with a block-pool allocator (2026-09-24, idea 2 from an external review, Opus 5.5, second round)

Same 64-list idea as `erat_small.hpp`'s attempts above, but backed by this class's
own block-pool allocator (`Blk`, `BLK_BYTES=1KiB`, shared with the sparse ring)
instead of a `std::vector` per list, an entry re-filed into a different (pr, phase)
slot every segment via `cross_off<PR>`'s own exit state -- structurally the sparse
ring's own pattern, just keyed by (class, phase) instead of (future segment).
Hypothesis was that pooled, promptly-recycled blocks would avoid the footprint
growth that sank the `std::vector` version.

It didn't: at N=1e12, instructions:u dropped 26.7% (1.9355T -> 1.4187T) but
cycles:u was flat (1.4595T -> 1.4558T, -0.25%, noise) because IPC fell 1.33 -> 0.97
and cache-misses:u roughly doubled (317M -> 533M); at N=1e13 a real regression
(cycles:u 19.603T -> 21.238T, +8.3%, cache-misses:u 16.10B -> 32.47B, +102%).

The decisive test: at N=1e10, where the medium population is small enough that
cache-misses:u are already near zero for BOTH versions (662K vs 290K -- med64
actually HALVES them here), instructions:u still dropped 23.8% (9.83B -> 7.49B) but
cycles:u didn't move AT ALL (9.2236B -> 9.2263B, +0.03%) -- IPC fell 1.07 -> 0.81
anyway. With cache-misses already negligible in both directions, this isolates the
real bottleneck: NOT cache/memory footprint (the failure mode both this attempt and
the original `std::vector` one were diagnosed against) but branch misprediction from
`cross_off<PR>`'s own unpredictable entry/exit switch on primes with only a few hits
per segment -- precisely what this tier's very first design note already said ruled
out the unrolled loop here, restated with a number: it's a front-end/control-flow
cost that no memory-layout change (vector or pool) can fix, because it was never a
memory problem. Reverted; if ever revisited, the switch itself -- not the entries'
storage -- is what would need to change.

Both flat tiers keep 8 bytes/prime of state (`erat::DenseState`), walked every
segment in place -- there is never a segment these primes "skip", so a bucket
would buy nothing.

These two tiers replaced (dev PC, cycles:u) a per-prime `delta[]` table tier (40
bytes/prime, capped by an L3/2 budget) plus the `ONFLY_CORRECTION` loop for
everything past that budget, both on the whole L2-sized segment: -26% at N=1e11,
-30% at N=1e12.

### Medium tier: 64-list restructuring scoped to a bounded sub-band (`med64_primes`, KEPT, 2026-09-26)

A third variant of the same 64-list idea as the two attempts above (byte marking
via `erat_small.hpp::cross_off<PR>`, grouped into 64 `(class, entry phase)` lists,
double-buffered like the second attempt) -- but this time scoped to only
`[small_limit, med64_limit)`, a bounded sub-band of the medium tier close to
`small_limit`, instead of the whole tier up to `seg_k_width`. `med64_limit =
seg_k_width * ERATOSTENES_MED64_NUM / ERATOSTENES_MED64_DEN` (env vars, default
1/8), computed and classified in `main.cpp`; processed by
`SegmentSieve::process_med64<PR>` between the small and (flat) medium tiers each
segment.

The hypothesis: both prior attempts won at N=1e12 but regressed at N=1e13 because
the *whole* medium tier's population keeps growing with N until `sqrt(N)` passes
`seg_k_width` (see `erat_small.hpp`'s own note on this saturation point) -- and the
CONFIRMED attempt's own cache-misses:u growth (20.06B->39.29B, +95.8%) tracked
almost exactly with that population's growth (75,773->152,886, +101.8%) between
those two N. A sub-band bounded well below `seg_k_width` saturates at a much
smaller N and then stays fixed population-wise, so that specific growth mechanism
shouldn't recur. Verified directly on the dev PC before writing any code: at
`seg_k_width=2,097,152`/`small_limit=24,576` (this machine's own auto-tuned
values), the band `[24576, 786432)` (3/8 fraction) holds exactly 60,221 primes at
*both* N=1e12 and N=1e13 (`pi(786432)-pi(24576)`, using the project's own binary to
count) -- fully saturated already at 1e12, while 100% of the medium tier's growth
between those two N (75,773->152,886) falls in the untouched remainder
`[786432, seg_k_width)` (15,552->92,665).

Measured (dev PC, i5-11400F -- via WSL2 this session, `perf stat
cycles:u,instructions:u,cache-misses:u,branch-misses:u`, 2 reps each, cold, natural
auto -s):

| fraction | N=1e12 cycles:u | N=1e13 cycles:u |
|---|---|---|
| baseline (NUM=0) | 1.4819T / 1.4901T | 19.256T / 19.347T |
| 1/2 | -- | +0.50% (regression) |
| 3/8 (initial guess, matching primesieve's FACTOR_ERATMEDIUM=3.0 loosely) | -3.8% / -3.7% | -0.91% / -1.21% |
| 1/4 | -- | -2.75% |
| **1/8** | **-5.5% / -5.6%** | **-4.07% / -4.17%** |
| 1/16 | -- | -3.94% (worse than 1/8 -- 1/8 is at or near the actual optimum, not just "smaller is better") |

1/8 wins cleanly at both N, unlike the two prior full-tier attempts -- this
directly confirms the population-saturation hypothesis: instructions:u dropped
substantially at 1/8 (24.728T->19.632T-ish range, ~-20%) same as before, but
cache-misses:u grew only +55.5% at 1e13 (18.0B->28.0B) instead of the CONFIRMED
attempt's +95.8%, and critically that growth no longer erases the instruction
savings the way it did before. **Kept at 1/8.**

A follow-up single, unreplicated rep suggested `small_limit` itself might have a
new optimum once med64 exists (lowering it from the standalone-tuned `L1d/2` to
`L1d/5` measured -6.67% at N=1e13 combined with 1/8, vs -4.1% for `L1d/2` combined
with 1/8) -- acted on below.

### `small_limit` re-tuned jointly with `med64_limit` (KEPT, 2026-09-26)

`small_limit`'s own divisor was made an env-var override
(`ERATOSTENES_SMALL_NUM`/`_DEN`, same style as `MED64_NUM`/`_DEN`) specifically to
follow up on the single-rep finding above with a real sweep. The reasoning for why
this needed re-tuning at all: `med64_limit`'s LOWER bound is `small_limit` itself,
so moving `small_limit` shifts which primes med64 even sees, and (per the
population-saturation argument in the entry above) med64's own optimal fraction
depends on how many primes actually land in its band -- the two cutoffs were never
independent once med64 existed, even though `small_limit` alone still measures best
at `/2` for the plain small-vs-medium split with med64 disabled (see its own
"cutoff tuning" entry above, unchanged).

**Landscape** (dev PC, `perf stat cycles:u`, N=1e12, single rep per point -- a fast
first pass before spending real time on N=1e13):

| `small_limit` | MED64=1/16 | MED64=1/8 | MED64=1/4 |
|---|---:|---:|---:|
| /2 | 1.4126T | 1.4042T | 1.4112T |
| /3 | 1.3844T | 1.3863T | 1.3988T |
| /4 | 1.3824T | 1.3813T | 1.4030T |
| /5 | 1.3705T | 1.3854T | 1.4097T |
| /6 | 1.3847T | 1.3804T | 1.4059T |
| /7 | 1.3904T | 1.3863T | 1.4287T |
| /8 | 1.4062T | 1.4106T | 1.4317T |

Clear pattern: smaller `small_limit` wants a smaller `med64` fraction too (the
per-row minimum drifts from MED64=1/8 at `small_limit=/2` toward MED64=1/16 at
`/4`-`/6`) -- consistent with med64's band needing a bounded, "right-sized"
population regardless of where its lower bound sits, not "more conversion is
always better." A refinement pass around the apparent best region (`small_limit` in
{4,5,6} x MED64 in {1/12,1/16,1/20,1/24,1/32}) found run-to-run noise of about
±1% even at a fixed config (e.g. `small=/5,med64=1/16` read 1.3705T in the first
pass and 1.3838T in the refinement pass) -- too close to call from single reps, so
the two best-looking round candidates (`small=/4,med64=1/8` and `small=/5,med64=1/16`,
statistically tied at ~1.384T average over 2 reps each) plus the single best grid
point (`small=/4,med64=1/12`, ~1.374T average over 3 reps) were compared properly.

`small=1/4, med64=1/12` won clearly at N=1e12 (3 reps, ~2.3% below the shipped
1/2+1/8 default) and was then confirmed at N=1e13 (2 reps, interleaved with the
1/2+1/8 default, `perf stat cycles:u,instructions:u,cache-misses:u,branch-misses:u`):

| metric | default (1/2, 1/8) | candidate (1/4, 1/12) | delta |
|---|---:|---:|---:|
| cycles:u | 18.904T / 19.071T | 18.629T / 18.548T | **-2.1%** |
| instructions:u | ~19.563T | ~19.070T | -2.5% |
| cache-misses:u | ~24.32B | ~24.17B | -0.6% |
| branch-misses:u | ~175.4B | ~151.4B | **-13.7%** |

No overlap between reps on any metric -- a clean win, not a trade-off (unlike the
original med64 sweep, where cache-misses:u/instructions:u moved in opposite
directions). The branch-misses:u drop is the most interesting number: `small=1/4,
med64=1/12` puts *fewer* primes in med64 (14,428 vs 20,275 at this N) even though
`small_limit` itself dropped (fewer, not more, of the highest-hit-count primes end
up in med64's own 64-list structure) -- consistent with the entry/exit branch-
misprediction concern `erat_small.hpp::cross_off`'s own comment already flags:
grouping by entry phase fixes the entry side, but the exit side still depends on
each prime's own phase alignment, and a smaller, better-sized med64 population
means less of that residual cost paid in aggregate. **Kept: `small_limit`
default moved to `L1d/4`, `med64_limit` default moved to `seg_k_width/12`.**

**Follow-up, done (2026-09-26): extended directly to N=1e13.** The N=1e12
landscape's "smaller `small_limit` wants a smaller med64 fraction, and keeps
improving" trend does NOT hold at N=1e13 -- single-rep cycles:u across the region
that looked most promising at 1e12:

| config | cycles:u (N=1e13) | vs `1/4,1/12` |
|---|---:|---:|
| `1/3, 1/10` | 18.205T | +0.25% |
| **`1/4, 1/12` (kept default)** | **18.160T** | -- |
| `1/5, 1/16` | 18.405T | +1.35% |
| `1/6, 1/16` | 18.575T | +2.29% |
| `1/6, 1/20` | 18.429T | +1.48% |
| `1/8, 1/24` | 18.393T | +1.28% |

`1/4, 1/12` is the best of everything tried, at both N -- going smaller keeps
helping at 1e12 right up until it doesn't at 1e13, the same shape as this
project's own `sparse_limit = seg_k_width/4` experiment and the original med64
fraction sweep's own 1/16 point. Confirms the kept default sits at (or very near)
the actual optimum for the N this project targets, not just the best point found
on an 1e12-only search. A finer local sweep exactly around `1/4` (e.g. med64 =
1/10, 1/11, 1/13, 1/14) was not run -- the margin over the next-best point
(`1/3,1/10`, +0.25%) is small enough that it's plausible, but this closes the
"extend to 1e13" question specifically.

### dTLB pressure at large N: investigated, ruled out (2026-09-25, external review, Opus 5.5)

Hypothesis: dTLB pressure from a thread's whole working set at large N -- the
256KiB segment array, ~1.2MB of medium-tier `DenseState` (152,886 primes * 8 bytes
at the natural N=1e13 cliff, matches the review's own estimate) walked whole every
segment, plus the sparse ring's blocks scattered across dozens of 4KiB pages -- all
live at once per thread, and with 2 threads/core sharing one STLB (true of the dev
PC, i5-11400F, 6C/12T) that's plausible to blow.

Measured before touching anything (`perf stat -e dTLB-load-misses,dTLB-loads,dTLB-
store-misses,dTLB-stores`): N=1e12 -- 11.3M/434.4B loads (0.0026%), 3.8M/281.6B
stores (0.0014%); N=1e13 -- 226.9M/5,329.7B loads (0.0043%), 40.5M/3,182.3B stores
(0.0013%). Even generously costing every one of the ~267M total dTLB misses at 1e13
at ~20-30 cycles (a full page-walk-from-cache penalty), that's ~5.3-8.0B cycles
against 19,274.6B total -- ~0.03-0.04%, nowhere near enough to matter, let alone
explain a double-digit ratio gap against primesieve. Miss rate did grow ~1.6x
relative from 1e12 to 1e13, but off a base this small that doesn't project to
anything significant by 1e14 either.

Not pursued further: no huge-pages experiment, no `madvise(MADV_HUGEPAGE)` -- the
measurement this review itself proposed as the cheap first step already closes the
question.

### Sparse tier: EratBig-style rewrite (adopted, 2026-09-24, isolated test of point 1 from an external review, Opus 5.5)

The original sparse tier (see attempts 1-5 below) was swapped for an EratBig-style
rewrite: byte marking (not bit), a mod-210 multiplier wheel (48/210 phases instead
of 8/30 -- valid because any multiplier that's a multiple of 7 lands on a composite
7 itself already crosses off in its own small-tier pass, so those phases are
redundant work here specifically, never a correctness gap), and pointer-aligned
blocks (a tail pointer landing exactly on a block boundary means "full",
primesieve's own `Bucket` trick, no per-block count field to load). Requires a
power-of-2 segment width in BYTES (see the constructor's `has_sparse` check) for the
bucket-slot math to become a shift/mask instead of a division -- `main.cpp` floors
`seg_k_width` to the nearest power of 2 whenever this tier is used.

Note: rounding the auto-computed width down to a power of 2 was already tried in
isolation for the OLD tier (attempt 7 below) and did NOT give a consistent win on
the dev PC (cache-refs got 12.9% WORSE at N=1e13) -- since that rounding is now a
hard requirement of this new tier's design, any win/loss measured for the rewrite
is the two effects bundled together, not the EratBig rewrite in isolation. This
rewrite is now the adopted design; the small/medium tiers were untouched by it.

### Sparse tier (original design, before the EratBig rewrite above): stepping-math attempts 1-5

Most segments have nothing to do for most sparse primes, so scheduling each one
into the future segment where its next hit actually falls (a fixed-size ring of
block-pooled queues) means a segment's processing only ever looks at the (few)
sparse primes actually due, not all of them. Steps forward the same
`qp*GAP_K[j] + ONFLY_CORRECTION[pr][j]` way the medium tier does -- no division by
the runtime value p anywhere in this tier -- with qp/pr/the wheel phase j packed
into one word (`erat::DenseState::qw`) alongside a `pos` relative to whichever
segment the entry is due in, 8 bytes total per live entry.

Five changes to this tier's *stepping math* were tried and reverted as net
regressions -- two predate splitting `process_sparse_bucket` into its own noinline
function: a shared-table scheme like `ONFLY_CORRECTION` (~2x slower at N=1e12 with
a third of base primes forced sparse) and an AoS relayout of its per-prime state for
locality (~2.25x slower), both measured while this whole function (dense + onfly +
sparse) was still fully inlined and suffering real register spilling -- any change
adding live variables looked catastrophic there regardless of its own merit.

- **Attempt 3** (N=1e13, natural auto -s, 72,036/227,647 base primes sparse):
  retried the shared-table scheme *after* the noinline split, on the theory that
  attempt 1's loss was purely the register-spilling confound. It wasn't -- clean
  same-session A/B via perf stat cycles:u (frequency-independent, not wall-clock):
  48.11T cycles vs 42.68T baseline, +12.7%; IPC 0.96->0.85; cache-miss rate
  9.52%->12.55%. Root cause: this scheme needs qp+pr per prime (`OnFlyPrime`, 24
  bytes padded) instead of the plain `uint64_t p` (8 bytes) `sparse_primes` held
  before, nearly tripling that array's footprint right in the tier accessed in
  pseudo-random order (via the bucket ring's intrusive list, no locality to begin
  with) -- costs more in cache pressure than the division (measured elsewhere as
  ~2.6% of this tier's cycles) saves. Register spilling was real for attempts 1-2,
  but wasn't the whole story either apparently -- or this tier's memory-footprint
  sensitivity is itself the thing that changed between N=1e12 (attempts 1-2) and
  N=1e13 (attempt 3), given how much bigger the sparse population is at the natural
  cliff vs a forced-small-N proxy. Reverted.
- **Attempt 4** (superseded by attempt 5 -- N=1e13, natural auto -s, same
  72,036/227,647 sparse split as attempt 3): same division-free goal as attempt 3,
  but stores `m` instead of adding qp/pr fields, so there's no memory-footprint
  growth to fight the division's removal with -- `wheel_index(p*m)` (a multiply
  plus a compile-time-constant divide) replaces both the old `wheel_number(k)/p`
  division *and* the extra per-prime storage attempt 3 needed. Clean same-session
  A/B via perf stat cycles:u: 41.39T vs 42.68T baseline, -3.0%; wall-clock 1011.38s
  vs 1094.00s, -7.55%; IPC 0.96->1.00; cache-miss rate flat (9.52%->9.63%,
  confirming no footprint growth this time).
- **Attempt 5** (same day): a division-by-p removal that fixes attempt 3's actual
  failure (memory footprint, not the division itself) directly, instead of attempt
  4's alternative fix (`wheel_index(p*m)` instead of the shared-table step).
  `qp=p/WHEEL_MOD` and `pr=p%WHEEL_MOD` are recomputed from p per due-check rather
  than stored -- both are divisions by the *compile-time* constant WHEEL_MOD, a
  cheap multiply-shift, not the runtime-p division being removed -- and k+j are
  packed into one word (`sparse_kj_`) instead of two, so total per-prime memory
  stays exactly what attempt 4's plain k took. Measured two ways: a
  forced-heavy-sparse proxy (N=1e12, `-s 500000`, 84% sparse, the same trick used
  elsewhere in this file to test this tier cheaply) showed a clean win -- cycles:u
  4.480T->4.112T (-8.2%), wall-clock 96.57s->89.06s (-7.8%), instructions:u down
  too (not just cycles), cache-miss rate 4.04%->3.48%. At the *natural* N=1e13
  cliff (31.6% sparse, this tier only ~17% of total cycles per a perf profile taken
  that session) the same fix only moved wall-clock 898.63s->892.09s (-0.7%) --
  small because the tier itself is still a minority of the work at this N, not
  because the fix doesn't hold up; instructions:u still dropped (42.819e9->
  42.293e9), confirming a real if modest effect here, expected to matter more as N
  grows past 1e13 and this tier's share of total cycles grows with it. **Kept.**

### Sparse tier: `process_big`/`process_sparse_bucket` split into its own noinline function

Pulled out of `sieve_and_emit` into its own function, and marked `noinline` to make
sure it stays that way even under `-O3`/`-flto`: with dense/onfly/sparse all fully
inlined into one function, perf showed real cycles going to a spilled-to-stack
reload of a loop-invariant member (`num_buckets_`) inside what's now this function --
the *number* of values simultaneously live across all three tiers was forcing
spills, not a poor choice of which value to spill. Passing values in as explicit
parameters instead of member reads didn't change anything measured, because that
doesn't reduce how many values are live at once, only where they come from. Giving
this tier its own function gives it its own register allocation scope instead, so
its live ranges stop competing with the other two tiers' for the same register
file.

Confirmed with perf stat, not just wall-clock (which turned out noisy for this
tier): at N=1e12 with a third of base primes forced sparse (`-s 500000`), cycles
dropped ~5-9% and IPC rose from 1.07 to 1.14-1.19 across repeated runs,
consistently. The flip side of a real function call is real call overhead, paid
once per segment even when this tier has nothing due -- `sieve_and_emit` skips the
call entirely when `sparse_primes` is empty for the whole run, which is what keeps
the dense-only case's numbers unchanged from before this split.

### Sparse tier design, current: fixed-size pooled blocks (attempt 6)

Replaces the idx-indexed intrusive list (`sparse_kj_`/`sparse_next_`/
`sparse_primes`, attempts 1-5 above) with fixed-size blocks of `erat::DenseState`
pulled from a pool, one queue (linked list of blocks) per ring slot -- primesieve's
own `EratBig` design. The entry itself now carries everything needed to process it
(qp/pr/j packed into `qw`, `pos` relative to whichever segment it's due in, same
layout as the dense tiers' `DenseState`), so rescheduling COPIES the entry into the
target slot's tail block instead of relinking an index -- both the read (draining a
slot's blocks front to back) and the write (appending to a tail) are sequential,
unlike the old scheme's pointer-chase over `sparse_kj_`/`sparse_next_`/
`sparse_primes` at effectively random idx values. This is NOT attempt 3's AoS
relayout (which kept the idx-array pointer-chase and only repacked its fields, and
lost to cache-footprint growth) -- here nothing is indexed by idx at all any more,
and a live entry costs exactly 8 bytes (`sizeof(DenseState)`) at any one time, less
than attempt 5's 12 bytes/active-prime (8 for `sparse_kj_` + 4 for `sparse_next_`, p
amortized via the shared `sparse_primes` array).

### `SPARSE_BLOCK_ENTRIES` tuning: 1024 vs. 128

`SPARSE_BLOCK_ENTRIES` matters more than it looks: a first pass at 1024 (8 KiB/
block, primesieve's own `EratBig` default) won cleanly at the *natural* N=1e13
cliff (cycles:u 23.16T->20.81T, -10.2%; wall-clock 506.64s->449.13s; IPC
1.25->1.41; cache-misses 22.00B->17.22B) but *lost* on this file's usual fast proxy
(N=1e12, `-s 500000`, 84% sparse forced): cycles:u 3.484T->3.775T, +8.3%;
wall-clock 75.43s->90.83s, +20.4% -- the opposite of every other attempt in this
tier's history, where the proxy and the natural N agreed on direction even when
they disagreed on magnitude.

Root cause: the proxy's tiny forced segment width needs many more ring slots
(`num_buckets_` scales with 1/`seg_k_width_`), so its ~66k active sparse primes per
chunk spread thin across ~64 slots -- ~86 live entries/slot, each getting its own
mostly-empty 1024-entry block (cache-references 84.79B, next to all of it pool
padding no prime ever occupies). The natural N=1e13 cliff has far fewer ring slots
(a much wider auto segment) and a comparable population, so its blocks stay
reasonably full and never hit this.

Shrinking to 128 (1 KiB/block) fixed the proxy without giving back the natural-N
win -- both now agree: proxy cycles:u 3.484T->3.190T (-8.4%), cache-refs
84.79B->18.49B (-78%), cache-misses 2.10B->0.258B (-87.7%); natural N=1e13 cycles:u
23.16T->20.78T (-10.3%), wall-clock 506.64s->447.74s (-11.6%), cache-refs
400.5B->295.8B (-26.1%). **Kept at 128** -- if this tier's population/ring-slot
ratio changes a lot on a future machine or N, re-check both regimes again rather
than assuming either one predicts the other for a block-size change specifically.

### Sparse tier: prefetch the next block of the chain, once per block (kept, 2026-09-27)

At E14+ the sparse tier's live state no longer fits any cache (~1.95M sparse
primes x 8 bytes = ~15.6MB per thread at E15), so draining a ring slot is a cold
streaming read from DRAM. Within a block the read is sequential and the hardware
streamer follows it -- but a slot's chain of blocks is scattered in memory (LIFO
free list), so the streamer restarts at every 1KiB block boundary and pays full
DRAM latency on the first lines of each block. `process_big()` already loads
`next_blk` at the start of each block, ~128 entries of work ahead, so it now
issues L2 prefetches (`prefetcht1`) for the whole next block right there. Unlike
attempt 8 and the medium-tier prefetch attempts (per-hit, where the per-hit cost
ate the gain), this is once per block: ~0.1 instructions per entry.

Measured (dev PC, `perf stat cycles:u`, OLD/NEW/NEW/OLD interleaved, cooldown
between runs):

| regime | OLD cycles:u | NEW cycles:u | delta |
|---|---:|---:|---:|
| N=1e13 `-s 1000000` (204,647 sparse, ~20MB total > 12MB L3: DRAM-bound) | 30.051T / 30.401T | 27.936T / 27.918T | **-7.6%** |
| N=1e13 natural (72,036 sparse, fits L3) | 18.438T / 18.588T | 18.363T / 18.372T | **-0.8%** |

instructions:u +0.14% / +0.015% (the prefetches themselves), IPC 1.10 -> 1.19 in
the DRAM-bound regime -- the signature of hidden memory latency, not removed
work. pi(N) exact in every run. Proxy chosen so blocks are full (~47 per slot),
like natural E14+, unlike the `-s 500000` N=1e12 proxy's mostly-empty slots.
Expected to matter most at natural E14/E15 on the server, where the sparse state
exceeds L3 without forcing -- not yet measured there.

**Follow-up: `prefetchnta` instead of `prefetcht1` (tried, reverted, 2026-09-30).**
At 1e15 about half of `process_big` stalls on the `s[pos] |= mask` RMW into the
segment (see the i5-13500 entry in main.cpp below). That is the same signature
the medium tier had before SoA + NTA (-12% at 1e14): segment misses caused by
another stream. So this tried pulling the bucket blocks in with the NTA hint
(`ERATOSTENES_BIG_NTA` knob, a runtime branch once per block). Dev PC, same
proxy as above (1e13 `-s 1000000`, 204,647 sparse, 10% tail), same binary with
the knob 0/1, order 0 1 1 0 (plus a `c5ec94f` control), counts exact:

| | NTA=0 | NTA=1 | change |
|---|---:|---:|---:|
| cycles:u | 3591.9 / 3612.6G | 3828.1 / 3814.7G | **+6.1%** |
| `mem_load_retired.l2_miss` | 868 / 883M | 1149 / 1179M | +33% |
| `mem_load_retired.l3_miss` | 267 / 270M | 383 / 368M | +40% |
| `l2_lines_out.non_silent` | 11.88 / 11.88G | 10.30 / 10.36G | -13% |

Fewer L2 writebacks, as intended, but many more misses. Drained blocks go back to
the LIFO free list and are reused right away as the write target of other slots.
With `prefetcht1` that block is still in L2 when it's rewritten, so the read
stream turns into the write stream in place. With NTA it's gone, and every
rewrite pays an RFO from L3/DRAM. That doesn't depend on the machine, so the
natural 1e14 tail was stopped and the knob removed. Side note: even at NTA=0
the knob's binary was +3.5% cycles / +1.5% instructions vs the control. The
second prefetch loop changed register allocation in the hit loop (1-2 more
instructions per hit): one more reason to keep `process_big`'s block-level code
minimal. Any future fix for the segment RMW stall should not break the "drained
block is the next write target, still in L2" property.

### Sparse tier attempts 7-10 (all tried, reverted)

- **Attempt 7**: `schedule_sparse`'s ring-slot placement divides by `seg_k_width_`,
  a runtime value -- rounding `main.cpp`'s *auto*-computed width down to the nearest
  power of 2 (leaving an explicit `-s` exactly as given) turns that into a shift.
  Measured at both natural N this tier's history already tracks: N=1e12 cycles:u
  1.6571T->1.6646T (+0.45%, noise-level), cache-refs 14.30B->13.45B (-5.9%); N=1e13
  cycles:u 20.776T->20.818T (+0.2%, also noise-level) but cache-refs
  295.8B->333.9B (+12.9%) and cache-misses 16.20B->18.04B (+11.4%) -- worse, and in
  the OPPOSITE direction from N=1e12. Net: no consistent win on the metric this
  tier's history actually trusts (cycles:u flat both times, within noise), and the
  cache impact of rounding the *width itself* down doesn't even agree in sign
  between the two N tried, let alone offset what the shift saves. Reverted; the
  division itself was never shown to cost anything on its own here, only entangled
  with a width change that didn't pay for itself.
- **Attempt 8** (N=1e12 forced-sparse proxy, `-s 500000`, 84% sparse, two reps):
  software-prefetched `s[(it+4)->pos]` in `process_big()`'s hit loop, on the theory
  that it's this tier's one genuinely scattered access and the target is already
  knowable (it+N's `pos` was written when it was scheduled last segment, so it's
  valid data, not something this iteration has to compute first). Regressed on
  both metrics, both reps: cycles:u 2.2092T->2.2858T (+3.5%) then 2.2262T->2.2603T
  (+1.5%); cache-misses:u 377.1M->1182.3M (+213%!) then 676.9M->777.3M (+14.8%) --
  worse in the SAME direction both times, unlike attempt 7's sign flip, so not
  noise. Root cause (inferred, not independently confirmed): the segment array is
  L2-sized by design, so a byte scattered across it isn't the long-latency miss a
  software prefetch usually hides -- the extra prefetch is then just added memory
  traffic, and with 12 threads all issuing it at once, contends for MSHRs/L2
  bandwidth instead of hiding anything. Reverted.
- **Attempt 9** (same forced-sparse proxy, two reps): 4-way software-pipelined
  `process_big()`'s hit loop -- load/table-lookup all 4 entries first, then all 4
  writes, then all 4 reschedules (pushes kept in original 0,1,2,3 order for
  block-chain correctness). `perf annotate` on the scalar version showed ~65% of
  this function's own cycles in one dependent chain per hit (load DenseState ->
  index `big::TABLE` -> compute next pos/slot -> check `head_[slot]`); unlike
  `cross_off_medium`'s while loop (data-dependent trip count, why ITS 4-lane
  attempt lost), this for loop always does exactly one pass per entry, so no lane
  can finish early and idle waiting on the others -- that specific failure mode
  genuinely doesn't apply here. IPC did improve (1.33-1.34->1.36-1.37) and
  cache-misses:u didn't get worse (757.7M-835.0M -> 764.7M-773.9M, flat to better)
  -- the ILP hypothesis wasn't wrong. But instructions:u rose 3.1% (3.0413T->
  3.1366T, both reps identically -- the remainder loop for block sizes not a
  multiple of 4, plus extra live registers/moves from unrolling) and that
  outweighed the ILP gain: cycles:u 2.2791T->2.3050T (+1.1%) then 2.2705T->
  2.2960T (+1.1%) -- small but consistent both reps, not noise. Reverted; if ever
  revisited, the remainder-loop overhead (not the pipelining idea itself) is the
  part that would need to shrink, e.g. by only pipelining when a block is known
  full-size (it's the LAST block in a chain that's ever partial).
- **Attempt 10**: replaced `head_`/`tail_`'s modular ring (power-of-2 size, 2x
  margin, `(cur_segment_+ahead) & bmask` on every push) with a SLIDING WINDOW,
  matching how primesieve's own `EratBig::crossOff` does it (read from its actual
  v12.7 source, not from memory -- `include/primesieve/Bucket.hpp`,
  `src/EratBig.cpp`): slot 0 always means "due this segment", a push uses `ahead`
  directly (no AND-mask, no absolute segment counter), and once slot 0 is fully
  drained the whole window shifts left by one (`std::copy`) instead. On the usual
  forced-sparse proxy (N=1e12, `-s 500000`, 84% sparse, two reps) this looked like a
  real if modest win: cycles:u 2.2689T->2.2632T (-0.25%) then 2.2766T->2.2691T
  (-0.33%), instructions:u down 1.79% both reps. But at the *natural* N=1e13 cliff
  it reversed: cycles:u 19.738T->20.118T (+1.93%), cache-misses:u 20.49B->21.67B
  (+5.8%), cache-references:u +5.0% -- a real regression, not noise. Root cause:
  the shift is unconditional, paid on EVERY `process_big()` call regardless of
  whether anything was actually due that segment -- and at natural N, sparse
  density is low (this tier was only ~2.66% of total cycles at 1e13), so most
  calls have nothing due at all. The old modular ring's equivalent no-op case was
  one `while (head_[slot])` check against false -- cheap and highly predictable.
  The forced-sparse proxy hides this because it's deliberately built to make
  nearly every segment have something due, so the shift's fixed cost gets
  amortized against real work almost every call there -- the exact same
  proxy-vs-natural sign flip attempt 6 already hit once before in this tier's
  history, which is why both are always checked here before keeping anything.
  Reverted; if ever revisited, the shift would need to be skipped (or made cheaper
  than a full-window copy) on segments where the window is already all-nullptr,
  which is the common case at realistic N.

### Attempt 11: shrinking the live entry from 8 to 7 bytes (tried, reverted, 2026-09-27)

Motivated by a per-tier profile (external review, Opus 5.5, see the `erat_small.hpp`
"med64: mod-210 stepping" entry above) diagnosing this tier as memory-bound, not
instruction-bound -- the reasoning being that this project's own E14 ratio gap
against primesieve (1.17x, vs. 1.03-1.09x through 1e13) coincides with this tier's
population, which keeps growing past `seg_k_width` saturation, becoming a bigger
share of total work. If bytes-moved is really what matters here, shrinking the
8-byte `erat::DenseState` this tier reuses seemed like the one lever that targets
the actual bottleneck instead of instructions.

The bit budget was checked before writing any code: `qw` (the existing
`(qp<<9)|idx` packing) needs ~30 of its 32 bits for the E15 target, no room to
spare there. `pos` is the one field with real slack -- a segment-relative byte
offset, needing only `log2(seg_k_width/8)` bits (18 on the dev PC's 256KiB
segment, more on a bigger-L2 machine) out of the 32 it's given. Total real need:
idx(9) + qp(21) + pos(~18-20) ≈ 50 bits, fitting a 7-byte (56-bit) packed value --
but NOT 6 bytes as first hoped (pos alone already exceeds 16 bits on real
hardware, ruling out a plain `uint16_t` field).

7 isn't a power of 2, which collides with a harder constraint than expected:
`std::aligned_alloc`'s alignment argument (this tier's `BLK_BYTES=1024`) must
itself be a power of 2, and the pool's "tail pointer lands exactly on a block
boundary = full" trick (primesieve's own Bucket design, no count field to load)
only works when the entry size evenly divides that power-of-2 block size --
impossible for any non-power-of-2 entry size, not just impractical. Implemented
anyway, accepting the trade: a packed 7-byte `SparseEntry` (byte-precise
pack/unpack, no unaligned wide loads) replacing `erat::DenseState` for this tier
only, and an explicit per-slot `tail_blk_`/`tail_count_` pair replacing the
pointer-alignment full-check (both already touched every push, so not a
logically new memory access, just two explicit fields instead of one implicit
one). `ENTRIES_PER_BLOCK` recomputed as `(1024-16)/7 = 144` exactly.

Correctness held at every scale tried: `make test` (which forces `-s 64`,
essentially all-sparse), a forced-84%-sparse proxy at N=1e12 (`-s 500000`,
pi(N) exact), and the natural N=1e13 cliff (31.6% sparse, pi(N)=346,065,536,839
exact). Performance was a clear, reproducible REGRESSION, not a wash -- measured
on the forced-sparse proxy (dev PC, i5-11400F, `perf stat cycles:u,
instructions:u,cache-misses:u`, N=1e12 `-s 500000`, 2 interleaved reps each,
cooldown between):

| metric | baseline (8-byte) | 7-byte packed | delta |
|---|---:|---:|---:|
| cycles:u | 2.2057T (avg) | 2.4354T (avg) | **+10.4%** |
| instructions:u | 3.181300T (identical both reps) | 3.714951T (identical both reps) | **+16.8%**, real and deterministic |
| cache-misses:u | 243M / 613M (2.5x spread between its OWN 2 reps) | 354M / 276M | too noisy on this metric to read directionally |

`instructions:u` being bit-identical across repeats on each side rules out
noise for that number specifically: this genuinely executes ~17% MORE
instructions to do the same work, the opposite of the intended trade
(fewer bytes moved, even at the cost of a few more instructions to pack/unpack
them). Root cause not fully isolated via `perf annotate` this session, but the
mechanism most consistent with the numbers: `SparseEntry`'s 1-byte alignment
means consecutive entries sit at non-4-byte-aligned, 7-byte-strided offsets --
neither a natural machine word size nor a power-of-2 stride, which likely
defeats both (a) the compiler's ability to lower `qw()`/`pos()`/`make()`'s
`memcpy`-based accessors into single clean load/store instructions (hence the
instruction blowup) and (b) the hardware prefetcher's ability to recognize a
7-byte-strided access pattern the way it does the old 8-byte one (a plausible,
though not directly confirmed, explanation for cache-misses trending worse in
one of the two reps). Both effects would push in the same direction actually
observed: more instructions AND no cache-locality win to show for it.

**Reverted, code removed** (not kept behind a flag -- confirmed dead end, not a
pending tune). **Why:** this closes the "just shrink the struct" version of the
memory-bound-tier hypothesis specifically -- the profile's underlying diagnosis
(this tier is memory-bound, and its growing E14+ population plausibly explains
the widening ratio gap) is not itself refuted, only this one proposed fix for
it. **How to apply:** don't re-propose a sub-8-byte packed entry for this tier
without a fundamentally different mechanism for AVOIDING the alignment/prefetch
penalty a non-power-of-2 stride incurs (e.g., a byte-aligned-but-vectorized
bulk pack/unpack across several entries at once, or restructuring to a
struct-of-arrays layout instead of packing one entry tighter) -- naive
byte-level packing of a single entry measured as a net loss on every metric
that wasn't too noisy to read. The E14 ratio gap itself remains open; the next
angle isn't a smaller entry, it's a different one entirely.

### Segment processed in parts for the sparse tier (tried, reverted, 2026-09-29)

**Where the 1e14 server gap sits.** i5-13500, P-cores (`--cpuset-cpus=0-11 -t
12`), 1% tail, `perf record cycles:u` per function for both programs, eratostenes'
shares scaled by the full-run ratio (3767.6s / 3349.9s = 1.125x; the 1% tail's own
1.25x wall is the idle artifact, CPU time was identical, 458.68s each), primesieve =
100:

| tier | eratostenes | primesieve |
|---|---:|---:|
| small (< 6.1k / < 9.8k) | 20.2 | EratSmall 22.9 |
| med64 (6.1k-349k) | 33.0 | EratMedium (9.8k-786k) 31.9 |
| medium (349k-2.1M) | 24.4 | (EratMedium + EratBig) |
| sparse (>= 2.1M) | 30.3 | EratBig (>= 786k) 42.0 |
| presieve | 2.5 | 1.8 |

Split by prime band with a hits model (hits in [a, b] ~ ln ln b - ln ln a): small
is at parity; the whole gap is in primes >= 6k -- med64 ~+8, sparse ~+5 (~20%
dearer per hit than EratBig), medium ~0. TopDown (same run): of the excess slots,
57% backend, 37% bad speculation, 11% frontend; we retire slightly *less* than
primesieve. Since the sparse cutoff 1/2, mispredicts explain under a third of the
gap.

**Each tier's response to the segment width**, same primes per tier (29138 /
125672 / 508968; 256KiB run with `ERATOSTENES_MED64_DEN=6` and sparse 1/1 to
match), 3% tail, 2 reps each, reps within ~1%:

| tier | 512KiB | 256KiB | change |
|---|---:|---:|---:|
| sparse | 1517G | 1375G | -9.4% |
| small | 1038G | 961G | -7.4% |
| presieve | 124G | 105G | -16% |
| med64 | 1549G | 1695G | +9.4% |
| medium | 1202G | 1732G | +44% |
| total | 5541G | 5980G | +7.9% |

(Dev PC, same test: med64 +3%, medium +22%, sparse within its own ±10% noise; no
tier clearly better at 256KiB there.)

**Tried:** keep the 512KiB segment for med64/medium (per-call costs) but process it
in `ERATOSTENES_SPARSE_PARTS` parts for the rest -- per part: presieve + small
sub-blocks, then that part's sparse bucket (ring at part granularity), med64 and
medium over the whole segment afterwards. Correct (make test; forced sparse regime
at 3e9..1e11 for 1/2/4/8 parts vs primecount; 1e14 0.1% tail; a 2-part `.db`).
Server, same session, 3% tail, 2 reps each:

| | cycles:u | wall |
|---|---:|---:|
| old (`fe6260a`) | 5473 / 5570G | 135.09 / 139.47s |
| parts=1 | 5687 / 5694G (+3.1%) | 141.95 / 141.73s |
| parts=2 | 5478 / 5499G (-0.6%) | 136.88 / 137.82s |

parts=2 only won back what the restructuring itself cost at parts=1, most likely
the tier order (sparse moved from last, right before extraction, to right after
small). The 256KiB tiers' gain comes from the smaller total footprint, not from the
processing order, so splitting doesn't capture it. Dev PC: parts=2 +4%. Reverted.

**Method note: check the binary layout before trusting an A/B.** The first server
run compared parts=1/2 within the new binary only and showed -3.3%; against the old
one it was ~0. The restructured `sieve_and_emit` had also changed GCC's inlining:
`cross_off<PR>` no longer inlined into 6 of 8 `process_med64<PR>`,
`cross_off_medium<0..3>` and `Presieve::fill` inlined into `sieve_chunk`. Pinning
it back (`always_inline`/`noinline`) restored the hot layout but not the +3.1% (so
it wasn't the cause), and shifted other, colder inlining in turn, so it was dropped
with the rest. Before an A/B of a structural change: `nm -C -S` both binaries and
diff the hot functions, and always keep the previous commit's binary as a control
in the same session.

### Sparse tier: `process_big` loads per hit, ~12 -> 5 (kept, 2026-09-29)

The disassembly of `process_big`'s hit loop showed ~12 loads per hit on a loop that
is load-port bound (3 ports on Golden Cove, 2 on Gracemont): `DenseState` read as
two fields, `big::TABLE`'s row as 4 byte loads (mask, dm, corr, next), three loop
constants spilled to the stack and reloaded every iteration (spilled around the
inlined block-allocation path), `tail_.data()` reloaded from `this` every hit (the
`s[pos]` byte store may alias anything), and the new entry assembled through
`vmovd`/`vpinsrd`. Changes:

- `big::TABLE64`: the same rows packed into one `uint64_t` (mask | dm << 8 |
  corr << 16 | next << 32), one load per row -- the trick `cross_off_medium`'s
  `PACK210` already won with;
- the entry read and written as one 8-byte load/store (`memcpy`);
- `tail_.data()` hoisted into a local;
- the new-block path moved out of line (`new_block`, noinline), with the null
  check folded into the block-boundary check (null & 1023 == 0).

Result: 5 loads per hit (entry, row, `s[pos]`, `tails[slot]`, one remaining stack
reload of `modsb`), no vector-domain crossing. `nm`: only `process_big` changed
size. Correct: `make test`, and the A/B rows in the presieve entry above.

Dev PC: neutral, as expected -- on the i5-11400F the sparse cutoff is 1/1 and there
are 0 sparse primes up to 1e13 (1e13 tail: 1831.6G vs 1833.3G for compact
presieve alone; 1e12 1208.3G vs 1211.0G). This change targets the i5-13500, where
`process_big` is ~30% of cycles at 1e14 and ~39% at 1e15. Server A/B pending.

### med64: re-filing without `std::vector::push_back` (tried, reverted, 2026-09-29)

Same audit, applied to `process_med64`: after every `cross_off_checked` call,
`push_back` reloaded `_M_finish`/`_M_end_of_storage` through `this` (the byte
stores may alias them), compared them, and built the entry with
`vmovd`/`vpinsrd`. Replaced the 128 med64 vectors with a minimal POD buffer:
before each class, every output list gets room for the class's whole population
(worst case: all entries leave on one phase), then the loop writes through 8 local
tail pointers (one per exit phase) with a single 8-byte store and no capacity
check. Per-call epilogue: 1 load instead of 3. `Presieve::fill` had to be pinned
`noinline` (GCC started inlining it into `sieve_chunk`); growth path out of line.

Correct (`make test`, pi(1e12)). Dev PC, `perf stat cycles:u`, 3 interleaved reps
against `8b0174e`:

| | 8b0174e | no push_back | delta |
|---|---:|---:|---:|
| 1e13, 10% tail, cycles:u | 1856.5G | 1859.9G | +0.2% |
| 1e13, 10% tail, instructions:u | 1675.3G | 1661.8G | -0.8% |
| 1e12 full, cycles:u | 1211.3G | 1213.8G | +0.2% |
| 1e12 full, instructions:u | 1283.3G | 1269.8G | -1.05% |

~1% fewer instructions, cycles flat (within the ~±1% spread of this test): the
per-call epilogue isn't on med64's critical path -- the tier is bound by its
byte stores and the one exit mispredict per call, not by the loads around the
re-file. Reverted. Not tried on the server, where med64 is a bigger share (~33 of
100 at 1e14) and the E-cores have 2 load ports; if revisited, that's the one
place it could still show.

Follow-up (2026-10-01, on `cross_off_checked210`, tried twice, both reverted).
Context: at `-t 1` eratostenes ran +8.8% instructions vs primesieve 12.7 in the
small+medium tiers at 1e11 (78.3G vs 71.6G total). The hit loop is already 4
instructions per hit like `EratMedium`; the excess is per call (~40 instructions:
state decode, q2..q10, the switch's range check, a per-phase stub loading W into
3 registers, then one shared tail that recomputes the list address from W, the
`push_back` and a `bytes_needed` reload) vs ~10-20 in `EratMedium`.
1. Exit through a per-phase callback (`exit.template operator()<W>(i)`, W a
   compile-time constant) with `std::vector::push_back`: GCC outlined
   `emplace_back` into a real call per exit (48 copies were too big to inline):
   +2.4% instructions, +3.7% cycles:u at 1e11 `-t 1`, +1.3% on the 1e12 10% tail.
2. Same exit with raw begin/end/capacity pointer arrays (inline pointer bump at
   a constant offset, growth out of line) and `__builtin_unreachable()` for the
   switch range: instructions -1.0..-1.7% as predicted (~7 per call), cycles:u
   1e10 `-t 1` -1.3%, 1e11 `-t 1` +0.1%, 1e11 `-t 12` -0.2%, 1e12 10% tail -0.45%
   (3/3/2 interleaved reps), for `process_med64<PR>` growing 630 -> 2241 asm lines
   each.

Same conclusion as above: the per-call bookkeeping runs in the shadow of the
store-bound hit loop. The instruction gap vs primesieve is mostly free: at 1e11
`-t 1` it costs ~1.5% in cycles (46.2G vs 45.5G), so what's left there has to
come from fewer stores or mispredicts, not fewer bookkeeping instructions.

### med64: `prefetchnta` on the state stream (kept, 2026-09-29)

Idea: after the medium tier's SoA + NTA win, med64's double-buffered state is the
largest non-segment stream left through L2 -- ~29k primes x 8 bytes read from
`m64_cur_` and rewritten into `m64_nxt_` every segment (~2 x 233 KB per thread
from 5e12 up, dirty lines), the same mechanism that flushed the segment out of L2
in the medium tier. Change: `prefetchnta` 32 entries ahead once per entry in
`process_med64` (read side only; NT stores into 8 interleaved `m64_nxt_` streams
per class were ruled out up front, too many for the write-combining buffers),
behind an `ERATOSTENES_MED64_NTA` knob (now default on, `=0` turns it off). `make test` green with it on.

Dev PC, `perf stat cycles:u`, same binary with the knob 0/1 (plus the `904317d`
binary as control), counts exact everywhere:

| | control | NTA=0 | NTA=1 |
|---|---:|---:|---:|
| 1e12 full (2 reps) | 1202.0G | 1204.1G | 1208.1G (+0.5%) |
| 1e13, 10% tail (2 reps) | 1720.1G | 1719.7G | 1710.2G (-0.6%) |
| 1e14, 5% tail (ABBA x2, idle PC) | | 12619G / 280.3s | 12245G / 272.8s (-3.0%) |

The 1e14 -3% did not hold up: same-config reps spread up to 7% (NTA=0 12218G to
13139G), 2 of 4 pairs went to NTA, t ~ 1.5. Settled with cache counters (4 GP
events, no multiplexing, 1e14 5% tail, order 0 1 1 0):

| | NTA=0 | NTA=1 | NTA=1 | NTA=0 |
|---|---:|---:|---:|---:|
| cycles:u | 12617G | 12123G | 12016G | 12010G |
| `mem_load_retired.l2_miss` | 256.3e9 | 243.0e9 | 246.2e9 | 246.1e9 |
| `mem_load_retired.l3_miss` | 1.53e9 | 0.86e9 | 0.72e9 | 0.75e9 |
| `l2_lines_out.non_silent` | 299.8e9 | 286.7e9 | 290.6e9 | 289.1e9 |

The second NTA=0 run is indistinguishable from both NTA=1 runs on every counter:
in a "good" run the prefetch doesn't change L2 misses or writebacks, so the
hypothesized mechanism (less L2 pollution by the state stream) is not what's
happening. What differs is the slow runs: pooling all 12 clean 1e14 runs, NTA=0
12517G / 277.3s vs NTA=1 12187G / 271.0s (**-2.6% cycles, -2.3% wall**), NTA=1
lower in 28 of 36 cross-pairs (p ~ 0.09 two-sided), and the three slowest runs
are all NTA=0 -- the slow ones carry ~2x the L3 misses. So NTA looks like it
makes the tier less sensitive to a bad memory state rather than faster in a good
one; not proven.

**Full runs vs README.md#benchmarks (dev PC, best of N, NTA=1):**

| N | NTA=1 (best of) | README best of 7 | change |
|---|---:|---:|---:|
| 1e10 | 0.17s (5) | 0.17s | = |
| 1e11 | 2.04s (5) | 2.04s | = |
| 1e12 | 24.78s (3) | 24.88s | -0.4% |
| 1e13 | 322.55s (2; other rep 324.16s) | 326.03s | **-1.1%** |

Both 1e13 reps beat the README's best of 7. **Kept, default on, no gate**: no
measurable wall cost at 1e10-1e12 (the +0.5% cycles:u at 1e12 in the first A/B
is within that test's spread). Pending: full 1e14 on the dev PC (README
4530.74s) and the server, where med64 is ~33 of 100 at 1e14.

Method note: on the dev PC a single 1e14 5% tail can land ~5% slow with no code
change, in `cycles:u` too (12 threads + HT: cycles:u is not immune to machine
state). Pair A/B tails with counters that measure the mechanism, and with
best-of-N full runs against the README.

### Sparse tier: mod-2310 multiplier wheel (kept, 2026-09-30)

Idea: every prime up to 163 is presieved (`PRESIEVE_GROUPS`), 11 included, so a
sparse hit `p*m` with `11 | m` re-marks a composite the presieve pattern already
has -- exactly the argument that took the multiplier wheel from mod 30 to mod 210
for 7. Stepping `m` through the residues coprime to 2310 instead of 210:
480/2310 = 0.2078 vs 48/210 = 0.2286 candidates per unit of `m`, **-9.1% sparse
hits**, each costing what a mod-210 hit costs. primesieve's EratBig stays on
mod 210, so this is ground it doesn't cover.

The 2026-09-25 entry
([`cross_off_medium`: mod-2310 stepping, considered, not implemented](#cross_off_medium-mod-2310-stepping-considered-not-implemented-2026-09-25-external-review-opus-55))
rejected the sparse half for two reasons that no longer hold:

- **`qp` bits.** Back then the entry was read as `DenseState` fields, `qw` a
  `uint32_t` with 9 bits of (class, phase), and 12 bits would have left `qp` 20
  bits (< isqrt(1e15)/30). Since the 5-loads-per-hit rewrite (2026-09-29) the entry
  is ONE 64-bit word and `pos` only needs log2(segment bytes) = 19 bits of the 32 it
  had. New packing: `idx` (ri*480 + w) bits 0-11, `pos` 12-35 (24 bits, checked in
  the constructor: log2(segment bytes) <= 24), `qp` 36-63 (28 bits, up to
  p ~ 8e9, far past isqrt(1e16) = 1e8; also checked).
- **Table size.** 8 x 480 rows at 8 bytes is 30 KiB. At 4 bytes
  (`mask | dm << 8 | corr << 16 | next << 20`; dm <= 14, corr <= 14, next < 3840)
  it's 15 KiB -- `big::TABLE2310`, generated and range-checked at compile time.

First version used 16-bit rows (7.5 KiB: dm/2 in 3 bits plus a wrap bit, next
index computed as `idx + 1 - wrap * 480`). The wrap arithmetic cost 6 more
instructions per hit (43 vs 37, objdump), instructions:u went **up** 2.0% despite
the fewer hits, and cycles:u only moved -2.1% mean / -2.4% median. Storing the next
index in the row (32-bit table) brings the hit loop to 38 instructions vs mod-210's
37. That's the version kept.

Implementation: `process_big<bool W2310>` (the mod-210 loop is the `false`
instantiation, unchanged), a mod-2310 branch in the sparse activation
(`M2310`/`NEXT_W2310`, same t/w decomposition), ring sizing with max(dm) = 14.
Default on; `ERATOSTENES_BIG_2310=0` goes back to mod-210 in the same binary (A/B).

Correctness: pi(N) equal to primecount at 1e6, 123456789, 1e10, 3e11, 1e12, each
with the sparse tier forced (`-s 100000`, `-t 3 -s 500000`, `-t 5 -s 2000000`);
tails [1e15 - 1e10, 1e15] and [1e16 - 1e10, 1e16] (the latter with `qp` past 2^20)
equal between both wheels and to primecount's difference; `make test` with the knob
on and off; full 1e14 = 3,204,941,750,802.

Dev PC (i5-11400F, 12 threads, 1.66M sparse primes at 1e15, cutoff 1/1), same binary,
knob 0 vs 1, ABBA, `perf stat`:

| test | cycles:u mean | cycles:u min | instructions:u | wall mean | wall min |
|---|---:|---:|---:|---:|---:|
| 1e15 0.1% tail, 3 pairs, `TAIL_CHUNKS_PER_THREAD=4` | -5.1% | -4.2% | -2.5% | -5.1% | -4.2% |
| 1e15 1% tail, 1 pair, `TAIL_CHUNKS_PER_THREAD=8` | -7.7% | -6.3% | -2.5% | -9.2% | -8.9% |

All three 0.1% pairs agree (-6.0/-4.1/-5.2%). The 1% tail gains more than -9.1% of
the sparse tier alone could explain; L3 misses fall 32% there (4.95G -> 3.37G): the
bucket stream is ~16 bytes of traffic per hit shared by 12 threads, and cutting 9%
of it relieves the other tiers too. One pair only -- the min-vs-min figures are the
conservative ones.

Full runs: 1e10-1e13 have no sparse primes on the dev PC, and HEAD vs the new
binary interleaved (HEAD, new, new, HEAD) tie at every N (1e12 23.65/23.70 vs
23.74/23.76s, 1e13 309.27/313.35 vs 309.72/311.96s) -- the code-layout change costs
nothing. 1e14 (368,632 sparse primes): **4185.20s** vs the README's 4530.74s
(-7.6%, 0.72x primesieve); that README row predates the other 2026-09-29/30
changes, which are worth ~4% at 1e13, so ~3.5% of it is this.

Server (i5-13500): `process_big` is ~39% of cycles at 1e15 there, so -3.5% or more
is the expectation at 1e15; ~1% at 1e14, where few primes are sparse. A/B pending.

Not extended to the other mod-210 tiers yet: medium primes have 1-3 hits per segment
and their cost is dominated by the loop exit mispredict, not the hits, and its
`(pos << 6) | w` state has no room for 480 phases; med64's fully unrolled 48-case
`switch` would become 480 cases (~60 KB of code) and 3840 lists.

## gap_encoding.hpp

### Gap encoding: wheel-index deltas

**Kept, 2026-09-28 (`.db` format_version 2).** Version 1 stored each gap as one
byte `delta/2`. Its distribution carries the residue-class structure of the mod-30
wheel (from a given residue only some gaps are possible, and multiples of 6 are
favored), which zstd can't exploit without knowing each prime's residue, so it
pays for it as entropy. Counting the gap in wheel indices instead (how many
candidates coprime to 30 the next prime is ahead) gives a near-geometric,
near-independent stream: its order-0 entropy equals H(p)/p with p = primes per
wheel candidate, the bound for an iid bitmap, and zstd's Huffman stage codes it
within ~1%.

Measured first with a standalone program on real 3e7-wide windows of primes
(zstd blocks of 65536 bytes, bits/prime):

| N | gap/2, zstd 1 | wheel gap, zstd 1 | wheel gap, H0 | raw mod-30 bitmap, zstd 1 |
|---|---:|---:|---:|---:|
| 1e10 | 4.877 | 3.971 | 3.934 | 4.007 |
| 1e12 | 5.156 | 4.263 | 4.220 | 4.323 |
| 1e13 | 5.273 | 4.391 | 4.341 | 4.488 |
| 1e15 | 5.484 | 4.619 | 4.563 | 4.841 |

Compressing the sieve's own bitmap (no extraction at all) is worse than the wheel
gap under zstd, and a custom arithmetic coder could win at most the ~1% gap to
H0, so neither was pursued.

End to end (dev PC, `.db` written to the WSL ext4 home, interleaved
base / new(3) / new(1) / new(1) / new(3) / base, counts exact in all runs):

| N | v1 gap/2, zstd 3 | wheel gap, zstd 3 | wheel gap, zstd 1 |
|---|---:|---:|---:|
| 1e11 size | 2.596 GB | 2.182 GB (-15.9%) | 2.108 GB (-18.8%) |
| 1e11 time | 20.24 / 27.06s | 25.46 / 22.66s | 13.60 / 13.00s |
| 1e12 size | 24.13 GB (5.13 bits/prime) | 20.94 GB (-13.2%) | 20.23 GB (-16.2%, 4.30 bits/prime) |
| 1e12 time | 290.41 / 340.24s | 270.07 / 277.50s | 235.67 / 200.54s |

1e12, means: -13% time at level 3, -31% at level 1, which became the default (see
[arg_parser.hpp](#--zstd-level-default-1-kept-2026-09-28)). At 1e15 the gap is the
same ~16%: ~3 TB less on the planned 22 TB disk.

Format: byte b in 1..255 is the wheel gap; 0 escapes to a raw 4-byte integer
delta, used when prev is off the wheel (2, 3, 5) and for wheel gaps over 255
(integer gaps over ~960, none below 1e15). `nth_prime` refuses any other
`format_version`, since decoding a v1 block as v2 would silently return wrong
primes.

### `.db` extraction in wheel indices: `GapBlockSink::write_k` (kept, 2026-10-01)

Profiling 1e10 `-o x.db -t 12` put 51% of all cycles in
`sieve_chunk<GapBlockSink>`, i.e. extraction plus gap encoding, more than the
whole sieve. The sieve knows each prime's wheel index k (k_low + bit
position), but the extraction loop turned it into a value (`q * 30 + R[r]`)
and `encode_gap` turned that value, and the previous prime's, back into wheel
indices (division by 30, modulo, `WHEEL_POS` lookup) only to subtract them.
`sieve_and_emit` now hands k straight to sinks that have `write_k` (checked
with a `requires` expression); the gap is `k - last_k_`, and the value is only
rebuilt for a block's first prime and for escapes. Same bytes on disk: `-t 1`
`.db` files at 1e9, 1e8 with 1000-prime blocks and 1e5 with 7-prime blocks are
byte-identical to the old binary's.

Dev PC, 3 interleaved reps: 1e10 `-t 12` instructions:u 41.10G -> 30.14G
(-26.7%), cycles:u 23.54G -> 19.35G (-17.8%). Wall-clock on the WSL disk didn't
move (1.1-1.6 s both, I/O-bound there); writing to tmpfs it did: `-t 12`
0.70 -> 0.63 s (-10%), `-t 2` 1.67 -> 1.34 s (-20%). So the win shows wherever
the CPU, not the disk, is the limit (few cores, fast storage).

## sqlite_prime_store.hpp

### `journal_mode=OFF` for the bulk load (tried, reverted, 2026-10-01)

With `journal_mode=WAL` every page is written twice: into the `-wal` file, then read
back and copied into the `.db` at each checkpoint. After the wheel-index gaps cut
the server's 1e13 `.db` from 216 to 190 GB with no clear change in time (~930-950s,
~200-250 MB/s), that double write became the main suspect. `ERATOSTENES_DB_JOURNAL=off`
(experimental knob, default unchanged) sets `journal_mode=OFF` + `synchronous=OFF`
and skips the final checkpoint; a crash mid-run corrupts the file, but a partial
`.db` is useless anyway. The knobs measured earlier in this section tuned the WAL;
none removed it.

Only data point so far, dev PC 1e11 (interleaved, files valid: count and positions
checked against primecount, no sidecars left): WAL 11.49 / 21.22s, off 6.74 /
6.03s. The 1e12 A/B was stopped (the dev PC disk is not the target); decide on a
server A/B at 1e12 (A, B, B, A).

**Server A/B, 2026-10-01** (i5-13500, 1e12 `.db`, WAL/off/off/WAL, `sync` inside the
timed span so `synchronous=OFF` can't leave its writes in the page cache; all four
files 20,226.5 MB +-0.003%, `--count` and the last position correct, no sidecars):

| order | mode | time |
|---|---|---:|
| 1 | WAL | 59.85s |
| 2 | off | 67.39s |
| 3 | off | 87.89s |
| 4 | WAL | 85.86s |

`off` never wins: +12.6% in the first pair, +2.4% in the second, +6.6% by the ABBA
estimator. Not a large loss, but none of the dev PC's 2-3x shows up. Plausible
mechanism: WAL appends sequentially to the `-wal` file and checkpoints in bulk, while
`journal_mode=OFF` writes every page in place, scattered by B-tree page splits.
**Reverted**: knob and code removed, WAL stays.

Side finding, worth more than the knob: both modes got ~30% slower between runs 2
and 3 (60-67s -> 86-88s) after ~40-60 GB written back to back -- most likely the
SSD's fast write cache running out, or dirty-page writeback throttling. `.db`
timings on the server depend on the disk's recent write history; ABBA only cancels
linear drift, so space such runs out (or check the drift) before reading small
differences. For the 1e15 plan (~16-19 TB in one run) the sustained write speed, not
the burst one, is what counts.

### Write-pipeline knobs: `BATCH`, `--db-block-size`, `wal_autocheckpoint` (all measured, kept at their defaults)

Profiled after the single-pass elimination (see the `.db` single-pass entry in
project history/git log): `writer_loop()` spends most of its wall-clock inside
`COMMIT`, not `insert_block()` itself -- sieve threads sit idle on the
backpressure queue while the single writer thread is the critical path. Three
tunable knobs were tried, all measured with clean interleaved A/B (same
binary, alternated, not back-to-back-same-config):

1. **`BATCH`** (`writer_loop`'s commits-per-transaction, hardcoded 1000, not
   a CLI flag): tried 3000/10000/100000. Default 1000 **won clearly** against
   10000 in a 3-rep interleaved test (14.7-19.9s vs. 24.2-28.1s). Kept at
   1000.
2. **`--db-block-size`** (primes/compressed-block, default 65536): tried 4x
   smaller (16384) and 16x bigger (1048576). **Both directions lost**
   (16-21s at default vs. 27-30s either extreme) -- 65536 is a real local
   optimum, not an arbitrary default.
3. **`PRAGMA wal_autocheckpoint=0`** (disable the ~4MB-WAL auto-checkpoint,
   relying only on `finish()`'s own explicit one): mixed and *more variable*
   than default (18.8-30.1s vs. a tight 20.0-20.8s for default across 3
   reps) -- deferring all checkpointing to one final flush trades predictable
   incremental cost for an unpredictable final spike. Reverted.

The bigger ~150-320MB/s-vs-disk-capability gap this profiling was chasing
looks structural, not a tunable parameter: SQLite only allows one writer
transaction at a time, so no amount of batching/block-size/checkpoint tuning
inside that single serialized thread can exceed what one thread's own
syscall/fsync pattern allows. Actually closing that gap would mean sharding
the `.db` across multiple files/writer threads for true write parallelism --
a real format and `nth_prime` interface change, not a tuning pass.
**How to apply:** don't re-propose bigger commit batches, bigger or smaller
`--db-block-size`, or disabling `wal_autocheckpoint` as fresh ideas for this
gap -- all three are measured, reverted dead ends specifically for it.

### `PRAGMA cache_size` increase (tried, reverted, 2026-09-25)

`PRAGMA cache_size=-524288` (512MB, up from SQLite's own default -2000/2MB), on the
hypothesis that a bigger internal page cache would cut down on re-fetching
`block_data`'s B-tree pages as it grows into millions of rows at large N. Measured
WORSE on the server at both N=1e12 and N=1e13 -- reverted. Root cause not isolated;
don't re-propose a bigger `cache_size` without new evidence for why the default
would be the bottleneck.

### `blocks`/`block_data` table split (kept until format 3, 2026-10-02: the blocks left SQLite, see `.db` output: the `.blk` sidecar)

Metadata (small, mutable -- `start_index` gets corrected after the fact via
`fix_offsets`) lives apart from the compressed payload (large, immutable, written
once). SQLite stores every column of a row together in one B-tree cell, so an
UPDATE touching even one small integer column would otherwise have to rewrite each
row's BLOB too (measured: an UPDATE across ~62K rows/2.6GB of blobs at N=1e11 took
~12-17s, comparable to the sieve pass itself it was meant to be cheaper than) --
splitting them means `fix_offsets` only ever touches the tiny `blocks` rows.

### Page size (kept)

`PRAGMA page_size` only takes effect on a page-less (brand new) database, so any
stale file at the target path must be removed first -- a leftover 64KB page size
would waste ~half a page per block via internal fragmentation (this was the
dominant overhead in an early prototype before it was tracked down).

## main.cpp
2026-10-03: the sizing and tier decisions (segment width in its six steps,
sub-block, cutoffs, prefetch gate, steal threshold, the tier split and the
startup log) moved out of `main()` into `src/tuning.hpp` (`plan_sieve`,
`print_plan`), unchanged; the entries below name `main.cpp` as they were
written. `main.cpp` keeps the flow: arguments, base primes, chunking, the
three output passes.

Same day, the refactor measured on the dev PC (i5-11400F, 12 threads). A/B
against the committed binary, 3 interleaved reps: 1e13 tail 3.40-3.45 s both,
1e15 tail 7.28-7.62 s before vs 6.97-7.22 s after. Then `REPS=3` of both
benchmarks: counts 0.86 / 0.81 / 0.87 / 0.86x (1e13: 303.69 s vs 352.756 s;
the table's round had 308.81 vs 363.157 s), tails 1e13-1e17 0.81 / 0.73 /
0.72 / 0.76 / 0.79x (3.39 / 4.96 / 6.74 / 9.22 / 12.07 s against primesieve
4.171 / 6.789 / 9.383 / 12.135 / 15.371 s; the table's 0.80 / 0.73 / 0.74 /
0.77 / 0.81x). Both programs 1-6% under the table's day, so the host; the
ratios equal or a hundredth or two better. 1e18 not measured: the host ran
out of memory on that tail (~5 GB per program at 12 threads) and the run was
killed, so BENCHMARK.md's dev tables stay on their complete round.

### `SUB_BLOCK_BYTES`: per-thread vs. machine-wide sizing (kept, uniform-with-margin wins)

An earlier version sized each thread individually for wherever it happened to be
running (`sched_getcpu()` + a per-CPU table) instead of one conservative
machine-wide value (the smallest detected domain). Measured SLOWER on the actual
target hardware (i5-13500, 2026-09: ~28-30s vs ~26-27s at N=1e12) than the simpler
"smallest domain, same for everyone" version, even though the per-thread version
was the mathematically "fairer" one.

The uniform, smallest-wins version tracks a run where an earlier, buggy version of
the per-thread code (which -- by an unrelated arithmetic bug, since fixed -- ended
up dividing every thread's share by an EXTRA 2 on top of the fair-share division)
measured fastest of all (~25-26s): not because the bug's exact numbers were
special, but because a smaller, safely-under-budget segment (sized once for every
thread, not per-thread-recomputed) seems to matter more on real many-thread-
contended hardware than hitting each core's own "fair" cache share exactly. See
git history for the full A/B trail (dev PC and server) behind this.

### `sieve_chunk`: one `SegmentSieve` per worker instead of per chunk (tried, reverted -- neutral on cycles:u, 2026-09-26)

`sieve_chunk` constructs a fresh `SegmentSieve` (its `words_` buffer alone is
~256KB at typical segment widths, above glibc's default mmap threshold) for
every chunk -- `CHUNKS_PER_THREAD=150` means up to 150 constructions per
thread per pass, twice over for text mode's two passes. `begin_chunk()`
exists specifically to reset a `SegmentSieve` for reuse across chunks without
reallocating, but was never actually used that way -- it's only ever called
once, immediately after construction, in the same function. This looked like
a clear, architecture-independent redundancy worth fixing on that basis
alone: reuse one `SegmentSieve` per `run_parallel_chunks` worker slot
(0..workers-1, stable for that call's whole lifetime) instead of one per
chunk, calling `begin_chunk()` between chunks as originally intended.
Required threading a worker-slot index through `run_parallel_chunks`'s
callback and moving `SegmentSieve` construction out of `sieve_chunk` into
each of the four call sites (count-only, `.db`, and text mode's two passes,
the last two sharing one set of per-slot instances). Correctness fully held
(`make test` green, pi(1e12) exact) -- `begin_chunk()`'s own reset already
clears every piece of per-chunk state (`words_` doesn't need explicit
clearing either: presieve fill overwrites each segment's whole byte range
before any marking touches it), so reuse across arbitrarily-ordered,
non-consecutive chunks on the same instance is safe by construction, not
just in the cases tested.

Measured (dev PC, i5-11400F, `perf stat cycles:u,instructions:u`, N=1e12,
order-varied and cooldown-separated to rule out the thermal/frequency drift
this project's own methodology notes have flagged before): an initial,
uncontrolled pair looked like a real win (wall 30.13s->28.92s, sys
0.146s->0.084s), but a careful 4-run OLD/NEW/NEW/OLD sequence with an 8s
cooldown between each didn't confirm it on the metric this project trusts:

| metric | baseline (2 reps avg) | with reuse (2 reps avg) | delta |
|---|---:|---:|---:|
| cycles:u | 1.3687T | 1.3727T | +0.3% (noise) |
| instructions:u | 1.306988T (identical both reps) | 1.306384T (identical both reps) | -0.05% (real, tiny) |
| wall-clock | 29.11s | 28.60s | -1.75% |
| sys time | 0.128s | 0.096s | -25% relative |

`instructions:u` being bit-identical across repeats on each side (not just
close) confirms the tiny instruction-count saving is real, not noise -- but
it's far too small to explain the wall-clock gap. `cycles:u`, the
frequency-independent metric this project's whole methodology is built
around specifically to see past exactly this kind of gap, shows no
improvement at all (if anything, a fraction of a percent worse, within
noise). The wall-clock/sys-time delta not showing up in cycles:u is the same
signature this project's own methodology notes already catalogued (see the
CHUNKS_PER_THREAD entry above, and `perf-cache-scaling-validated` project
memory) as frequency/thermal drift between runs, not a real code effect.
Root cause of the *lack* of a real win, not independently confirmed but
consistent with `page-faults:u` also not moving between binaries: glibc's
allocator dynamically raises its own mmap threshold once it observes a freed
chunk get immediately re-requested at a similar size (specifically to avoid
mmap/munmap thrashing for exactly this repeated-allocation pattern) -- the
redundancy this change targets was plausibly already absorbed by the
allocator before it ever reached the kernel, which would also explain why
`sys` time's absolute drop (tens of ms) is real but small relative to total
wall-clock, not the dominant effect the initial uncontrolled reading
suggested.

**Reverted** (working tree only, never committed) -- not because reusing the
`SegmentSieve` is wrong (it still matches `begin_chunk()`'s own documented
intent better than the current one-per-chunk pattern), but because this
project only keeps changes with a measured win on `cycles:u`, and this one
doesn't have one on this hardware. Not re-run on the production server
(i5-13500) -- skipped by the user's own call, not because the reasoning
wouldn't transfer; if the server's allocator or thread count ever makes this
worth re-checking, the code for it is in git history (this commit's diff),
not carried forward as a live flag.
**Why:** a change that's architecturally cleaner and provably correct can
still be a net-zero on the metric that actually decides whether it ships --
worth recording so "construct once per worker, not per chunk" isn't
re-proposed as an obviously-free win without first checking whether the
allocator has already absorbed the cost being targeted.
**How to apply:** don't re-propose per-chunk `SegmentSieve` construction as
unexamined overhead -- it's now measured as allocator-absorbed, not a real
cost, on this codebase's target allocator (glibc). A different allocator
(jemalloc, tcmalloc, or a `--static` musl build) could plausibly behave
differently here; that would be a new, from-scratch measurement, not a
re-run of this one.

**Follow-up (2026-10-01): kept.** The per-chunk construction grew since:
med64's 768 lists are reserved on every new instance, and the sparse tier's
block arenas start empty. Profiling 1e10 `-t 12` showed `malloc`/`free`/
`memset` at ~1% and a tiny-tail startup at 6.7 ms vs primesieve's 2.3 ms.
Reimplemented without threading a slot index through `run_parallel_chunks`:
`sieve_chunk` keeps a `thread_local` cache of up to two instances, keyed by
the `TierSet` (a run has a narrow and a wide one); worker threads only live
for one pass, so the cache goes with them. Dev PC, `perf stat`, 3 interleaved
reps (2 for the tail), identical counts:

| | per chunk | per thread | delta |
|---|---:|---:|---:|
| 1e10 `-t 12` cycles:u | 7.626G | 7.502G | -1.6% |
| 1e11 `-t 12` cycles:u | 92.66G | 92.10G | -0.6% |
| 1e13 last 0.1% `-t 12` cycles:u | 18.03G | 17.50G | -2.9% |
| tiny 1e10 tail `-t 12`, wall | 6.67 ms | 6.12 ms | -0.55 ms |

Instructions drop too (-1.7% at 1e10, -0.9% at 1e11), so this isn't the
frequency drift that sank the first measurement; the gain is largest where
chunks are short (small N, `--start` tails). At 1e12+ full runs it stays
small, as the first entry found.

### `run_parallel_chunks`: chunk-granularity idle-time investigation (2026-09-25, external review, Opus 5.5)

Hypothesis: `CHUNKS_PER_THREAD=16` is too coarse at large N -- a fixed chunk count
means each chunk gets proportionally WIDER as N grows (1e13: ~20s/chunk on the
server; 1e14 projected ~250s/chunk), and since `split_ranges` carves chunks in
original k-order, the LAST chunks (widest population) could still leave most
threads idle at the end even with the shared-counter queue -- worse on the
server's i5-13500 specifically if a straggler chunk lands on a slower E-core.
Proposed the `ERATOSTENES_DEBUG_IDLE` diagnostic as the cheap first step before
trying any fix, given the review's own honest caveat that N=1e12 already sits at
ratio 1.01 with the identical scheme.

Measured on the dev PC (i5-11400F, 12 symmetric P-cores, no E-cores): idle=1.4% at
N=1e12, idle=0.9% at N=1e13 -- LOWER at the larger N, not higher, directly
contradicting the "idle grows with N" half of the hypothesis, and far too small
either way to explain this machine's own ~13% ratio gap at 1e13. The queue is
already doing its job here. Caveat: this refutes the mechanism on symmetric cores
specifically -- the review's own strongest argument (a straggler chunk landing on
a slow E-core) has no equivalent on this dev PC to test, since it has none.

**Done (2026-09-25, production server, i5-13500, 20 threads, real P/E cores this
time)**: idle=2.1% at N=1e12, idle=2.1% at N=1e13 -- flat across N, same as the dev
PC, and still nowhere near large enough to explain this machine's own ~12% ratio
gap at 1e13. This closes the straggler-on-an-E-core mechanism too, on the one
machine that could actually exercise it: the queue keeps every thread fed
regardless of which core class picks up the tail chunks. **VERDICT: as an
explanation for the ~12% ratio gap, REJECTED on real target hardware, not just the
dev PC** -- 2.1% idle can't be most of a 12-point gap. Chunk granularity/scheduling
is not worth re-investigating for THAT reason.

**Follow-up (2026-09-25, same review)**: a constant (not N-growing) ~2% idle at
BOTH N is still consistent with a simpler, smaller effect -- roughly half a chunk's
worth of tail latency, independent of the ratio gap entirely. Recovering it is
cheap (`CHUNKS_PER_THREAD` 16->150 tried, ~10 lines, doesn't touch any tier's hot
path) even if it doesn't explain the gap. Dev PC (i5-11400F) numbers looked
promising at first (idle 1.4%->0.2% at 1e12, 0.9%->0.2% at 1e13) but turned out
unusable: a same-config re-run drifted from 499.98s to 419.95s between two points
in the same session (16% apart, far past normal noise) after hours of continuous
heavy jobs on this machine -- almost certainly residual contention (see this
project's own "never run two thread-heavy jobs at once on the dev PC" lesson), not
a real effect; cycles:u under that same contention even flipped sign once (+0.84%
for 150 vs 16).

**Re-measured on the server (2026-09-25, i5-13500, 20 threads)**: idle DID drop the
same way (2.1%->0.2% at both N), and the first single-rep wall-clock comparison
looked like a wash (1e12 24.22s->24.81s, WORSE; 1e13 330.54s->327.50s, better) --
but that turned out to be measuring the wrong thing. `REPS=5 make docker-benchmark`
(which rebuilds and runs 1e10, then 5x1e11, then 5x1e12 back to back) showed 1e11
climbing 1.65s->1.75s->2.04s->2.04s->2.04s and 1e12 drifting 25.49->25.58s across
its own 5 reps, SAME build, nothing else changed -- a same-direction, reproducible
slowdown under sustained load, not random noise (thermal throttling or, if this is
a burstable cloud instance, exhausted CPU credit -- never confirmed which, but the
shape matches either). That contaminated every earlier server number in this
writeup, not just the k=150 ones: they were all taken after this session's own
hours of back-to-back heavy jobs.

Once measured cold/isolated instead (`make run`, nothing queued before or after,
repeated on different occasions): 1e12 consistently 23.91-23.92s, 1e13 325.93s --
both BETTER than this project's own README figures for this machine (24.51s,
330.38s) and never once worse across every clean reading taken. **KEPT at 150**:
idle drops from 2.1% to 0.2% (real, reproduced every time it was checked) and the
cold-measured wall-clock never regressed, only improved slightly -- the earlier
"REVERTED" verdict was itself a measurement artifact of the same accumulated-load
effect just described, not a real finding about `CHUNKS_PER_THREAD`. If revisiting
this again, measure cold (`make run`, isolated, no prior load that session) -- the
benchmark script's own `REPS` loop is NOT safe for this machine as currently
written, since later reps run hot.

### Chunk-width floor: at least 4 segments per chunk (kept, 2026-09-27)

`CHUNKS_PER_THREAD=150` (tuned at 1e12/1e13) makes chunks narrower than one
segment at small N: at 1e10 with 12 threads, 1800 chunks of ~1.48M wheel
indices each vs a ~2.1M-index segment (worse on the 20-thread server: 3000
chunks). Every chunk then pays full per-chunk setup (`SegmentSieve`, fresh
ring, re-activating every base prime with a runtime division) for one partial
segment. Chunk count now floored at `total_k / (4 * seg_k_width)` (still at
least one per thread); `ERATOSTENES_MIN_SEGS_PER_CHUNK` overrides the 4, 0
restores the old behavior exactly. Measured with that env var on one binary
(dev PC, 3 interleaved reps, still with the old 48KiB sub-block): 1e10
9.674G -> 9.538G cycles:u (-1.4%, all 3 reps separated), 1e11 neutral. No
effect at large N, where chunks span thousands of segments. Should matter
more on the server; not yet measured there.

### Top-of-range tails: startup costs that grow with sqrt(N) (kept, 2026-10-01)

`make benchmark-tails` (the last 1e11 numbers below N, see
[BENCHMARK.md](BENCHMARK.md)) lost more the higher the window: server
0.95/1.17/1.49/1.92x at 1e15/1e16/1e17/1e18, dev PC 0.83/0.91/1.13/1.76x. Sieving
a 1e11 window costs about the same at every height; what grows is pi(sqrt N):
1.95M base primes at 1e15, 50.8M at 1e18. Fitting T(W) = F + c*W over two
window widths (dev PC, 12 threads) gave a fixed part F of ~4-5 s at 1e17 and
~23 s at 1e18, against primesieve's 0.1-0.5 s and 1.3 s, with similar slopes
c. Startup with timestamped log lines, 1e18, 1e10 window: base-prime sieve
7.8 s on one thread, `classify` 1.4 s, worker threads 13.2 s, of which ~2.5 s
was sieving and the rest eight activations per thread of 50.5M sparse primes
(~1.3 s each; all 12 threads write their bucket rings at once, so it is
memory-bound).

Three changes, none of them in the per-segment loop:

- `sieve_base_primes` (base_sieve.hpp): segmented, 32 KiB windows of odd
  numbers (a byte each), each sieving prime carrying its next multiple between
  windows, instead of one 62 MB `vector<bool>`; the result is reserved from
  Dusart's bound on pi(x) (within 0.05% at 1e9), so it is never reallocated.
  sqrt(1e17): 1.44 s -> 0.26 s; sqrt(1e18) = 1e9: 7.8 s -> 0.86 s. Same output
  as the old sieve for every limit up to 70000 and at sqrt(1e10..1e18).
- `--start` tails: fewer chunks per thread than `TAIL_CHUNKS_PER_THREAD` (8)
  once activation would weigh: a chunk spans at least `K_PER_BASE_PRIME` =
  128 wheel indices per base prime (one activation costs about as much as
  sieving 2.6 indices, so ~2%), never under one chunk per thread. 12 threads,
  last 1e11: 8 per thread below 1e14 and 1e15 (unchanged), 3 below 1e16, 1
  below 1e17 and 1e18, i.e. primesieve's one setup per thread. Full runs don't
  take this path; their chunks span ~1e13 numbers at 1e16. (Replaced the same
  day by carrying the sieve across chunks, see the next entry.)
- `classify`: the narrow tier set is skipped when a tail starts past narrow^2
  (no chunk can use it: one pass over the base primes instead of two), and
  the sparse vector is reserved.

Dev PC, 12 threads, `REPS=2 make benchmark-tails`, best of 2 (primesieve
12.7):

| last 1e11 below | before | after | primesieve | ratio before -> after |
|---|---:|---:|---:|---:|
| 1e15 | 8.25 s | 8.95 s | 10.01 s | 0.83x -> 0.89x |
| 1e16 | 11.64 s | 11.12 s | 12.79 s | 0.91x -> 0.87x |
| 1e17 | 18.48 s | 14.80 s | 16.70 s | 1.13x -> 0.89x |
| 1e18 | 37.01 s | 20.60 s | 21.43 s | 1.76x -> 0.96x |

1e14 and 1e15 keep their chunking, so they only gain the (small) base-sieve
saving there. An interleaved A/B of both binaries (same flags, ABBA, 6 runs
each) ties: 1e14 mean 5.43 s vs 5.43 s, 1e15 median 8.10 s vs 8.09 s, with
one ~+15% outlier on each side, like this table's 8.95 s. All `make test`
cases pass.

Not measured yet: the server after the change. One chunk per thread at 1e17
and above gives up the queue's balancing between P- and E-cores there, as
primesieve's even split does.

### `run_parallel_chunks`: contiguous runs, the sieve carried across chunks, steals (kept, 2026-10-01)

The entry above traded balance for activations: one chunk per thread from
1e16 up left threads idle, `--debug-idle` 12.6% at the 1e18 tail on the dev PC
(12 threads, uniform cores), 10.9% (1e18) and 13.5% (1e16) on the i5-13500
server (20 threads, P- and E-cores). With the shared-counter queue every
chunk paid a full activation, because consecutive chunks went to different
threads.

- `run_parallel_chunks` gives each worker a contiguous run of chunk indices
  (an equal share) walked in order. A worker whose run is empty takes the back
  half of the run with the most chunks left, if that piece spans at least
  `STEAL_MIN_K` = 4 wheel indices per base prime (a steal activates every
  base prime once, ~2.6 indices' worth of sieving each when all threads
  activate at once; stealing pays when the piece takes longer than that);
  otherwise it stops. One mutex, taken once per chunk.
- `sieve_chunk` remembers, per thread and tier set, the k where its
  `SegmentSieve` stopped; a chunk starting there skips `begin_chunk()` and
  goes on as if both were one chunk. `split_ranges` makes every chunk but the
  last a whole number of segments, since the sparse ring counts whole
  segments.
- Tails go back to many chunks, `TAIL_CHUNKS_PER_THREAD` = 32 (only the steal
  granularity depends on it now); the `K_PER_BASE_PRIME` rule is gone.
- Also: `sieve_base_primes` splits its range over the threads (1e9: 0.87 s ->
  0.41 s with 12, the rest is concatenating 406 MB on one thread), and the
  sparse tier is a `std::span` over the tail of `base_primes` instead of a
  copy: every prime from 7 to 163 is pre-sieved, so the sparse tier is exactly
  the primes from max(sparse_limit, 164) up (`classify` throws if that ever
  stops being true).

`--debug-idle`, dev PC, 12 threads, last 1e11: 374 chunks, 2-9 steals; idle
1e15 3.8% -> 1.5%, 1e16 6.1% -> 1.0%, 1e18 12.6% -> 2.0-4.1%. Interleaved A/B
against 0719d5b (same flags, ABBA):

| case | runs | before | after | |
|---|---:|---:|---:|---:|
| full 1e11 | 4 + 4 | 1.995 s | 1.92 s | -3.8% (all 4 below) |
| full 1e12 | 4 + 4 | 24.51 s | 24.52 s | tie |
| tail 1e15 | 4 + 4 | 8.61 s | 8.05 s | -6.5% |
| tail 1e16 | 4 + 4 | 11.91 s | 11.52 s | -3.3% |
| tail 1e18 | 10 + 10 | median 21.33 s | median 20.39 s | -4.4% |

At 1e18 two back-to-back runs of the new binary took 25.8 and 28.3 s in the
first A/B (interference: the same binary ran 19.6-21.7 s in the other eight).
Full 1e11 gains from no longer re-activating ~27k base primes in each of its
~1800 chunks.

The largest N is now 2^64 - 2^32 * 16; `parse_args` rejects anything above it.
Activation computes `p * m` up to start + 14p (the sparse tier's mod-2310
multiplier moves up to 13 past ceil(start / p), 14 being the largest gap
between residues coprime to 2310) with p up to 2^32, so primesieve's own
ceiling, 2^64 - 2^32 * 10, isn't enough here: the last 1e9 below it threw
"bucket sieve: a sparse prime's step exceeds the bucket ring's margin" (a
wrapped `p * m`, caught by that check rather than miscounted). The last 1e9
below the new ceiling matches primesieve (22,546,380 primes, `-t 2`), and so
does the last 1e11 below 1e19 (2,285,738,870, dev PC `-t 6`: 30.93 s vs
29.33 s). primesieve doesn't check its expression parser: `primesieve
999999999e11 1e20` wraps both bounds modulo 2^64 and counts the window below
~7.77e18 (2,299,052,535 primes, more than the window below 1e19). `isqrt`
compares by division now: `(s + 1) * (s + 1)` wrapped to 0 for N >=
(2^32 - 1)^2.

### `-t 2` gap vs primesieve at the 1e15 tail: profile and sparse cutoff by thread count (measured, not adopted, 2026-10-01)

With one thread per core the gap is flat from 1e14 to 1e18: dev PC, `THREADS=2
REPS=2 make benchmark-tails` at 0719d5b, 1.19/1.25/1.26/1.23/1.27x (15.15 vs
12.72 s at 1e14, 33.67 vs 26.47 s at 1e18). perf stat, last 1e11 below 1e15,
`-t 2`, 2 runs each (within 1.5%):

| | eratostenes | primesieve |
|---|---:|---:|
| cycles:u | 158.8G | 131.5G |
| instructions:u | 269.3G | 275.2G |
| IPC | 1.70 | 2.10 |
| branch-misses:u | 1.318G | 0.992G |
| L1-dcache-loads / misses | 58.9G / 18.0G | 75.6G / 17.0G |

Fewer instructions than primesieve, 21% more cycles: stalls, not work.
(WSL's vPMU has no generic LLC events, but `l2_rqsts.*`,
`mem_load_retired.l2_miss/l3_miss` and `cycle_activity.stalls_l2_miss` work.)
By symbol (cycles, G): process_big 43.3, medium 56.1, med64 34.9, small 18.7,
presieve 3.4; primesieve EratBig 62.4, EratMedium 43.0, EratSmall 22.1. Our
dense tiers below the bucket ring cost 109.7G against primesieve's 65.1G,
because ours reach p < 4.19M (~1 hit per 512 KiB segment) while primesieve
sends everything above ~0.8M (~3 hits per 256 KiB sieve) to EratBig: that's
the medium tier's per-prime loop-exit mispredict again (see the i5-13500
entry below).

`--tune sparse` at the same tail, cycles:u (2 runs each, ABCDE EDCBA order at
`-t 2`, ABC CBA at 6/12):

| threads | 1/1 (dev default) | 1/2 | 1/4 | 1/8 |
|---:|---:|---:|---:|---:|
| 2 | 159.4G | -6.7% | -7.0% | -3.2% |
| 6 | 219.3G | +8.0% | +16.7% | |
| 12 | 350.7G | +13.6% | +29.0% | |

`--l2-bytes 256k` (256 KiB segment) at `-t 2`: +10%. Branch-misses fall with
the cutoff at every thread count (2 threads: 1.32G -> 0.91G -> 0.65G ->
0.52G), but from 6 threads up the extra bucket traffic costs more than they
save. `-t 6` gives every thread its own core and 512 KiB of L2, like `-t 2`,
and still loses, so it isn't the per-thread L2 (unlike the sub-block): it
looks like a shared resource, L3 or memory bandwidth (12 MiB of L3 for 2
threads vs 6), and the i5-13500 won with 1/2 at 20 threads. No thread-count
rule fits; left at the current gate. A startup calibration of this cutoff
on the machine at hand is the candidate.

### Segment ceiling: half the L2 per thread, within 16-32 x L1d (kept, 2026-10-02)

The sparse-regime doubling (see arg_parser.hpp's entries) takes the segment to
the whole L2 share. On the 2-vCPU Emerald Rapids sandbox (48 KiB L1d, 2 MiB L2
per vCPU) that is 2 MiB, and `-s` at 1 MiB or 512 KiB was 3-11% faster on the
last 1e11 below 1e15..1e18. primesieve never goes there: api.cpp's
`get_sieve_size` caps its sieve at 16 x L1d and below the L2 per thread (8 x
L1d when the OS reports no cache sharing, as in VMs). A first ceiling of
16 x L1d (6f645c9) fixed 1e15+ on that VM (-4..-9%) but cost +6% at 1e13
(768 KiB instead of 1 MiB) and +3% at 1e14; on the 32 KiB-L1d / 1 MiB-L2 Xeon
sandbox it took 1 MiB down to 512 KiB, -7% at 1e15, neutral at 1e18. Both
fit `max(16 x L1d, min(32 x L1d, L2 per thread / 2))`: 1 MiB on the first,
512 KiB on the second; the dev PC and the i5-13500 (256 KiB of L2 per thread
under HT) get 768 KiB, above their 512 KiB, unchanged. Fitted on two VMs; the
startup log says when it applies.

**2026-10-03, the `L2 / 2` term dropped (three operators, 691509d, `-t 2`,
last 1e11 below N, `real`, two passes).** auto / `-s 31457280` (1 MiB) /
`-s 47185920` (1.5 MiB):

| host, width | 1e14 | 1e15 | 1e16 | 1e17 | 1e18 |
|---|---|---|---|---|---|
| Xeon 2.80, auto (512 KiB) | 19.04, 18.06 | 21.80, 21.36 | 24.99, 24.99 | 29.54, 29.30 | 34.52, 34.94 |
| Xeon 2.80, 1 MiB | 17.89, 17.52 | 21.46, 21.40 | 24.00, 24.61 | 28.48, 29.17 | 33.68, 33.35 |
| Emerald Rapids clean, auto (1 MiB) | 15.49, 15.05 | 17.43, 17.55 | 20.03, 20.03 | 23.08, 23.74 | 26.66, 26.69 |
| Emerald Rapids clean, `-s 31457280` | 15.19, 15.00 | 17.67, 17.88 | 20.47, 20.98 | 23.13, 23.60 | 27.06, 27.01 |
| Emerald Rapids noisy, auto (1 MiB) | 14.75, 14.84 | 17.96, 17.32 | 20.73, 19.71 | 24.55, 23.35 | 29.19, 29.45 |
| Emerald Rapids noisy, `-s 31457280` | 14.88, 14.24 | 16.97, 17.01 | 21.22, 20.21 | 23.95, 25.25 | 29.21, 28.57 |

Xeon 2.80, means: 1 MiB -4.6% / -0.7% / -2.8% / -2.1% / -3.5%, 9 of 10
passes under both auto passes. Third round on this CPU with the same sign
(e379255 with cutoff 1/2: -2.5% / -4.6%; 691509d with 1/4: -3.2% / -3.8%,
both 1e14 / 1e15). On the Emerald Rapids `-s 31457280` is the auto width,
so those pairs are the host's noise floor: within 3%, both signs.

`-s 47185920` never ran in the sparse regime: the power-of-2 fixup (main.cpp,
after the ceiling) rounds an explicit `-s` down as well, 1.5 MiB -> 1 MiB,
and said nothing (the startup log now reports it). The 1.5 MiB width only
held at 1e14, where it has no sparse tier at all (isqrt(1e14) = 1e7 below
its seg_k_width of 12.6M: 582,554 medium primes, 0 sparse) and cost +8%
(clean host), +17% (noisy), +5% (Xeon 2.80) -- the medium tier down to one
hit per segment, not a segment-size result. So on the Emerald Rapids the
ceiling's `L2 / 2` (1 MiB) and `L2` (1.5 MiB, rounded to 1 MiB) were never
distinguishable, and on the Xeon 2.80 `L2 / 2` was wrong. The ceiling is
now `max(16 x L1d, min(32 x L1d, L2 per thread))`, the fixup after it: Xeon
2.80 1 MiB, Emerald Rapids 1 MiB, the HT machines (768 KiB floor above
their 512 KiB) unchanged.

Same round, the SMT question on the Xeon 2.80 (the one VM whose dense tiers
lose, 1.02-1.04x at the 1e13 tail): 1e13 tail, `-t 1` -> `-t 2`, eratostenes
28.93 -> 14.21 s (2.04x), primesieve 27.76 -> 14.26 s (1.95x); the Emerald
Rapids hosts 1.92x / 1.88x and 1.98x / 1.97x. Two real cores on all three
VMs, so that loss is not a shared physical core. At `-t 1` the ratios are
1.04x (Xeon 2.80), 1.02x and 0.95x (Emerald Rapids): the same as at `-t 2`.

**Validation of the ceiling change (0192744, `REPS=3 make benchmark-tails`,
best of 3, same day):** the Xeon 2.80 host, auto now 1 MiB, against its
691509d table (512 KiB, best of 2):

| N | 691509d | 0192744 | all 3 reps | primesieve 691509d / 0192744 |
|---|---:|---:|---|---:|
| 1e13 | 14.13 | 14.19 (+0.4%) | 14.61, 14.19, 14.81 | 13.58 / 13.91 |
| 1e14 | 18.18 | 17.69 (-2.7%) | 17.72, 17.69, 18.13 | 16.21 / 16.27 |
| 1e15 | 20.84 | 20.80 (-0.2%) | 21.17, 20.80, 21.17 | 19.79 / 18.94 |
| 1e16 | 24.52 | 24.07 (-1.8%) | 24.07, 24.68, 24.33 | 21.89 / 21.80 |
| 1e17 | 28.89 | 28.56 (-1.1%) | 29.26, 28.85, 28.56 | 24.88 / 27.08 |
| 1e18 | 34.32 | 33.57 (-2.2%) | 34.28, 33.57, 33.82 | 33.71 / 32.59 |

primesieve within 2% of its previous best at every N but 1e17 (host), so the
host state is comparable. 1e13 unchanged, as it should be (no sparse tier
there, the whole-L2 base rule already gave 1 MiB). 1e14 and 1e18: every rep
below both 691509d reps; 1e16 and 1e17: the best rep below, the spread
overlapping; 1e15: flat, as in the `-s` sweep (-0.7% there). Smaller than
the `-s 31457280` runs promised (-2..-5%), the sign is the same in all four
tails where it was expected. Ratios 1.02 / 1.09 / 1.10 / 1.10 / 1.05 / 1.03x
(BENCHMARK.md updated); the 1e15 1.10x is primesieve's fast run, not a
regression. The two Emerald Rapids operators ran the same commit with the
same effective binary: 1.00 / 0.95 / 0.99 / 1.01 / 1.02 / 1.01x on the host
in its fast state, 0.91 / 0.98 / 0.93 / 1.01 / 0.93 / 0.97x on a host 10%
slower that day for both programs -- the host-to-host spread again; the
BENCHMARK.md table there stays on the 691509d round, which was faster in
every tail.

### Whole-L2 base segment: one thread per core, no sparse tier (kept, 2026-10-03)

The base segment is half the L2 share so the segment, the tiers' state and a
hyperthread sibling fit together. On the 2-vCPU Xeon @ 2.80GHz sandbox (32 KiB
L1d, 1 MiB L2 per vCPU, no SMT -- the only machine still losing to primesieve,
1.00-1.16x on the tails) a sweep of the knobs over the last 1e11 below 1e13 and
1e15 moved only one of them: `--l2-bytes 2097152`, i.e. a 1 MiB base segment
instead of 512 KiB, was -8..-13% at 1e13 on two hosts (every run below every
default run) and 0..-3% at 1e15, where the sparse-regime doubling already gives
1 MiB and the ceiling takes it back to 512 KiB. `--tune sparse=1/1` was +9% at
1e15 (1/2 stays), the sub-block, the prefetch distance and `minsegs` were noise.

Rule: when no base prime is sparse (no doubling) and there are no more threads
than physical cores with the largest L1d (the same `one_per_core` as the
whole-L1d sub-block), the base segment is the whole L2 share, within 32 x L1d.
On an SMT machine the share is already half the L2 and nothing changes (dev
PC, i5-13500: 256 KiB per thread). The ceiling now applies only in the sparse
regime, where it was fitted; below it the rule above is the only widening.
Consequences: Xeon 2.80 1e13/1e14 512 KiB -> 1 MiB (the measured win), 1e15+
unchanged; Emerald Rapids (48 KiB L1d, 2 MiB L2) 1e13 1 MiB -> 1.5 MiB,
1e14+ unchanged (operators to A/B against `-s 31457280`, 1 MiB); the dev PC
`-t 2` with a forced 512 KiB base (`--l2-bytes 1048576`, L2 per core instead of
per thread) was neutral: cycles:u 1e11 48.9/52.7 vs 53.4/52.5 G, 1e12 698/710
vs 702/666 G, 1e13 tail 104.7/103.7 vs 108.1/102.4 G (ABAB). `-t 12` and
every run with -s or --l2-bytes keep their width. The startup log says when the
rule applies.

Validation on the sandboxes (e379255, last 1e11 below 1e13, `real`, two
passes each of auto / `-s 15728640` (512 KiB) / `-s 31457280` (1 MiB)):

- Xeon 2.80GHz (auto = 1 MiB): 13.67, 13.66 / 15.26, 15.26 / 13.82, 13.63 s --
  -10.5% against the old 512 KiB, equal to the forced 1 MiB as expected. The
  1e13 tail went from 1.10x to 1.02x. 1e14-1e17 stay 1.10-1.15x: there the
  sparse regime holds and the ceiling keeps 512 KiB.
- Emerald Rapids (auto = 1.5 MiB), two hosts: 12.47, 12.39 / 15.28, 15.09 /
  12.92, 12.97 s (-4% against 1 MiB) and 14.76, 14.11 / 15.33, 15.33 / 13.44,
  13.79 s (+5% against 1 MiB, the noisier host). 1.5 vs 1 MiB unresolved,
  512 KiB clearly worst on both; the 1e13 tail went from 0.97x to 0.91x /
  1.00x. A 32 x L1d cap that gave 1 MiB here (21 x L1d) would take the Xeon
  2.80 back to 672 KiB, so the cap stays unless a longer ABAB settles it.
  Settled the next round on the clean host: 1.5 MiB 12.06, 11.73, 12.05 s vs
  1 MiB 12.45, 12.54, 12.64 s (-5%, 3/3); the noisy host spread 14.4-16.8 s
  on the same configuration and said nothing.

### Half the whole-L2 width with few base primes, on every one-per-core machine (kept, 2026-10-06)

The 2026-10-04 i5-3470 rule (base at half the L2 while the base primes are
at most 40K, gated on L2 <= 256 KiB) turns out not to be about the small
L2. Chasing the 1e11 counts on the 2-vCPU sandboxes (1.03-1.11x) with `make
benchmark-ab`, x3 interleaved against auto, 1e11 (9e10 window) / 1e12 tail
(1e11 window):

| machine | auto | B | 1e11 | 1e12 |
|---|---|---|---:|---:|
| Emerald Rapids (48 KiB L1d, 2 MiB L2) | 1.5 MiB (32 x L1d cap) | `-s 23592960` (768 KiB) | **-3.3% (3/3)** | -2.1% (overlap) |
| same | same | `-s 31457280` (1 MiB, half the L2) | -1.7% (3/3) | -0.5% (overlap) |
| Xeon @ 2.80GHz (32 KiB L1d, 1 MiB L2) | 1 MiB | `-s 15728640` (512 KiB) | -0.5% (overlap) | +2.4% (overlap) |

On Emerald Rapids the gain grows as the segment shrinks and the best point
is half of the width the whole-L2 rule picked (768 KiB of 1.5 MiB), not
half of the L2 (1 MiB) -- the same shape as the i5-3470's 128 KiB of 256
KiB. On the Xeon @ 2.80GHz half the width is a tie. So the rule becomes:
**one thread per core, no sparse tier, base primes <= 40K: the base segment
is half the whole-L2 width** (min(L2 share, 32 x L1d) / 2), on any L2; with
more base primes the whole width as before. `--l2-bytes` now stands in for
the detected share inside this rule too (it used to skip it), so test.sh
checks both branches with forced caches: 128 KiB from a 256 KiB L2, 768 KiB
from a 2 MiB L2 with a 48 KiB L1d, and the whole 1.5 MiB once the base
primes pass 40K. Nothing changes on SMT machines (the share is already half
the L2 there, the whole-L2 rule is a no-op). The Xeon @ 2.80GHz's 1e11 also
had the old 55e2808 binary 2.6% ahead of 5f7d213 (3/3, overlapping); its
segment was the suspect, and this A/B clears it -- with the same 512 KiB
the current binary is not faster, so whatever is left there is another
change or that night's host.

### Sparse cutoff 1/4 from 1 MiB of L2 per thread (kept, 2026-10-03)

Same operators, e379255, last 1e11 below 1e14 and 1e15, `real`, two passes
of auto (cutoff 1/2) / `-s 31457280` (1 MiB segment, i.e. without the
ceiling on the Xeon 2.80; on the Emerald Rapids auto already is 1 MiB there)
/ `--tune sparse=1/4`:

| host | tail | auto | 1 MiB | sparse 1/4 |
|---|---|---:|---:|---:|
| Emerald Rapids, clean | 1e14 | 15.44, 15.54 | 15.26, 15.19 | 14.51, 14.84 |
| | 1e15 | 17.61, 17.60 | 17.53, 17.68 | 16.80, 16.72 |
| Xeon 2.80 | 1e14 | 18.55, 18.09 | 18.08, 17.50 | 17.96, 17.87 |
| | 1e15 | 21.74, 22.09 | 20.56, 21.23 | 20.72, 21.19 |
| Emerald Rapids, noisy | 1e14 | 17.95, 19.93 | 18.00, 18.69 | 17.37, 17.85 |
| | 1e15 | 21.16, 21.54 | 20.97, 21.35 | 23.70, 21.05 |

1/4 is -5% on the clean Emerald Rapids at both tails (4/4) and -2..-4% on the
Xeon 2.80; the noisy host agrees at 1e14 and says nothing at 1e15. These are
2-vCPU machines with 1 MiB and 2 MiB of L2 per vCPU and no SMT. The i5-13500
(640 KiB per thread) had 1/4 behind 1/2 at both N (-1.1% vs -6.0% at 1e14,
+6.2% vs +0.2% at 1e15, P-cores, cycles:u), and the dev PC at `-t 2` had 1/2
and 1/4 tied (-6.7% / -7.0% against 1/1) -- the bucket ring's extra blocks
want room in L2. So the per-thread-L2 gate gains a second step: 1/2 from
512 KiB (unchanged), 1/4 from 1 MiB. Nothing changes on the dev PC (256 KiB),
the i5-13500 (640/512 KiB) or the i5-1235U (640/512 KiB); `--tune sparse`
still overrides. The startup log prints the cutoff and why.

Also in that round, the Xeon 2.80 with the 1 MiB segment in the sparse
regime (the ceiling lifted): -2.5% at 1e14, -4.6% at 1e15 -- the opposite
sign to the -7% at 1e15 that fitted the ceiling's `L2 / 2` term on another
host of this CPU (2026-10-02). Host noise either way; the ceiling stays
until 1/4 and the segment are measured together.

Confirmation round on 691509d (1/4 now the default), same three operators,
two passes of auto (1/4) / `--tune sparse=1/2` / `-s 31457280` (1 MiB), last
1e11 below 1e14 and 1e15, `real`:

| host | tail | auto (1/4) | sparse 1/2 | 1 MiB |
|---|---|---:|---:|---:|
| Emerald Rapids, clean | 1e14 | 14.72, 15.02 | 15.73, 14.91 | 14.31, 14.60 |
| | 1e15 | 16.71, 17.22 | 17.70, 17.61 | 17.28, 16.88 |
| Xeon 2.80 | 1e14 | 17.72, 18.04 | 18.47, 18.17 | 17.18, 17.42 |
| | 1e15 | 21.37, 21.03 | 21.86, 21.61 | 20.43, 20.37 |
| Emerald Rapids, noisy | 1e14 | 16.93, 17.62 | 17.92, 17.49 | 17.47, 16.93 |
| | 1e15 | 19.80, 19.73 | 20.83, 19.61 | 20.14, 19.76 |

1/2 is +3..+4% (means) on the clean Emerald Rapids and +2.5% on the Xeon
2.80; the noisy host has it +2.4% with one pass each way. The 1/4 default
holds. On the Emerald Rapids the 1 MiB column repeats auto (same segment)
and lands within 1% of it, which is the host noise floor for this test.
On the Xeon 2.80 the 1 MiB segment on top of 1/4 is -3.2% at 1e14 and
-3.8% at 1e15 (4/4 passes under every auto pass), the same sign as the
e379255 round (-2.5% / -4.6% with 1/2): the ceiling's `L2 / 2` term now
has two rounds against it and one for it (the -7% at 1e15 of 2026-10-02)
on this CPU. Tails on 691509d: clean Emerald Rapids 0.94 / 0.95 / 0.96 /
0.90 / 0.99 / 0.96 (first round with all six under 1.00x on this machine;
BENCHMARK.md updated), Xeon 2.80 1.04 / 1.12 / 1.05 / 1.12 / 1.16 / 1.02,
the same as the e379255 row within host noise (BENCHMARK.md updated to this run all the same, so both VM tables sit on 691509d). Next on the Xeon
2.80: auto vs `-s 31457280` at 1e16-1e18 (the uncapped 2 MiB lost there on
the Emerald Rapids); if 1 MiB holds, lift the ceiling to the whole L2 share
when L2 <= 32 x L1d, which leaves the Emerald Rapids (2 MiB > 1.5 MiB) at
its measured 1 MiB.

### Base segment capped at 32 x L1d: a VM whose sysfs reports the host's L3 as L2 (kept, 2026-10-03)

A 2010 MacBook Pro (Core i7 M620, Arrandale: 2 cores + HT, 32 KiB L1d,
256 KiB L2 per core, 4 MiB L3, 8 GB) running the latest Ubuntu with Docker
Desktop, which on Linux too runs the containers inside its own VM. What that
VM's sysfs says, through `make docker-benchmark-mini` (scripts/benchmark_mini.sh,
new today: sysfs listing, the CLI's own choice, a segment / cutoff / prefetch
sweep, each run paired with its own primesieve run):

```
index0 L1 Data         32K      cpus 0
index2 L2 Unified      4096K    cpus 0
index3 L3 Unified      16384K   cpus 0-3
4 threads (1 per core), 1.7 GiB of RAM
```

The host's L3 shows as a private 4 MiB L2 per vCPU, the L3 is invented, HT
is invisible and the VM has 1.7 GiB of the 8. From that the CLI chose a
2 MiB base segment (half of cpu0's "L2", arg_parser.hpp) that nothing
bounded below the sparse regime, a whole-L1d sub-block ("4 threads <= 4
cores") and, in the sparse regime, the 1/4 cutoff ("L2 per thread >= 1
MiB"). Last 1e10 below 1e13, 4 threads, primesieve 11.0 (the image's),
single runs:

| config | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| auto (2 MiB, no sparse tier, 524,288 medium primes) | 17.06s | 7.987s | 2.14x |
| `-s 3932160` (128 KiB) | 8.13s | 7.987s | 1.02x |
| `-s 7864320` (256 KiB) | 8.08s | 7.987s | 1.01x |
| `-s 15728640` (512 KiB) | 7.66s | 7.987s | 0.96x |
| `-s 31457280` (1 MiB) | 8.56s | 7.987s | 1.07x |
| 512 KiB, `--tune sparse=1/1` / `1/2` / `1/4` | 7.83 / 7.65 / 7.91s | | 0.98 / 0.96 / 0.99x |
| 512 KiB, `--tune medium_nta=1` / `0` | 7.53 / 7.76s | | 0.94 / 0.97x |
| 512 KiB, `ERATOSTENES_MED64_NTA=0` | 8.14s | | 1.02x |

(An earlier manual round through `make run` had the same shape: 2 MiB
14.78 s, 512 KiB 7.76 s, 256 KiB 8.12 s, 128 KiB 8.11 s against 7.62 s.)
primesieve picked a 128 KiB sieve there. The 2x was the segment alone; the
cutoff and the prefetch knobs are within single-run noise.

Kept: the automatic base width is capped at 32 x L1d (main.cpp, before
`sparse_regime` is evaluated, so the doubling and the sparse-regime ceiling
see the capped width; `-s` and `--l2-bytes` are left alone; the startup log
says when it applied and what sysfs claimed). Every real machine measured so
far already sits at or under it -- Emerald Rapids 1.5 MiB = 32 x 48 KiB
(the whole-L2 base rule's own bound), Xeon 2.80 1 MiB = 32 x 32 KiB, the HT
machines at 256-640 KiB -- so only a lying topology reaches it. Verified on
the dev PC: `-t 12` and `-t 2` unchanged (512 KiB), `--l1-bytes 4096` forces
the cap (128 KiB instead of 256 KiB) and `-s` still bypasses it; `make test`
green. On that VM the cap gives 1 MiB (1.07x), not the measured best 512 KiB
(0.96x): no rule fed by that sysfs can tell this VM (fake 4 MiB L2, real
256 KiB) from the Xeon 2.80 (real 1 MiB L2, 1 MiB measured best), which is
the case for calibrating the width at startup on a short probe instead of
deriving it from the topology.

Same day, the i5-13500 through `WIDTH=1e11 make docker-benchmark-mini` (20
threads, Docker Engine on the host kernel, real sysfs): auto 0.90x, 128 KiB
0.90x, 256 KiB 0.97x, 512 KiB (= auto) 0.95x, 1 MiB 1.30x, every cutoff and
prefetch knob 0.88-0.92x, the control pair 0.89x. Two things from it: the
machine slows ~25% for both programs after the first pair in every round
(2.55 / 2.825 s first, 3.1-3.6 s afterwards; the same at WIDTH=1e10), which
the pairing absorbs -- the old single-reference version of the script had
reported that drift as a 1.15x loss; and 128 KiB was the fastest eratostenes
run within the slow regime in both 1e11 rounds (3.11 vs 3.24 s for 256/512
KiB, and 3.03 vs 3.19-3.22 s), -4%, single runs: a proper interleaved A/B
at 20 threads is pending (256 KiB was +5.2% cycles against 512 KiB on the
P-cores alone at 1e14, 2026-09-28; 128 KiB was never tried).

**Same laptop, native (g++ on the host's Ubuntu, primesieve 12.12, 3a981b9,
`make benchmark-mini`, last 1e10 below 1e13, 4 threads).** Real sysfs now:
L1d 32K `cpus 0-1`, L2 256K `cpus 0-1`, L3 4096K `cpus 0-3`, 2 threads per
core, 7.2 GiB. The CLI chose 256 KiB (128 KiB base from the 256 KiB L2,
doubled in the sparse regime), a 16 KiB sub-block, medium prefetchnta on
and cutoff 1/1 -- the i5-11400F recipe, derived from the topology alone --
and the pairs read:

| config | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| auto (256 KiB) | 6.83s | 7.007s | 0.97x |
| `-s 3932160` (128 KiB) | 7.07s | 7.289s | 0.97x |
| `-s 7864320` (256 KiB) | 6.82s | 7.070s | 0.96x |
| `-s 15728640` (512 KiB) | 6.77s | 7.348s | 0.92x |
| `-s 31457280` (1 MiB) | 7.25s | 7.039s | 1.03x |
| 512 KiB, `--tune sparse=1/1` / `1/2` / `1/4` | 6.57 / 6.48 / 6.73s | 6.957 / 7.354 / 6.999s | 0.94 / 0.88 / 0.96x |
| 512 KiB, `--tune medium_nta=1` / `0` | 6.60 / 6.56s | 7.120 / 6.995s | 0.93 / 0.94x |
| 512 KiB, `ERATOSTENES_MED64_NTA=0` | 6.50s | 7.018s | 0.93x |
| auto (again) | 6.81s | 7.105s | 0.96x |

primesieve's own runs spread 6.96-7.35 s (5.7%), so single-run ratios carry
+-0.04x: the 0.88x is one of its slow runs. What holds: auto is 0.96-0.97x
on a 2010 Arrandale with no knob touched, the 2.14x of the morning was the
Docker Desktop VM's sysfs and nothing else, and 512 KiB (4 x the 128 KiB L2
share) with the cutoff at 1/2 was the fastest eratostenes run in absolute
terms (6.48 s, -5% on auto's 6.83 / 6.81 s), single runs -- the same
"segment past the L2 share pays under HT" shape as the dev PC, to be
confirmed with repetitions before a rule is touched (on the i5-13500 at 20
threads 1 MiB is 1.30x, so it would not be "4 x the share" in general).

Its tails, native, `WIDTH=1e10 REPS=2 make benchmark-tails` (3a981b9, auto
everywhere): 0.95 / 0.98 / 1.02 / 1.05 / 1.05 / 1.07x from 1e13 to 1e18
(BENCHMARK.md, own section). The usual shape -- ahead where the dense tiers
dominate, behind where the sparse tier does -- on a core with a 128-entry
ROB and a 4 MiB L3 for two cores; every rep of the two agrees within 3%.

The hint, interleaved (`B="-s 15728640 --tune sparse=1/2" make benchmark-ab`,
scripts/benchmark_ab.sh, new today, 8d9a3ed, same window): auto 6.88, 6.96 s
against 6.82, 6.69 s, -2.3%, every B run below every A run. Real, small,
and not one knob: at 512 KiB the medium-tier prefetchnta gate also turns
off (A "yes", B "no", same 131,072 medium primes), so B differs in segment,
cutoff and prefetch at once. Not worth a rule for the 128 KiB-share class
on one machine and one N -- the same "segment past the share" move is
1.30x on the i5-13500 at 20 threads -- and left as measured.

### One thread per core: the medium tier's per-call cost, and the sparse cutoff by active threads (2026-10-03, evening)

`perf record` (instructions:u, cycles:u, by symbol) of both programs on the
last 1e10 below 1e13 at `-t 1`, dev PC (i5-11400F, 512 KiB segment, no
sparse tier: isqrt(1e13) = 3.16M < seg_k_width 4.19M; primesieve's sieve
256 KiB, EratBig from ~786K):

| | eratostenes | primesieve |
|---|---:|---:|
| instructions / cycles / IPC | 15.39G / 9.61G / 1.60 | 17.00G / 8.22G / 2.07 |
| wall | 2.30 s | 1.95 s (1.18x) |
| small tier (p < 6K / < 9.8K) | 2.91G instr, 1.66G cyc | EratSmall 3.10G, 1.92G |
| p >= 6K | med64 4.28G / 3.24G + medium 7.62G / 4.28G = 11.9G / 7.52G | EratMedium 6.47G / 3.97G + EratBig 6.93G / 1.93G = 13.4G / 5.90G |
| presieve | 0.42G / 0.32G | 0.06G / 0.05G (AVX-512, partly unsymbolized) |

Fewer instructions than primesieve (-9.5%), 17% more cycles, and the whole
gap sits above p = 6K: 11% fewer instructions there for 27% more cycles.
The medium tier is 197,708 primes walked every one of the 636 segments,
126M calls at ~60 instructions and ~34 cycles each (state load, loop entry,
1-4 hits, the loop-exit mispredict, state store); EratBig takes the same
primes above 786K at IPC 3.6 with no per-segment cost for a prime that does
not hit. (The "+8.8% instructions at -t 1" of an earlier note isn't what
this window shows; the deficit is IPC, i.e. the call count.)

So the medium/sparse cutoff, again -- but this time at N below the sparse
regime, where the automatic cutoff is 1/1 and no sparse tier exists at all,
and on a thread count the gate (per-thread L2 share, 256 KiB here -> 1/1)
never looked at. `--tune sparse` vs auto, last 1e10 below N, interleaved
A/B x2 (scripts/benchmark_ab.sh), dev PC:

| threads | N | 1/2 | 1/4 |
|---:|---|---:|---:|
| 1 | 1e13 | -7.3% (4/4) | -10.5% (4/4) |
| 1 | 1e14 | -10.1% (4/4) | -12.4% (4/4) |
| 2 | 1e12 | | +1.0% (noise) |
| 2 | 1e13 | +0.8% (noise) | -7.6% (4/4) |
| 2 | 1e14 | -19.9% (4/4) | -13.8% (4/4) |
| 6 | 1e13 | -10.9% (4/4) | -3.8% (overlap) |
| 6 | 1e14 | +2.9% (overlap) | -1.5% (overlap) |
| 6 | 1e15 | | +3.3% (4/4 worse) |
| 12 | 1e13 | | +13.9% (4/4 worse) |
| 12 | 1e14 | | +13.0% (4/4 worse) |

Lowering the cutoff pays a lot with 1-2 active threads, fades at 6 and
hurts at 12 (the known shared-resource wall: bucket traffic against the L3
and memory bandwidth every active thread shares). Every machine where we
lose is a 1-2-thread-per-L3 machine (the 2-vCPU Xeons), and every
measured optimum fits an "L3 per active thread" reading: dev 12 MiB / 2 =
6 MiB -> 1/4, / 6 = 2 MiB -> 1/2 at 1e13 and no better than 1/1 at 1e15,
/ 12 = 1 MiB -> 1/1; i5-13500 24 MiB / 20 = 1.2 MiB with 640 KiB L2 -> 1/2
(the L2 gate); Xeon 2.80 33 MiB / 2 -> 1/4 (measured best); i7-620M 4 MiB
/ 4 = 1 MiB -> 1/1 (auto; 512 KiB with 1/2 was -2.3%). Candidate rule:
den = max(L2-share gate, L3-per-active-thread gate) with 1/4 from ~4 MiB
and 1/2 from ~3 MiB per active thread, applied below the sparse regime too
(a lowered cutoff creates the sparse tier; the power-of-2 fixup already
handles it). To be measured on the 2-vCPU Xeons at 1e13 (`--tune
sparse=1/4` vs auto: the below-the-regime half of the rule), on the Ivy
Bridge tower at 4 and 2 threads, and on the i5-13500 (where it must stay
at 1/2) before the rule goes in.

**Kept (same evening): the L3-per-active-thread gate.** tuning.hpp: 1/4
when the L3 (sysfs, cpu0's) divided by min(threads, its sharers) is at
least 4 MiB, on top of the per-thread-L2 gate; below the sparse regime only
when an octave of base primes lands in the sparse tier (base_limit >= 2 x
seg_k_width / 4), which is what the Emerald Rapids at 1e13 lacked. The new
automatic against the old behaviour (`--tune sparse=1/1`), dev PC, 1e10
windows, x2: `-t 1` 1e13 -10.0%, `-t 2` 1e13 -10.6%, 1e14 -9.2%, all 4/4;
`-t 2` 1e12 identical configurations, +4.2% "every B above" -- the noise
floor of a 1 s run. Unchanged where the gate is off: `-t 6` (2 MiB per
thread), `-t 12`, the i5-13500 (1.2 MiB), the HT laptops (1 MiB), and the
2-vCPU Xeons at 1e13 (the octave margin; at 1e14+ they already had 1/4 from
the L2 gate). The operators' round on three Emerald Rapids hosts the same
evening: `--tune sparse=1/4` at 1e13 -6.3% / +1.4% / -1.8% (the segment
shrinking 1.5 -> 1 MiB by the fixup, nothing else: every prime <= isqrt(N)
has 4+ hits in a 1.5 MiB segment), and `-DERA_MED_BANDS=1` +4..+13% at
1e13-1e15 on all three, +8..+10% with the cutoff raised to 1/2: the medium
bands are refuted on Emerald Rapids as on the i5-11400F. The i5-13500 at 20
threads with 1/4 at 1e13: +3.2% (3/3), the gate's lower end.

The i5-13500 at 20 threads, same evening, `make docker-benchmark-ab` x3,
1e11 windows: the 128 KiB segment (`-s 3932160`, the -4% hint of the two
mini rounds) is +6.5% at 1e13 (overlapping, the host's first-run drift) and
+5.4% at 1e14, every B run above every A: closed, the 512 KiB auto stays.
`-DERA_MED_BANDS=1` at 1e14: +9.0%, each B run above its A pair (3.66 vs
3.11, 3.93 vs 3.75, 4.02 vs 3.79 s). The bands are refuted on the i5-11400F,
three Emerald Rapids hosts and the i5-13500's P+E mix: the medium tier's
loop-exit mispredict is cheaper than the predicated iterations everywhere
measured. The flag stays as an A/B knob only.

**The i5-3470 tower (Ivy Bridge, 2012: 4 cores, no SMT, 32 KiB L1d, 256 KiB
L2, 6 MiB L3; Ubuntu from a live USB, primesieve 12.12), the same night --
the first one-thread-per-core machine on the metal.** Its tables
(BENCHMARK.md): counts 1.14 / 1.09 / 1.10 / 1.05x (1e10-1e13, single runs),
tails 1.01 / 0.99 / 1.00 / 0.95 / 0.92 / 0.92x -- the inverse of every HT
machine: behind in the dense regime, ahead from 1e16 up, on 256 KiB
segments (the doubled 128 KiB base) with cutoff 1/1. The L3 gate's
threshold, `--tune sparse` vs auto, 1e11 windows, x2:

| threads (L3 each) | N | 1/2 | 1/4 |
|---|---|---:|---:|
| 4 (1.5 MiB) | 1e13 | -3.6% (overlap: a 12 -> 13.4 s drift between reps) | +1.1% |
| 4 (1.5 MiB) | 1e14 | -5.1% (4/4) | -0.9% |
| 2 (3 MiB) | 1e13 | -4.6% (4/4) | -3.5% (4/4) |
| 2 (3 MiB) | 1e14 | -3.5% (4/4) | -1.0% |

1/2 wins at both thread counts and both N; 1/4 is behind 1/2 everywhere,
so the gate gets a 1/2 step from 1.5 MiB of L3 per active thread (inside
the sparse regime only: below it the margin for 1/2 is the regime itself).
Checked on the dev PC at 6 threads (2 MiB each), where the 1/2 step now
fires: 1e14 tail -2.1% / -3.5% against 1/1 in two rounds (4/4 both), 1e15
noise (+2.3%, overlapping). 12 threads (1 MiB) and the HT laptops stay at
1/1, the i5-13500 at its L2-gated 1/2.

The dense-regime loss on the tower (1.09-1.14x at 1e10-1e12) is not the
instruction set: the dev PC built with `-march=ivybridge` (no AVX2, no BMI)
against native is +1.7% at the 1e11 window and +2.4% at 1e12 (4/4), +0.0%
at the 1e13 tail. The `make benchmark-mini` round there was unreadable
(auto 1.14 s at the start, 1.34 s at the control: the live system drifts
17% within three minutes), and `perf` is closed by perf_event_paranoid = 4
on that kernel, so what the Ivy Bridge core does with the dense tiers --
the segment sweep hinted at nothing, every width within the drift -- stays
open until it can be counted.

The tower's dense-regime round the same night (x3, auto vs `--l1-bytes
32768`, last 9e10 below 1e11 and 1e11 below 1e12): +6.0% and +13.4%, every B
above A -- but `--l1-bytes` also switches the topology step off, and with
it the whole-L2 base rule, so B ran a 128 KiB segment with the 16 KiB
sub-block: the two changes are confounded, and the sub-block alone needs
`-s 7864320 --l1-bytes 32768`. `--tune sparse=1/4` at 1e12 (the primes from
524K to 1e6, 2-4 hits per segment, to the bucket ring): +1.1%, overlapping
-- a wash, so the dense loss there is not primesieve's EratBig split either.
The sub-block alone (`-s 7864320 --l1-bytes 32768`: 16 KiB on the same
256 KiB segment, x3): +1.7% at 1e11 (overlapping) and +3.7% at 1e12 (6/6
worse). The whole-L1d sub-block is right on the Ivy Bridge too. So the
tower's dense loss is not the ISA, not the cutoff, not the segment (128 KiB
was worse) and not the sub-block; what is left to sweep there without
counters is the small/med64 split and the medium prefetch.

The rest of the tower's dense-regime sweep (1e12, last 1e11, x3, auto vs
`--tune`): `small=1/2` -1.6% (overlapping), `small=1/8` +5.2% (6/6 worse),
`med64=0` +15.9% (6/6: the med64 tier earns its keep there), `med64=1/6`
**-3.8% (6/6)**, `medium_nta=1` +0.1%, `-s 15728640` -6.0% but overlapping
(one auto run at 10.33 s against 9.06 / 9.08). So the one knob that moves
the Ivy Bridge in the dense regime is the med64/medium split: at its 256 KiB
segment (seg_k_width 2.1M) the default 1/12 puts the band at 6K-175K; 1/6
takes it to 350K, which is where the default 1/12 lands on a 512 KiB
segment (4.19M / 12 = 350K): the measured optimum may be an absolute prime
(~350K, i.e. a hit count per call that depends on the segment width) rather
than a fixed fraction. On the dev PC `--tune med64=1/6` (band 6K-700K): 12
threads 1e12 tail -2.2% (4/4), 1e13 -1.1% (overlap), 1e14 +0.5%, 1e18 +0.9%
(x3, overlapping), full 1e12 count ABAB 25.37 / 24.63 vs 24.48 / 24.83 s; 2
threads 1e12 -7.2%, 1e13 -10.0% (overlapping) -- during a stretch where
the host ran 5-20% slower than in the morning for everything, so none of
the dev numbers is better than a hint. Candidate: med64 band up to
~350K-700K regardless of the segment (a hits-per-call floor instead of
1/12), to be settled with the operators (1 MiB segment: 1/6 would take the
band to 1.4M, a doubled double-buffered state) and the tower at 1/4.

The Xeon 2.80 (now identified: Cascade Lake, family 6 model 85 stepping 7,
AVX-512, 1 MiB L2; 1 MiB segment, so 1/6 takes the band to 1.4M, 106,410
med64 primes against 55,910), x3: 1e13 -0.8%, 1e14 -2.8%, both overlapping
(its reps spread 4-6%). The doubled band does not hurt on a 1 MiB segment
either. The i5-13500 (640 KiB share, HT pairs on a 1.25 MiB L2, where the
doubled double-buffered state is the risk) is the one machine still to ask
before 1/6 becomes the default.

**Kept (2026-10-04): med64 default 1/12 -> 1/6.** The i5-13500 (20 threads,
640 KiB share, 512 KiB segment, the doubled double-buffered state on HT
pairs), x3: 1e13 +4.4% but only through its usual fast first run (2.80 s,
then 3.27 / 3.37 against 3.30 / 3.29 / 3.29), 1e14 -0.5%: neutral. Four
architectures: -3.8% (6/6) on the i5-3470, -2.2% (4/4) at the 1e12 tail on
the i5-11400F and noise elsewhere there, -0.8% / -2.8% (overlapping) on the
Cascade Lake, a tie on the i5-13500. Never worse beyond noise; the default
moves to 1/6, `--tune med64=1/12` restores the old band.
Confirmed on the dev PC once the host was idle (the earlier dev numbers
were taken under an unexplained 5-20% slowdown of everything), new default
1/6 as A against `--tune med64=1/12` as B, x3: 4 threads 1e12 B +1.1%,
12 threads 1e13 B +1.2%, 1e12 B -0.8% (all overlapping), and the full 1e12
count ABAB x2 23.51 / 23.93 s (1/6) against 24.17 / 25.41 s (1/12). On the
i5-3470, `med64=1/4` at 1e12 is -4.3% (6/6) against the old 1/12, the same
as 1/6's -3.8%, and 1/6 at the 1e13 tail (sparse regime, cutoff 1/2) -0.7%
overlapping: the gain is the dense regime's, and 1/6 is where it saturates.
### `run_parallel_chunks`: steals priced with the run's own measurements (kept, 2026-10-02)

The fixed steal threshold (4 wheel indices per base prime) came from the dev
PC; on the i5-13500 it let only 4 steals through at the last 1e11 below 1e18
and left 7-9% idle (P-cores finishing ~1.3 s before E-cores). sieve_chunk now
times each fresh start's activation and each chunk's sieving (three clock
reads per chunk), and a thief takes the back piece, in whole chunks, that has
it and the victim finish together: activation + piece / its rate = (left -
piece) / victim's rate, the victim being the run whose own worker would take
longest. No steal when a single chunk doesn't pay its activation.
`--debug-idle` prints the measured activation cost and the range of rates.

i5-13500, 1e18 tail: 10 steals instead of 4, idle 3.1% instead of 7.0-9.1%,
rates 176-270 Mk/s (E- vs P-cores), but the wall time didn't move (7.48 s vs
7.36-7.47 s): each extra steal re-activates 50.8M primes (~0.75 s of one
thread at 14.7 ns each), which eats what the balance gains. Dev PC: same
decisions as the old threshold (activation measured ~3.2 indices per prime
at 1e18), A/B within noise. Kept: no loss anywhere, and it adapts by itself.

### Activation cost at the top of N (2026-10-02)

At the last 1e11 below 1e18 the dev PC sieved ~9% faster than primesieve per
wheel index but paid ~1.6 s more of fixed cost (T(W) = F + c*W over W = 5e10
and 1e11, 12 threads: F 3.6 s vs 2.0 s, c 2.05 vs 2.25 s per 1e10): the
single-threaded part of the base-prime phase (0.45 s) and ~1.5 s per thread
activating 50.5M sparse primes (29 ns each, ~130 cycles).

- Kernel time: a 1e18 run with a 1e9 window spent 10.6 s sys against 14.4 s
  user (12 threads, 1.42M page faults): each thread's bucket pool (406 MB of
  8-byte entries) is faulted in and zeroed as activation fills it.
  primesieve's EratBig holds the same entries (4.71 GB peak vs our 5.06 GB at
  the 1e11 tail), so it pays the same.
- Transparent huge pages for the pool (2 MiB-aligned arenas,
  `madvise(MADV_HUGEPAGE)`; WSL2 has `enabled=madvise`, `defrag=madvise`):
  faults 1.42M -> 0.22M but wall 2.3 s -> 4.3 s, sys 20.6 s -- direct
  compaction for every huge page, 12 threads at once. Micro-benchmark, 12
  threads x 406 MB written once: 4 KiB pages 0.8-0.96 s wall, THP 2.4 s,
  `MAP_POPULATE` 2.1-3.2 s (threads serialize in the kernel). Reverted.
- Activation in batches of 16 with a write prefetch of each target bucket
  tail before the pushes: 28-35 ns per prime vs 30-33, no change. Reverted.
- Base primes as a bitmap on the wheel (kept, e1b177a): `sieve_base_primes`
  sets bits (an atomic OR, neighbouring threads can share a word) instead of
  filling a `vector<uint64_t>`; classify lists only the dense tiers' primes
  and the sparse tier is a run of the bitmap (`SparsePrimes`) that activation
  walks with ctz. 1e9 (N = 1e18): 33 MB instead of 406 MB, sieved in 0.12 s
  instead of 0.41 s with 12 threads (no concatenation), and every thread
  reads 33 MB instead of 406 MB when activating (20-22 ns per prime instead
  of 23-31). Same primes as the old sieve for every limit up to 20000 and at
  sqrt(1e10..1e18) with 1, 3 and 12 threads, same `count_upto`; all `make
  test` cases pass. A/B, last 1e10 (fixed cost dominates), 4 runs each:
  1e17 2.29 -> 1.83 s (-20%, every new run below every old one), 1e18 3.75
  -> 3.30 s (-12%), i.e. ~0.45 s less; on the last 1e11 that is 2-3%, within
  that night's noise (1e15..1e18 ABBA x2: tie).

### `-t 2` gap with the VMs' cutoff (sparse 1/2): L2 misses in med64, slicing med64 per segment (tried, reverted, 2026-10-02)

The `-t 2` profile above was taken with the 1/1 cutoff; the sandbox VMs run 1/2
(their L2 per thread passes the 512 KiB gate). Same tail (last 1e11 below 1e15),
dev PC, `-t 2 --tune sparse=1/2`, perf stat (quiet machine):

| | eratostenes | primesieve |
|---|---:|---:|
| cycles:u | 147.1G | 129.6G (1.135x; 1.21x with 1/1) |
| instructions:u | 270.7G | 275.2G |
| branch-misses:u | 0.91G | 0.99G |
| l2_rqsts.miss:u | 6.57G | 2.39G |
| cycle_activity.stalls_l2_miss:u | 13.25G | 6.60G |

Fewer instructions and fewer mispredicts than primesieve; 2.75x its L2 misses.
By symbol (perf record on l2_rqsts.miss): process_big 42% (2.8G, EratBig 2.0G),
**med64 ~56% (3.7G, no counterpart: EratSmall isn't in primesieve's top 8)**,
medium 12% (0.8G vs 0.24G). med64 makes ~1.76M marks per 512 KiB segment, so
one mark in three misses L2: the segment is the size of the L2 (one thread per
core) and gets evicted while med64 scatters over it; primesieve sieves 256 KiB.
Shrinking the state instead doesn't help: `--tune med64=1/24` +2.7%, `small=1/2`
tie, `med64=0` worse (4 runs each, ABBA); a 256 KiB segment (`--l2-bytes 256k`)
+4.7% (medium and sparse pay twice the per-segment costs).

Tried: med64 alone in N slices of the segment (`--tune med64_parts=N`, each
slice re-filing the 384 lists by exit phase, the last one rebasing), medium and
sparse still seeing the whole segment. Counts OK. L2 misses: 7.9-8.1G -> 5.6G
(N=2), 5.0G (N=4). Cycles: `-t 2` 158.0G -> 159.9G (N=2), 162.7G (N=4);
`-t 12` 355-362G -> 381-390G (+8%), 434-441G (+22%). The misses go away and the
time doesn't: they are overlapped (same lesson as the L1/prefetch entries), and
each slice adds a re-filing pass over 29k entries that is paid for real.
Reverted. TopdownL1 under WSL's vPMU counts ~6M slots for a 150G-cycle run:
unusable here; the server (native) is where that question can be answered.

### Medium tier in fixed-iteration bands, predicated hits (tried, reverted, 2026-10-02)

The medium loop (`cross_off_medium`) leaves at a data-dependent `pos < end`:
one mispredict per prime per segment, 60% of all branch misses on the
i5-13500 at 1e14 and ~70% of its gap in TopdownL1 (entry below). Tried:
each class's list (sorted by p, so expected hits per segment fall along it)
cut at activation into bands with the same fixed iteration count h = expected
hits (6.857 x bytes / p) x factor + 1; a band's primes all run exactly h
predicated iterations (a hit past `end` marks a spare byte s[end] and doesn't
advance), the trip count constant across the band so the exit predicts; a
plain loop afterwards catches the rare prime with more hits (correctness) and
the primes expected above 8 hits (`--tune medband=a/b`, `medbandmax`).

First version with `hit ? pos : end`: GCC compiled the ?: back into a
`pos < end` branch per iteration (asm: 149 jcc, 18 cmov) -- branch misses
unchanged (1.36G -> 1.39G), cycles +6-8%. With arithmetic masks
(`m = 0 - (pos < end)`, `s[(pos & m) | (end & ~m)]`, `pos += step & m`) the
misses do go: 1e13 tail `-t 12` 1.31G -> 0.64G (-51%), 1e15 tail `-t 2`
(sparse 1/2) 0.98G -> 0.54G (-45%). Cycles, same runs: `-t 12` 172.7G ->
192.3G (+11%; factor 3: +44%), `-t 2` 154.0G -> 156.4G (factor 1.2, +1.5%),
162.7G (factor 1.4, +5.5%). The arithmetic matches: medium is ~75G of the
`-t 12` run, the mispredicts removed are worth ~11G, and the predicated loop
costs ~10 ops per iteration instead of 5 plus ~40% wasted iterations, ~+30G.
So on this core the medium exit mispredict is cheap -- overlapped with the
s[pos] misses the tier waits on anyway -- and the tier's cost is the work per
hit. Reverted. The same conclusion as the small/med64 "redirected stores"
attempt (erat_small.hpp entries): fewer mispredicts bought with more
instructions don't pay in any tier here.

### `-t 2` gap: Topdown from raw slot counters (2026-10-02)

WSL's vPMU breaks `-M TopdownL1` (bad-spec = all slots) but counts the raw
events. Last 1e11 below 1e15, `taskset -c 0,2`, 2 threads, dev PC, cutoff 1/1:

| | eratostenes | primesieve | delta |
|---|---:|---:|---:|
| TOPDOWN.SLOTS | 720.9G | 601.2G | +119.7G |
| UOPS_RETIRED.SLOTS | 300.8G | 315.6G | -14.8G |
| issued - retired (bad speculation) | 137.4G | 92.7G | +44.7G |
| slots not issued (front/back-end stalls) | 282.7G | 192.8G | +89.9G |

We retire fewer uops and still need 20% more slots: 37% of the gap is bad
speculation (INT_MISC.CLEARS 1.35G vs 1.06G), 63% stalls with nothing issued.
`cycle_activity.stalls_l2_miss` (13.25G vs 6.60G cycles, ~33G slots) is about
a third of those stalls; the rest is consistent with med64's scattered RMW
stores filling the store buffer (not counted as load stalls). Splitting
front- from back-end needs a native PMU (the server).

primesieve's own tier split at `-t 2` (its sieve is 256 KiB and EratMedium
stops at ~2.7 hits per segment, the rest going to EratBig): `--l2-bytes 256k`
with sparse 1/2, 1/4 and 1/8, against our 512 KiB with 1/2 (ABCDE EDCBA,
same tail): 164-170G cycles vs 152-160G, +5..+10% for every 256 KiB
combination (512 KiB with 1/4: tie). The split isn't what makes primesieve
faster per thread; halving our segment doubles the medium and sparse tiers'
per-segment costs, which EratMedium doesn't pay the same way. Not adopted.

Compiler: clang 19.1.7 (`-O3 -march=native -flto`) against GCC's build, ABBA
x2, cycles:u -- 1e11 `-t 12` 90.3-91.4G vs 89.8-90.9G, the 1e15 tail `-t 2`
152-173G vs 152-169G: a tie both times. The switch/fall-through loops don't
depend on which of the two compiles them.

### `MIN_SEGS_PER_CHUNK` as `--tune minsegs` (knob added, default kept, 2026-10-02)

The i5-13500 at 1e10 (1.09x) is a tail-balance loss: `--debug-idle` shows 6.2%
idle (0.169 s vs 0.190 s between threads), rates 408-1229 Mk/s -- at this N the
E-cores run 3x slower than the P-cores -- and the ~12 ms idle is about the
whole gap. The chunk floor of 4 segments (~9 ms on an E-core) sets the tail
granularity, and its reason (each chunk re-activated every base prime) is gone
since the sieve is carried across chunks. Dev PC (uniform cores), 1e10 `-t 12`:
`minsegs=1` idle 2.9-5.0% -> 1.8-2.8%, 636 chunks instead of 255, wall
unchanged (0.16-0.17 s); 1e11 unaffected (the floor doesn't bind: 1590 chunks
either way). Default left at 4 until the server measures 1 and 2.

### Per-tier cycles against primesieve at `-t 2`, and the sparse tier's block size: 4 KiB (kept, 2026-10-02)

Two `perf record`s (cycles:u, instructions:u) per program, summed by tier
(symbols -> small / med64 / medium / sparse / presieve; primesieve's
EratSmall / EratMedium / EratBig). Dev PC, last 1e11 below 1e15, `-t 2`,
sparse cutoff 1/2:

| | eratostenes | | primesieve | |
|---|---:|---:|---:|---:|
| small (<6K) + med64 (6K-350K) + medium (350K-2.1M) | 83.7G | | EratSmall + EratMedium (<768K) 63.9G, plus EratBig's share for 768K-2.1M (~30% of its hits, ~18G) | ~82G |
| sparse (>= 2.1M) | 57.9G | IPC 2.36 | EratBig for >= 2.1M (~70% of 60.8G) | ~43G, IPC 2.84 |
| presieve | 3.5G | | | 0.5G |
| total | 147.6G | IPC 1.83 | | 128.9G, IPC 2.13 |

Below 2.1M the two are even; the `-t 2` gap on the dev PC is the sparse
tier (+15G: the same instructions per hit as EratBig at a lower IPC). The
presieve line is not a gap: primesieve's shows as 0.5G only because its
AVX-512 pre-sieve kernels have no symbol and land in "other" (the September
1e11 profile put them at ~4.5%, i.e. ~6G here), so the two pre-sieves cost
about the same; ours vectorizes too (AVX2 `vporq`, checked in the asm) and
is load-bound (16 tables that don't fit L1). On the i5-13500
(native perf, P-cores 0,2, same tail): 119.1G vs 108.8G, we execute 7%
fewer instructions (266.8G vs 286.0G) at IPC 2.24 vs 2.63, L2 misses only
+20% (1.25 MiB L2), nothing reaches memory; process_big 38.3G against
EratBig's ~35G share for the same primes, the dense tiers 80.8G against
~74G. So the sparse tier suffers where the segment is the whole L2 (dev,
one thread per core), ties where it isn't. (An earlier reading here that
"our sparse beats EratBig by 23%" compared different prime ranges --
EratBig starts at 768K -- and was wrong.) The grouped Topdown events
(`cpu_core/topdown-*`) are "not supported" by the server's perf, so the
front/back-end split is still open.

The sparse tier's one untested knob in this regime was its block size:
1 KiB (128 entries) was picked over 8 KiB in September at 12 threads;
primesieve's buckets are 8 KiB. `-DERA_BLK_BYTES`, ABAB x4, cycles:u:

| | 1 KiB | 4 KiB | 8 KiB (ABC x2) |
|---|---:|---:|---:|
| 1e15 tail, `-t 2` (1/2) | 151.3G | **145.4G (-3.9%)** | ~150G |
| 1e15 tail, `-t 12` | 345.8G | **333.5G (-3.6%)** | 350-357G |
| 1e13 tail, `-t 12` | 163.4G | 164.9G (+0.9%) | |

4 KiB kept as the default (`ERA_BLK_BYTES` stays overridable): fewer
block boundaries per chain (the next-block prefetch, the pool push/pop)
at a pool footprint that still fits beside the segment, where 8 KiB
didn't. Confirmed where it matters most:

- Dev PC, `REPS=2 make benchmark-tails` (wall, best of 2): 1e15 7.59 ->
  7.09 s, 1e16 10.80 -> 9.65 s, 1e17 14.13 -> 12.84 s, 1e18 19.21 -> 18.28 s
  (-5..-11%) with primesieve within 1% of its previous times from 1e15 up;
  1e13/1e14 -4..-5% with primesieve also -3..-4% (host). Ratios 0.80 / 0.73 /
  0.74 / 0.77 / 0.81 / 0.86x.
- Emerald Rapids sandbox (2 vCPU, 1 MiB segment), f512e44 vs bfcf8d1
  interleaved on the same host, 2 runs each: 1e13 -1%, 1e14 -1% (noise),
  1e15 -2%, 1e16 -6%, 1e17 -6%, 1e18 -8%, every new run below every old one
  from 1e16 up. Ratios there 0.93 / 0.99 / 0.93 / 1.04 / 1.03 / 1.01x.

The i5-13500 at 20 threads is still unmeasured (its 1e14 full run of
2026-10-02, 3442 s against 3392 s, was the binary before this change).

### Two more `-t 2` probes on the sparse tier and the presieve (both tried, neither kept, 2026-10-02)

- Sparse wheel at `-t 2`: `--tune big2310=0` (mod-210 table, 3 KiB) against
  the default mod-2310 (15 KiB TABLE2310, competing for L1 with the segment
  lines and the bucket blocks): 1e15 tail, sparse 1/2, ABAB x3, 148.0G vs
  146.4G -- a tie; at `-t 12` the same tail is 334-346G vs 325-328G, the
  -4% that chose mod-2310 in September. The table isn't what lowers the
  sparse tier's IPC at two threads. Default kept.
- `Presieve::fill` in one pass over all 16 tables (dst written once per
  chunk) instead of 4 passes of 4 (dst read-modify-written 4 times): 1e10
  `-t 12` 7.39G -> 7.79G (+5.5%), 1e11 90.8G -> 94.8G (+4.3%), 1e15 tail
  `-t 2` 145.8G -> 148.6G (+2%). 16 streams in one loop lose more to
  vectorization/register pressure than the three extra dst passes cost.
  Reverted. fill's 4-table loop does vectorize (AVX2 `vporq`/`vmovdqu` on
  ymm); the cost is the 16 table streams, which primesieve pays too (see
  the per-tier entry above). Nothing left to take here short of fewer
  tables, and dropping coverage was measured worse than the fill it saves
  (each prime removed costs ~8x its share of the fill in small-tier hits).

### med64 tier crossed off per L1 sub-block (tried, reverted, 2026-10-04)

With the cutoffs of 2026-10-03 (sparse 1/4 at 1-2 threads, med64 1/6) the
med64 tier became the dominant one in the dense regime: `perf record` on the
dev PC, last 1e10 below 1e13, one thread, 44% of the cycles and 67% of the
program's L1 data misses -- 0.94G misses for 1.16G hits, 0.81 per hit, 3.2
cycles per hit against the small tier's 1.16, which marks inside an
L1-resident sub-block. The obvious move: cross the band's lower part off
per sub-block too (`process_med64s`: the same kernel and (class, phase)
lists, swapped once per sub-block, the entry rebased on the segment's last
sub-block; primes below 2 x the sub-block, 14+ hits each per sub-block, 64%
of the med64 hits for 17% of its primes). `--tune med64s=0` as B against
the band on, same window, interleaved:

| threads | N, window | band off vs on |
|---:|---|---:|
| 1 | 1e13, 1e10 | -13.5% (4/4) |
| 1 | 1e12, 1e10 | -20.7% (4/4) |
| 2 | 1e13, 1e10 | -11.3% (4/4) |
| 6 | 1e13, 1e11 | -9.4% (4/4) |
| 12 | 1e12, 1e11 (x3) | -24.6% (6/6) |
| 12 | 1e13, 1e11 (x3) | -16.0% (6/6) |
| 12 | 1e15, 1e11 | -10.5% (4/4) |

The band is 10-25% slower at every thread count. The L1 misses the
whole-segment med64 takes are overlapped (independent RMWs, a store per
~3 cycles), and what the sub-blocked path adds -- one list-entry copy and
one kernel entry per prime per sub-block, eight times the state traffic per
segment -- costs more than those misses ever did. Same lesson as the
sparse tier's re-file: a miss that overlaps is not a cost, and the stream
structure that lets it overlap is worth more than L1 residency. Kept as
`--tune med64s=a/b` (default 0 = off) for other cores; the default is the
whole-segment med64.

### Activation at the top of N: the integer division is not the cost (tried, reverted, 2026-10-04)

`--debug-idle` prints the activation cost per base prime. Dev PC, 1e18 tail
(50.8M base primes), 2 threads: 12.6 ns per prime, 0.64 s per thread -- 19%
of a 1e10 window, 2% of the standard 1e11 one, and presumably 2-3x that on
a core with a slow 64-bit divider (Nehalem, Ivy Bridge, Cascade Lake). The
obvious target was the `ceil(start / p)` integer division per prime in
file_sparse / activate_medium / activate_med64, replaced by a double
division with a +-1 fixup (~5 cycles of throughput against ~40): 16.6 ns
per prime instead of 12.6 (+32%), counts identical. On Rocket Lake the
divider is already fast and the 40 cycles per prime are the push of the
entry into a random ring slot (the first hit of consecutive primes lands
anywhere in the ring: a cache miss per prime), not arithmetic. Reverted.
The measurement that would say whether the division matters on an old
core is `--debug-idle` on the i5-3470 or i7-620M at the 1e18 tail; the
push cost is structural (primesieve's storeSievingPrime pays it too).

### Sparse tier: next-block prefetch spread over the current block (kept, 2026-10-04)

A fresh pass over the whole algorithm on the dev PC (i5-11400F, 12
threads, cycles:u) started from a profile by symbol: at the 1e12 window
med64 ~40% of the cycles, small ~31%, medium ~23%, presieve 4%; at the
1e15 tail `process_big` 32% (39% of the instructions), med64 27%, medium
27%. Per hit that is ~5 instructions and ~5 cycles in med64 (one L1 miss
per hit, the segment lives in L2), ~2.3 / 2.5 in the small tier, and ~38
instructions / ~40 cycles per hit in the sparse tier at 12 threads.

`perf annotate` of `process_big<true>` put **22% of its cycles on the
next-block prefetch loop** (`prefetcht1 0x40(%rax)` / `sub $-0x80,%rax`),
the 64 prefetches issued in a burst at every block boundary (kept
2026-09-27 as a win over no prefetch). Sixty-four outstanding L2 requests
exceed the core's miss queue, so the loop stalls until they drain: a
synchronous fetch of the next 4 KiB block dressed as a prefetch. The
other hot spots were the entry loads (the `and $0xffffff` after the
16-ahead `pe` load, 11%) and the table row (`movzbl %dh`, 9%).

Spread instead (ERA_BIG_PFSPREAD, now the default): inside the unrolled
loop, during entries 0..255 of the current block, prefetch line idx/4 of
the next block -- 64 lines over half a block, each requested twice (U =
2), never more than a couple outstanding. Interleaved A/B
(`benchmark_ab.sh`, 1e11 windows, REPS=3, every B run below every A run):

| tail | threads | burst | spread | delta |
|---|---:|---:|---:|---:|
| 1e14 | 12 | 5.35 s | 5.09 s | -4.9% |
| 1e15 | 12 | 7.33 s | 6.98 s | -4.8% |
| 1e16 | 12 | 9.88 s | 9.41 s | -4.8% |
| 1e17 | 12 | 13.12 s | 12.43 s | -5.3% |
| 1e15 | 6 (one per core) | 8.60 s | 8.03 s | -6.6% |

cycles:u at the 1e15 tail, A/B/A/B: 323.9 / 309.8 / 325.2 / 316.2 G
(-3..-4.4%) with +3.4% instructions (the per-iteration condition and
address). The dense regime (1e12 and below, no sparse tier) is untouched.
`-DERA_BIG_PFSPREAD=0` keeps the burst for an A/B on the other machines.

i5-13500 (20 threads, 2026-10-05): a mean-of-7 tails round with the
spread against the previous day's mean-of-5 with the burst, primesieve
+0.6..+2.5% between the two rounds: 1e13 3.16 -> 3.10 s (-1.9%), 1e14
3.85 -> 3.85, 1e15 4.45 -> 4.44, 1e16 5.16 -> 5.16, 1e17 6.19 -> 6.12
(-1.1%), 1e18 7.92 -> 7.74 (-2.3%). Neutral at 1e14-1e16, -1..-2% at the
two largest tails, no regression anywhere: the burst did not stall the
Raptor Lake cores the way it stalled the Rocket Lake ones.

i5-3470 (4 threads, one per core, 2026-10-05): `benchmark_ab.sh` at the
1e15 tail, spread 15.37 / 15.33 / 15.32 s vs burst 15.97 / 15.97 / 15.96 s,
**-3.9% (3/3)**, every spread run below every burst run. Ivy Bridge
stalled on the burst like Rocket Lake, with no HT to hide it. Pending:
i5-1235U, the Xeons; then a tails round on the tower and the laptop to
refresh their tables (both measured on 3a1218b, before the spread).

### Sparse tier: `process_big` is issue-bound at 12 threads; the ring's wrap mask and the spills are what is left (open, 2026-10-05)

Where the sparse tier stands after the prefetch spread above, from the
same dev-PC session (i5-11400F, 12 threads, cycles:u / perf annotate /
objdump of `process_big<true>`):

- ~47 instructions per hit in the unrolled U = 2 loop (95 per iteration),
  of which ~4 are the 16-ahead segment prefetch (`ERA_BIG_PF`, re-checked:
  `PF=0` +4.2%, keep) and ~2.5 the spread next-block prefetch. `UNROLL=1`
  +8.6% (3/3), `LOOP=1` -0.6% (noise): both knobs stay.
- At 12 threads the loop runs at ~20 cycles per hit per core for 2 x 47
  instructions, i.e. an IPC near the core's issue width: **the tier is
  instruction-bound with HT**, not memory-bound (with one thread per core
  it is latency-bound instead). Every instruction removed per hit is time
  at the tails.
- The loop body keeps 21 values live for 15 registers: `tails`, `bmask`,
  `modsb`, the next block's pointer and its flag are reloaded from the
  stack each iteration (`mov 0x8(%rsp)` x2, `mov 0x10(%rsp)`, `and
  (%rsp)` x2, `cmpb 0x20(%rsp)`), and the 36-bit mask is rematerialized
  (`movabs`) every iteration.

The one lever identified, not yet tried: **drop the ring's wrap mask**.
Today a hit's slot is `(cur + (pos >> log2sb)) & bmask`. With `head_` /
`tail_` arrays of 2 x num_buckets entries, `cur` kept below num_buckets
and the slot simply `cur + (pos >> log2sb)` (always < 2 x num_buckets, the
ring's own sizing guarantees ahead < num_buckets), the `and` goes and
`bmask` leaves the loop, freeing a register and one or two of the stack
reloads; once `cur` reaches num_buckets the two arrays are shifted down by
num_buckets (a memmove of a few hundred pointers, once every num_buckets
segments) and `cur` reset. Every slot user must follow: `process_big`,
`file_sparse` / `stage_sparse_entry` (`(cur_segment_ + ahead) &
(num_buckets_ - 1)` today), `new_block`, `begin_chunk`, `ERA_ACT_BATCH`'s
slot groups. Expected: 2-3 instructions of 47 per hit, ~1.5% at the
1e14-1e17 tails with HT, less without. Worth a session of its own, with
`test.sh` and the ring-margin exceptions as the safety net; not worth
doing at the end of a long one.

Rejected on paper the same night: re-ordering the packed entry (idx | pos
<< 12 | qp << 36). Whichever field sits in the middle costs a shift and a
mask, so moving `pos` to the top saves its mask and adds one on `qp`; the
repack stays at five operations. Zero change in instruction count.

Done (2026-10-05, laptop i5-1235U under WSL2: no PMU, so instructions by
callgrind and wall by short interleaved tails): **the wrap mask is gone,
kept.** `head_`/`tail_` hold 2 x num_buckets_ slots, `cur_segment_` stays
below num_buckets_ and a hit files into `cur_segment_ + ahead` as is (the
ring's sizing has ahead < num_buckets_ / 2 for a re-filed hit and
`file_sparse` already throws past num_buckets_ for an activation); when
the cursor reaches num_buckets_, `wrap_ring()` copies the upper half of
both arrays down and zeroes it (the slots below the cursor are drained by
then), once every num_buckets_ segments. callgrind on the 1e15 tail, 1e8
window, one thread: `process_big<true>` 233.6 M -> 228.6 M Ir (**-2.2%**),
`activate` 170.3 M -> 164.7 M (-3.3%, `file_sparse` lost its mask too).
objdump of the U = 2 loop: 75 -> 73 instructions per pair of hits, exactly
the two `and 0x8(%rsp)`; the reloads of `tails` (x2) and `modsb` and the
36-bit `movabs` are still there -- freeing `bmask` did not get GCC to keep
the rest in registers, so this is 1 of ~47 per hit, not the 2-3 hoped for.
Wall (1e10 windows, A/B interleaved): 12 threads x6 a tie at 1e14/1e15
and 1.74 -> 1.68 s median at 1e16; 4 threads x5 1.22 -> 1.21 (1e14), 1.42
-> 1.41 (1e15), 1.68 -> 1.66 s (1e16). Same counts throughout, `make test`
87/87, plus `-s 2000` at 1e10 (thousands of ring wraps) against primecount.
Kept on the instruction count; cycles:u on the dev PC still to be taken.

Validated the same evening with `scripts/perf_ab.sh` (A = b38e197 in a
worktree, B = 3234cb3, 1e10 windows, x3). The i5-3470 (4 threads, one per
core, instructions reproducible to 0.0% between reps): instructions:u
-2.7 / -3.2 / -3.4 / -3.5% at 1e14..1e17, 3/3 each; cycles:u +1.1% (0/3,
wall a tie at 1.30 s) at 1e14, -0.5%, -0.7%, -2.7% (3/3 each) at
1e15..1e17. The i5-13500 server (20 threads, inside the dev container with
`--cap-add SYS_ADMIN`, ~50 other containers running): the same binary's
instructions:u varies 30% between reps at 1e14 (233-304 G) -- under
contention a descheduled worker's run gets stolen and every steal
re-activates the base primes, so the work itself moves -- and only the
1e17 tail is conclusive, -6.3% cycles (4/4) with 1e11 windows; 1e14 -3.5%
(3/4), 1e15/1e16 +1..1.6% (1/4), wall B <= A at every N but one rep. No
machine loses; the issue-bound HT machines gain more than the Ivy Bridge.

Left in the loop, same session's reading of the asm: the per-iteration
`modsb` and `tails` reloads and the `movabs`. Tried next, both on the
laptop with callgrind:

- `(pos & modsb) << 12` as `pos - ((pos >> log2sb) << log2sb)` on the shift
  already taken for the slot (`ERA_BIG_SUBSHIFT`, removed again): the
  `modsb` reload goes, but `shlx + sub` is one instruction more per hit than
  the `and` and the reload was one per two hits: `process_big<true>` 228.6 M
  -> 231.1 M Ir (+1.1%). **Rejected on the count**, never measured in wall.
- `tails_cur = tail_.data() + cur_segment_` as the one pointer the loop
  keeps, a hit filing into `tails_cur[pos >> log2sb]`: neither the array
  base (reloaded from the stack per hit) nor the cursor (added per hit) is
  live any more; the slow path recovers the slot index as `tp -
  tail_.data()`. callgrind: `process_big<true>` 228.6 M -> 223.6 M Ir
  (**-2.2%**, another instruction per hit); the U = 2 loop 73 -> 71
  instructions, `lea (%r15,%rdx,8)` straight off the pointer in place of
  the `mov (%rsp)` reload and the `add`. Correct on the forced-sparse,
  mod-210, `-s 2000` and 1e15/1e16 tail checks. What is left per
  iteration: one `modsb` reload and the `movabs` of the 36-bit mask.
  **Committed for the cycles:u A/B on the i5-3470 and the server** (A =
  e7a4520, B = this, 4e03879).
- Two more on top of 4e03879, same method (laptop, callgrind, A = 4e03879):
  `pos & modsb` as one BMI2 `bzhi` taking the bit count from `log2sb`
  (`low_bits`, `#ifdef __BMI2__`, the `and` kept for Ivy Bridge and the
  portable build), and the spread next-block prefetch made unconditional
  by pointing the last block of a chain at itself (already cached) instead
  of testing `next_blk != nullptr` on every pair of hits -- that flag was a
  `cmpb` on the stack plus a branch per iteration. `process_big<true>`
  223.6 M -> 221.1 M Ir with `bzhi` alone (-1.1%), **-> 211.1 M with both
  (-5.6%)**; the U = 2 iteration 82 -> 78 instructions. One detour recorded
  so nobody repeats it: declaring `log2sb` as `uint64_t` so `shrx` and
  `bzhi` would share the count register made GCC keep two copies and spill
  `tails_cur` instead (back to 223.6 M); as `uint32_t` it keeps one
  zero-extended copy of the count on the stack for `bzhi` and the rest in
  registers. Register allocation in this loop is a lottery: every change
  needs its own callgrind and asm read, the source-level intent predicts
  nothing. `make test` 87/87. Committed for the A/B against 4e03879.

### Sparse tier: `process_big` in groups of 4 entries, no per-iteration edge tests (kept, 2026-10-06)

The U = 2 loop in the laptop's objdump (GCC 13, ffd8e7c) was 82-86
instructions per pair of entries, ~42 per hit, and 15-19 of them per pair
were about the edges of a block, paid on every iteration: the next-block
spread's `idx < 256` test and address (its line pointer reloaded from the
stack), the `it + ERA_BIG_PF + U <= end` test before the 16-ahead segment
prefetch, and `end` itself compared from the stack. `ERA_BIG_FASTBLK`
(default 1, `=0` restores the single loop): the number of groups of 4
entries whose 16-ahead neighbours are still inside the block is worked out
once per block; the first min(groups, 64) groups also prefetch line g of the
next block (one request per line, not two), the rest only the segment
bytes, and the last few entries go through the plain pairs. The pair body
is unchanged, now an always_inline lambda both loop shapes share.

objdump: 150 instructions per group of 4 in the spread loop, 147 in the
other, ~37 per hit. callgrind, one thread, 1e8 windows, A = ffd8e7c:

| tail | A `process_big<true>` | B | delta | program total |
|---|---:|---:|---:|---:|
| 1e15 | 211.1 M Ir | 187.1 M | **-11.4%** | -3.6% |
| 1e17 | 331.2 M | 293.6 M | **-11.4%** | -1.0% |

Counts identical between A, B and B with the knob off, and to primecount:
1e10 with `-s 2000` and `-s 100000`, 1e11 `-s 2000000 -t 5`, the last 1e9
below 1e15, 1e16, 1e17 and 1e18, the unaligned 2e16 start; `make test`
passes. Wall on the laptop under WSL (1e10 windows, x5, 12 and 4 threads):
-3.3% .. +7.4%, every row overlapping (one binary spread 1.14-1.96 s within
a series): no instrument for this. Two leftovers in the asm: the spread's
line pointer still lives on the stack (deriving it from `it` instead made
GCC strength-reduce it back into the same stack slot, `addq $0x40`: -0.3%
Ir, not kept), and the spread loop round-trips one table row through the
stack to extract dm with `movzbl %ah` (the other loop doesn't). Expected:
a few percent at the 1e15+ tails where `process_big` is issue-bound (HT
pairs, 30-39% of the cycles there), little with one thread per core, where
it is latency-bound.

i5-3470 (4 threads, one per core; b53a814 against its own
`-DERA_BIG_FASTBLK=0` build, which callgrind puts within 42 Ir of ffd8e7c's
`process_big<true>`; `scripts/perf_ab.sh`, last 1e10 below N, x5, wall only:
perf_event_paranoid was 4 on that boot):

| tail | A (knob off) | B | delta | pairs B lower |
|---|---:|---:|---:|---:|
| 1e15 | 1.53-1.59 s | 1.51-1.52 s | -1.94% | 5/5 |
| 1e16 | 1.86-1.87 s | 1.83-1.86 s | -0.54% | 5/5 |
| 1e17 | 2.41-2.45 s | 2.36-2.42 s | -0.82% | 5/5 |
| 1e18 | 3.48-3.60 s | 3.50-3.52 s | -0.85% | 4/5 |

Small, and in the same direction at every N, on the one-thread-per-core
core where the loop is latency-bound.

i5-13500 (20 threads, HT pairs; inside `eratostenes:dev` with `--cap-add
SYS_ADMIN`, A = the same image's `-DERA_BIG_FASTBLK=0` build), last 1e11
below N, x4, wall:

| tail | A | B | delta (means) | pairs B lower |
|---|---|---|---:|---:|
| 1e15 | 3.72, 4.44, 4.42, 4.38 s | 4.26, 4.26, 4.26, 4.40 s | +1.3% (-2.4% without A's first run) | 3/4 |
| 1e16 | 5.11, 5.04, 5.01, 5.18 s | 5.05, 5.03, 5.03, 4.99 s | -1.2% | 4/4 |
| 1e17 | 5.93, 6.03, 6.41, 5.98 s | 5.70, 5.78, 5.85, 5.88 s | **-4.7%** (every B under every A) | 4/4 |
| 1e18 | 7.59, 7.35, 7.47, 7.58 s | 7.42, 7.28, 7.41, 7.40 s | -1.6% | 4/4 |

A's 3.72 s at 1e15 is the machine's usual fast first run (the burst
regime, see the Makefile section). The perf counters of one binary spread
21% and 38% between reps at 1e15 and 1e16 (as in the 2026-10-05 entry on
this machine), so those two N say nothing in cycles (-15.6% and +0.6%);
at 1e17 and 1e18, with a 5-7% spread, instructions:u -8.0% and -7.6% (4/4
both) and cycles:u -2.5% (2/4) and -2.2% (4/4). **Kept** (default on):
no N worse beyond its noise on either machine, -0.5..-1.9% with one thread
per core, -1.2..-4.7% wall at 1e16-1e18 with HT pairs, where the loop is
issue-bound as predicted.

### Activation at the top of N on old cores: 83 ns per prime on Nehalem, two flags to split it (open, 2026-10-04)

The measurement above, taken: i7-620M (Nehalem, 2010), 1e18 tail, 1e11
window: **83.0 ns per prime at 2 threads, 128.7 at 4** (HT pairs), against
11.6-12.6 on the i5-11400F -- 7x, not the 2-3x a slow divider alone would
give, and growing with the thread count, which is the signature of the
memory side (the ring's ~4096 tail lines plus the `tail_` array do not fit
a 256 KiB L2; at 4 threads the two HT siblings share one). At 50.8M primes
that is 4.2 s per thread at -t 2 and 6.5 s at -t 4: 3-5% of the Mac's
120-134 s tails, most of its 1.10x there. Two compile-time flags split
the two suspects, both default off, both leaving counts identical on the
1e18 tail and on a mod-210 (`--tune big2310=0`) 2e16 tail:

- `-DERA_FPDIV=1`: the first multiplier via a double division plus an
  exact fixup (`file_sparse` only; the dense tiers' activation is cheap).
  Dev PC (fast divider): 16.2 ns per prime instead of 11.6 -- the reverted
  result above, now kept as a knob for the old cores.
- `-DERA_ACT_BATCH=1`: the activation's pushes staged in a per-thread
  buffer of 32K entries and flushed grouped by `slot >> 6` (one counting
  pass, then 64 slots at a time, so a group's 64 tail lines and 8 lines of
  `tail_` stay in L1). Dev PC: 12.1 ns per prime (neutral, as expected
  where the tail lines already fit the 512 KiB L2).

`make variant DEFS=-DERA_FPDIV=1` and `BIN_B=./eratostenes_variant make
benchmark-ab` at the 1e18 tail on the i7-620M and the i5-3470 decide
which one, if any, becomes the default below some L2 size.

Results as they come in (1e18 tail, 1e11 window, x2 each):

| machine | FPDIV | ACT_BATCH |
|---|---:|---:|
| Xeon @2.10GHz, 2 vCPU, 96 KiB L1d, 4 MiB L2 (operator 2) | +0.0% (overlap) | +1.6% (2/2 worse) |
| Xeon @2.10GHz, 2 vCPU, third host (operator 3) | -0.1% (overlap) | +0.6% (overlap) |
| Xeon @2.10GHz, 2 vCPU, other host (operator 1; A spread 7%) | +0.4% (overlap) | +1.0% (overlap) |

Expected there: a fast divider and an L2 that holds the whole ring's tail
lines, so the staging pass is pure overhead.
On the 1e10 window (x3, operators 2 and 3, same numbers on both hosts):
ACT_BATCH +8.2% (3/3 worse), 3.88 -> 4.20 s -- the same +0.32..0.44 s as on
the 1e11 window, i.e. a fixed cost of ~6 ns per activated prime per
thread, not a per-segment one: the first version staged every entry in
one 32K buffer and counting-sorted it (4 memory operations per entry).
Operator 1 also settled a scare: b38faee vs 94f93af at the 1e18 tail x3,
+1.5% overlapping, the host had drifted. Rewritten as one buffer of 512
entries per group of 64 slots, drained when it fills (one sequential
append and one read per entry): dev PC 13.4 ns per prime vs 11.5, counts
identical on both wheels; the Mac decides.

The Mac (x2, 1e18 tail): FPDIV -0.4% (overlap), the first ACT_BATCH +3.1%
(2/2 worse). The i5-3470: 20.0 ns per prime at 2 threads, 24.8 at 4 --
twice the dev PC, not seven times; FPDIV +1.2% and ACT_BATCH +0.3%, both
overlapping. So the division is closed on every architecture, and the
Mac's 83 ns are its own problem. Best explanation: cache-set aliasing of
the ring's tail lines. Every slot fills at about the same rate, so the
thousands of tail pointers sit at the same offset inside their 4 KiB
blocks, with the same set-index bits 6-11: a 256 KiB 8-way L2 can hold 64
of the 4096 tail lines, the Mac's 4 MiB 16-way L3 1024 -- three pushes in
four go to DRAM. `-DERA_BLK_COLOR=8` starts a block's entries 0-7 lines
past the header by its address (5.5% less capacity per block). Dev PC, 2
threads: 1e18 with a 1e10 window -2.7% (3/3) with 8 colours, -2.9%
(overlap) with 16; with the 1e11 window +1.9% (2/2 worse): the activation
gains, the sieve phase pays the extra blocks where the lines already fit
(512 KiB L2, 12 MiB L3). To settle on the Mac and the server (1.25 MiB L2
holds 320 of the 4096 lines).

Colouring, 1e18 tail, 1e11 window: i5-13500 (20 threads) **+6.3% (2/2)**
and +5.1% (2/2) at 1e17; i5-3470 +3.1% (2/2); Xeon @2.10GHz +1.4% and
+3.1% (both overlapping, x3); Xeon @2.80GHz (Cascade Lake, 2 MiB L2)
-1.4% (3/3). Refuted as a default, and the reason is instructive: the
aliasing is a cheap cache partition during the sieve phase. With every
tail line in the same set-index bits, the ring's write stream can only
occupy 1 of the 64 L1 sets and 8 of the 512 L2 sets, so it never evicts
the segment or the consumer stream; coloured, the same stream spreads
over 8x the sets and the sparse marking pays it on every machine with a
small L2 share. The activation (one pass, 2-3% of a tail) is the only
phase that gains. Kept as a knob.

i5-13500 activation: 7.6 ns per prime at 6 threads, **18.6 at 20** (HT
pairs: 2.4x) -- 0.95 s of a 7.2-7.9 s 1e18 tail, about 12%, the largest
share of any machine; 1e17 (17M primes) about 5%. The per-group batch is
untested there.

i7-620M, 1e18 tail x2: colouring +0.4% (2/2, 0.5 s), per-group batch
+0.8% (overlap). Neither moves its 83-129 ns per prime, so that cost is
not the tail-line RFO at all (and not the division: FPDIV -0.4%). What is
left in the activation path is the kernel: the ring's 406 MB per thread
are first-touched there (1 MiB arenas, huge pages gated off on HT pairs;
`--tune huge=1` gave ~-1% earlier), a page fault per 4 KiB page on a 2010
laptop, or something only a profile shows. Next: `perf record` without
`:u` on a 1e9 window of 1e18 at 2 threads, where the activation is 3/4
of the run.

i5-1235U (WSL, 3.7 GB), 1e17 tail at 8 threads x3: per-group batch -1.5%
(overlapping; the runs drift 14.4 -> 16.8 s, laptop power limits), 16.0
ns per prime without it, 18.0 with. HT pairs do not rescue it either.

**Resolved (i7-620M): it is the clock.** `perf record` on a 1e9 window of
1e18 at 2 threads: 53.8% in `activate` (user code, file_sparse inlined),
23.5% in the base-prime sieve, 11.2% in process_big, 0.3% kernel -- no
page faults. 9.5 G cycles of activation over 2 x 50.8M primes = 94 cycles
per prime, i.e. 35 ns at the nominal 2.67 GHz, yet 83 were measured.
`perf stat`: 16.5 G cycles:u over 14.28 s of task-clock = **1.16 GHz**.
The machine runs at well under half its nominal clock (no battery: the
MacBook's SMC caps the CPU when the adapter is the only supply). Every
Mac number in BENCHMARK.md is a 1.2 GHz Nehalem, primesieve's included,
so the ratios stand; the 83 ns were never a code problem. The three
activation experiments (FPDIV, ACT_BATCH, BLK_COLOR) are closed on every
machine; the flags stay as knobs.

i5-13500, 20 threads, x3: the per-group batch **+7.7% (3/3)** at the 1e18
tail, +3.6% (3/3) at 1e17, 20.5 ns per prime against 18.6. Refuted on the
one machine where the activation weighs 12%: HT pairs or not, the group
buffers are extra traffic through an L2 that the direct push does not
need. ERA_ACT_BATCH closed everywhere.

### Sparse activation from the bitmap index: 18% fewer instructions per prime (kept, 2026-10-05)

Line-level callgrind (`make variant DEFS=-g`, 1e17 tail, 1e8 window, one
thread, 17 M sparse primes activated) put `activate` at 1.50 G Ir, ~88
instructions per filed prime, and named the avoidable ones: `ri =
WHEEL_POS[p % 30]` 153 M (a multiply-shift modulo and a table load),
`(p / 30) << 36` inside the entry pack 153 M (another), `(1 << log2_sb_)
- 1` rebuilt per prime, and `if (p * p >= high_n)` 89 M (a multiply, a
compare and a branch per prime, plus `wheel_number(k)` for a prime that
may not activate). The bitmap walk already has the index: for the mod-30
wheel `k >> 3` is `p / 30` and `k & 7` the residue class, so `file_sparse`
now takes `k`, derives `qp`, `ri` and `p` from it and never divides by 30;
`sb_mask_` is a member; and the per-prime square test became one `isqrt`
per segment -- `k_cut = wheel_count_upto(isqrt(high_n - 1))` bounds the
walk, the partial last word is masked once. `activate` 1.50 G -> 1.23 G Ir
(**-18.2%**, ~72 per prime); `process_big` unchanged. Correct on both
wheels, `-s 64`/`-s 2000`, the 1e15-1e17 tails and the unaligned 2e16
start; `make test` 87/87. Where it shows: every fresh start of a worker
and every steal at the top of N (50 M primes at the 1e18 tail, ~1.3 s per
thread on the dev PC before this), i.e. the tails' startup and the steal
price in `run_parallel_chunks`. The same profile puts the base-prime
sieve's `comp[k] = 1` at 31% of that window (1.3 G for 17 M primes up to
3.16e8); it runs once per run and in parallel, so it was left alone, but a
mod-3 or presieved inner loop there is the next obvious cut if the
1e17-1e18 startup ever matters.

What is left in `file_sparse` per prime, by line: the `m = ceil(start / p)`
division (the real 64-bit `div`, ~4 instructions but the latency chain),
`m / 2310` and `m % 2310` (5), `p * m / 30` (7, a 64-bit multiply and a
constant division), the two table loads and the ring push (~10). The
division by p was already shown not to be the cost on modern cores (the
`ERA_FPDIV` entry above).

Measured the same evening (`scripts/perf_ab.sh`, A = e7a4520, B = d7d2203,
1e10 windows). **i5-13500** (20 threads, in the dev container, x7):
cycles:u -3.9% (7/7) at 1e14, -0.5%, -3.3% (7/7), -1.6%, **-7.3% (7/7) at
1e18** with A's spread 1.3%; instructions -7.4 / -3.0 / -5.3 / -6.0 /
-11.6%. **i5-3470** (4 threads, x5): instructions -2.9 .. -6.5% (5/5) as
predicted, cycles **+0.8, +1.3, +2.3, +6.5, +15.5%** (0/5 at every N),
wall 3.43 -> 3.92 s at 1e18. Per-commit bisect there (1e17/1e18, x3):
4e03879 and 25f5c75 are ties (-0.1 / +0.2%), d7d2203 alone is the +7.7% /
+15.8%. `perf stat` with the memory events: L1 misses, dTLB misses and
branch misses unchanged, LLC loads *down* (721 M -> 587 M); `perf
annotate` of `activate` puts 34% of its samples (20% before) on the
`cmpb` right after the 64-bit `div` -- the skid of the division. Same
operands, ~35 more cycles per division. Split into two knobs
(`ERA_ACT_KCUT`, `ERA_ACT_IDX`, eff26f6) and bisected on the i5-3470 against
HEAD (both on), x3: KCUT off +0.1 / +0.6% cycles (the isqrt bound is the
better of the two there as well); **IDX off -7.1% / -13.1% (3/3)**, both
off -7.3 / -13.3%. So the whole loss is deriving `qp` and `ri` from the
index -- the same `q * 30 + R[j]` arithmetic `wheel_number` does, but
without the independent `p / 30`, `p % 30` and table work that used to sit
beside the division. Why removing independent work beside a non-pipelined
divider costs Ivy Bridge 35 cycles per prime is not explained here, only
measured (the 2026-10-04 entry above has the same core 83 ns per prime on
Nehalem for the same loop). **Kept gated:** `ERA_ACT_IDX` defaults to 1
only with `__BMI2__` (Haswell and newer), 0 otherwise -- the tower and the
portable build keep the old derivation, `ERA_ACT_KCUT` stays on
everywhere. Unmeasured in between: Haswell to Skylake clients. Confirmed on
the i5-3470 (5f7d213 vs 25f5c75, 1e17/1e18, x3): cycles +0.5% / +0.3%
inside A's spread, instructions -0.8% / -1.6% -- a tie, the regression
gone; the i5-13500's binary is unchanged from the d7d2203 measurement.

### i5-3470 profile at 1e12: the med64 tier over the whole-L2 segment is 59% of the cycles (open, 2026-10-04)

First PMU profile of a dense-regime loss on a one-thread-per-core machine
(Ubuntu live USB, `perf stat`/`perf record -e cycles:u`, 1e12, 4 threads,
primesieve 12.12 with its 128 KiB sieve):

| | eratostenes | primesieve |
|---|---:|---:|
| cycles:u | 1008 G | 952 G |
| instructions:u | 1159 G | 1359 G |
| IPC | 1.15 | 1.43 |
| branch-misses:u | 7.6 G | 10.1 G |
| L1-dcache-load-misses:u | 119.6 G | 106.2 G |
| wall | 82.15 s | 76.50 s |

We retire 15% fewer instructions and 25% fewer mispredicts and still lose
6% of cycles: the gap is memory, not work. By symbol: `run_med64` 29.7%
plus `process_med64<0..3>` 29.4% (the other four classes are inlined into
`run_med64`) = **59% of our cycles in the med64 tier**, small tier
(`cross_off<PR>`) 21%, presieve 4.9%, medium under 2.4% per class.
primesieve: EratMedium 55.7%, EratSmall 23.9%, EratBig 11.9%, presieve
5.6%. The tiers do not line up: here the med64 tier takes the primes from
~3.8K (526 small primes) to ~350K and runs over the whole 256 KiB segment
(the whole-L2 base rule), so every one of its ~1e11 hits is an L1 miss
(that is where the 119.6 G come from) served by an L2 that also streams
the 470 KB of double-buffered med64 state and the medium state every
segment -- on Ivy Bridge, with a 168-entry ROB, that latency is not
hidden the way the i5-11400F hides it. primesieve's EratSmall covers up
to ~23K (0.175 x its sieve size) and runs L1-blocked, so the 3.8K-23K
band that costs us ~6 cycles per hit costs it under one. The sub-blocked
med64 band (`--tune med64s=a/b`, refuted on Rocket Lake, see above) is
exactly that band on our side; the i5-3470 is the machine it was built
for. Pending: `--tune med64s=1/4|1/2|1` A/B at 1e12 there, and `perf
stat` with the L2 request events to confirm the L2 misses.

Follow-up (same day). `--tune med64s=1/4|1/2|1` at the 1e12 tail: -1.2%,
-1.2%, 0.0%, all overlapping -- the L1-blocked band is not it. `perf
stat` at 1e11, 4 threads, is:

| segment | cycles:u | L2 demand reads | L2 hit | LLC-loads | wall |
|---|---:|---:|---:|---:|---:|
| 256 KiB (auto, whole L2) | 80.0 G | 9.74 G | 82% | 1.78 G | 5.96 s |
| 128 KiB | 76.8 G | 8.85 G | 94% | 0.52 G | 5.73 s |
| 64 KiB | 85.1 G | 7.37 G | 94% | 0.43 G | 6.34 s |
| primesieve (128 KiB sieve) | 74.5 G | 8.31 G | 97% | 0.22 G | 5.55 s |

The whole-L2 segment is the pathological size on a 256 KiB 8-way L2: its
4096 lines are all 8 ways of all 512 sets, so every line of med64/medium
state streaming through evicts a hot sieve line, which misses on its next
touch and evicts another -- 1.78 G L2 misses, 34 per sieve line per
segment. Half the L2 leaves 4 ways per set for the streams: -71% L2
misses, -4% cycles at 1e11. But at the 1e12 tail `-s 3932160` is **+12.1%
(3/3)**: the med64/medium cutoff scales with the segment (29375 med64 +
48559 medium become 15333 + 62601) and the medium tier, which is
per-segment-call bound, gets 14K more primes and twice the segments. The
segment and the band have to move together: pending `-s 3932160 --tune
med64=1/3` (same med64 population as the auto) and `1/2` at 1e12.

Done: with the populations equalised (`-s 3932160 --tune med64=1/3`:
29375 med64 + 48559 medium, same as the auto) the 1e12 tail is still
**+9.0% (3/3)**, `1/2` +5.4% (3/3); `perf stat` on that tail: L2 misses
2.03 G -> 0.69 G (-66%) and cycles 111.7 G -> 120.1 G (+7.5%). So the
cascade is real but hidden (overlapped), and halving the segment doubles
the per-segment visit of every med64/medium entry (78K primes x 12.7K
extra segments = 1e9 visits for 8.4 G cycles, ~8 cycles each), which
costs more than the misses ever did. Half L2 only pays while the base
primes are few (1e11: -4%). The whole-L2 rule stands on the i5-3470; its
dense loss is not the L2 either. Left: the med64 kernel's ~6 cycles per
hit against primesieve's EratMedium -- `perf annotate` is the next look.

`perf annotate` (1e12 tail, cycles:u, samples summed by mnemonic inside
the kernel): `process_med64<0>` add 73%, lea 14%, cmp 8%, orb 0.1%;
`run_med64` (the inlined classes) lea 85%, cmp 8%, orb 0.7%;
primesieve's `EratMedium::crossOff_7` add 73%, cmp 11%, andb 2.5%. With
skid the sample lands on the instruction after the one that stalls, and
in both kernels that is the add/lea right after the byte RMW: both are
bound by the segment-byte read-modify-write missing L1, at the same
shape. Per hit the kernels are alike; what differs is how many hits each
program sends through a non-L1 kernel (primesieve's EratSmall runs
L1-blocked up to ~23K, ours to ~3.8K) and how many per-segment visits
(our separate medium tier, 48559 primes at 1e12, is the generic
per-prime stepping kernel primesieve does not have: it keeps its 64-list
design up to the EratBig limit). The band experiments (small=1/2,
med64s) were neutral, so the hit count below 23K is not it; the medium
tier is the remaining structural difference -- `--tune med64=1/2|1/1`
(moving it into the med64 design) is the next A/B on the i5-3470.

Done, and it splits by machine. i5-3470 (x3, 1e11 windows): `med64=1/2`
**-5.9% (3/3)** at 1e12, `1/1` (no medium tier at all) -6.7% (3/3),
`1/2` at 1e13 -4.5% (3/3). i5-11400F (12 threads, x3): `1/2` +7.7%
(3/3) and `1/1` +8.1% at 1e12, +11.5% / +13.1% at 1e13. The two tiers
rank the opposite way on the two cores: the medium tier's generic
per-prime stepping (table-driven, a load on the position chain) is what
Ivy Bridge pays and Rocket Lake hides, while the med64 double-buffered
state traffic is what Rocket Lake pays. So the med64 fraction is a
per-microarchitecture setting, not a cache-size one: 1/6 on the modern
cores, the whole segment on the i5-3470. Pending on the i5-13500,
i5-1235U and the Xeon VMs before choosing the gate (L2 <= 256 KiB is the
candidate proxy for "old core", with the caveat that Skylake-class
clients have 256 KiB too and are unmeasured).

The rest of the fleet, `med64=1/2` x3 (1e12 / 1e13 tails): i5-13500
+7.6% / +3.2% (both overlapping, the host drifting); i5-1235U +4.5%
(overlap) / -2.6% (3/3, but confounded: the auto had the medium-tier NTA
on and the 1/2 run, with fewer medium primes, had it off); Xeon
@2.80GHz 1 MiB L2 -3.4% / -2.3% (overlap); Xeon @2.10GHz identical at
1e12 (1/6 of its 1 MiB segment already takes every prime) and -1.1%
(overlap) at 1e13; Xeon @2.80GHz 2 MiB L2 +3.1% (3/3, by 0.02 s) /
-1.8% (overlap). Only the i5-3470 moves clearly, only the i5-11400F
clearly the other way. **Kept as a gate on the physical L2: 1/2 when it
is 256 KiB or less** (tuning.hpp, `med64 cutoff:` startup line; `--tune
med64` overrides; `--l2-bytes` counts). 1/2 over the whole segment (1/1)
because 1/2 is the one measured in both regimes on the i5-3470. Pending
there: the new auto against `--tune med64=1/6` and a fresh table.

Validated on the i5-3470 (62fc592, x3): the new auto against
`--tune med64=1/6` **-5.4% (3/3)** at the 1e12 tail and **-5.6% (3/3)**
at 1e13; `1/1` against the new auto at 1e13 +0.1% (overlap), so 1/2 and
the whole segment are the same there and 1/2 stays. Fresh tables
(REPS=2): counts 1.12 / 1.06 / 1.02 / 1.01x (were 1.12 / 1.14 / 1.09 /
1.07), tails 0.98 / 0.91 / 0.82 / 0.85 / 0.92 / 0.91x (were 1.02 / 1.05 /
0.99 / 1.00 / 0.91 / 0.97). The whole-L2 segment plus this cutoff turned
the tower's dense regime from a 7-14% loss into a tie, and its 1e14-1e16
tails gained 9-17%. Left there: 1e10 (0.49 vs 0.44 s: 50 ms of startup,
unprofiled) and 1e11 (1.06x: the only range where half the L2 measured a
gain, -4% in cycles, before the cutoff moved).

1e10 profiled: nothing to see -- med64 59%, small tier 32%, presieve 7%,
thread start 2.4%; the 50 ms are the same per-segment shape as 1e11.
Half the L2 with every prime in med64 (`-s 3932160 --tune med64=1/1`,
x3): **-4.8% (3/3)** on 1e11 (9e10 window), **+5.5% (3/3)** on the 1e12
tail. The crossover sits between 27K and 78K base primes; the candidate
rule is half L2 on a 256 KiB core while the base primes are few, and the
threshold needs the 2e11-5e11 points.

The points (x3, 1e11 windows, `-s 3932160 --tune med64=1/1` vs the auto):
1e10 **-8.7% (3/3)**, 2e11 -1.4% (3/3), 3e11 +1.2% (3/3), 5e11 +1.9%
(overlap); with 1e11 -4.8% and 1e12 +5.5% the crossover is ~2.5e11, i.e.
~40K base primes. **Rule (tuning.hpp): on a core with an L2 of 256 KiB or
less, the base segment stays at half the L2 while the base primes are at
most 40,000, and the med64 tier takes the whole segment** (the 1/2 gate
became 1/1: a tie with 1/2 at 1e13, -6.7% vs -5.9% at 1e12, and it makes
the half-L2 case simple). Startup lines `segment: half the L2 (...)` and
`med64 cutoff: the whole segment (...)`; test.sh checks both with forced
caches. Expected on the i5-3470: 1e10 1.12x -> ~1.02x, 1e11 1.06x ->
~1.01x, nothing else changes; the modern machines never see either line.

### i5-3470 follow-up: med64 gate at the tails, the +6% that was not code, and the 1e12 window profile (2026-10-04)

The 3ad8ce3 gate (med64 = the whole segment on an L2 of 256 KiB or less)
was only validated at the 1e13 tail. At the sparse-regime tails, where the
segment doubles to 512 KiB (2x the L2), x3 interleaved, auto (whole
segment) / `--tune med64=1/2` / `--tune med64=1/6`: 1e15 15.92-15.97 /
15.93-16.03 / 16.48-16.50 s; 1e16 18.57-18.62 / 18.61-18.65 / 19.09-19.17
s. **Whole segment and 1/2 tie, 1/6 loses 3.3% (3/3 at both). Gate kept.**

The same round's tails came out 4-7% above the table taken at 62fc592
(1e14 12.85 -> 13.39, 1e15 15.06 -> 15.97, 1e16 17.34 -> 18.52, two reps
each, tight), with primesieve unchanged at 1e13-1e15 and the only code
diff being 3ad8ce3. Chased three ways, all in one session
(`~/torre_2026-10-04.log`):

- **The 62fc592 build against d8bc352, x3 interleaved at the 1e15 tail:
  15.95 / 15.97 / 16.10 vs 15.96 / 15.97 / 15.99 s. A tie.** The 15.06 s
  of the table is not reproducible with its own code: the difference is
  machine state, not code.
- **THP is fine:** `thp_fault_alloc` +32 during a 1e15 tail (4 threads x
  2 MiB arenas), `thp_fault_fallback` unchanged at 302, 5.6 GB available,
  the live overlay (`/cow`) at 22%. The "best effort" `madvise` was
  getting its huge pages.
- **`--tune huge=0` x3: 16.13 / 16.13 / 16.13 vs auto 15.96 / 16.24 /
  15.95.** Huge arenas still ~1% ahead, consistent with the above.

primesieve's own 1e17 tail re-run twice: 25.78, 25.58 s against the
table's 21.70 (+18%); 1e18 28.84 vs 25.24 (+14%). The next day's mean-of-5
round (3a1218b, same code) shows what that is: across five interleaved
reps primesieve ranges 21.2-23.0 s at 1e16 and 25.5-27.7 s at 1e17 while
eratostenes stays within 0.1 s (18.54-18.63, 20.37-20.46). On this
machine primesieve's big tails are the unstable side, not ours. Both tools moved
between the two sessions on the same live USB, each on its own rows. See
the power-regime note under Makefile (the server showed the same thing the
same day: 21.37 s cold vs 22.83 s sustained at 1e12).

Count table replaced from the REPS=2 round: 1e10 1.12x -> **1.03x**, 1e11
1.06x -> **1.02x**, 1e12/1e13 1.02x/1.01x unchanged, as predicted for the
half-L2 rule (9,592 base primes at 1e10, the startup line confirms it;
1e12 takes the whole L2).

`perf stat` on the 9e11-1e12 window, 4 threads, the auto plan (an
earlier run with `-t 12` on this 4-core machine was 1.20x: oversubscribed,
the one-per-core rules off; discarded):

| | eratostenes | primesieve |
|---|---:|---:|
| cycles:u | 104.8 G | 105.5 G |
| instructions:u | 127.1 G | 160.3 G |
| IPC | 1.21 | 1.52 |
| branch-misses:u | 0.85 G | 1.04 G |
| L1-dcache-load-misses:u | 12.8 G | 11.4 G |
| LLC-loads:u | **2.03 G** | 0.58 G |
| wall | 7.93 s | 7.85 s |

Cycles tie with 21% fewer instructions and 18% fewer mispredicts; the
whole-L2 segment costs **3.5x primesieve's LLC loads** (its 128 KiB sieve
stays in L2 next to its state), mostly hidden by the out-of-order window,
and that is the whole of the remaining dense-regime gap on this core.
Halving the segment is known to cost more than it saves here (the med64
per-prime-per-segment cost, the half-L2 points above 2.5e11); the lever,
if any, is fewer state streams through L2 during the sieve, not a smaller
segment.

### Sparse ring arenas as 2 MiB huge pages, with one thread per core (kept, 2026-10-04)

Where the one-thread-per-core machines lose most is the 1e16-1e18 tails,
and what grows with N there is the bucket ring's active write set: the
slots ahead scale with isqrt(N) (32 at 1e15, ~1024 at 1e18 on a 1 MiB
segment), each with a 4 KiB tail block being written -- 4 MiB of pages at
1e18 that every re-file store touches at random. Two things were tried on
it. Smaller blocks, to shrink that set (`ERA_BLK_BYTES`, dev PC, 1e18 tail,
1e10 window, 2 threads, x2): 1 KiB +11.7% (4/4 worse), 2 KiB +1.3%
(overlapping) -- the block size stays at 4 KiB. And the arenas the blocks
come from as 2 MiB regions advised `MADV_HUGEPAGE` (one huge page each,
THP in Ubuntu's default "madvise" mode), so the write set costs two TLB
entries instead of a thousand:

| threads | N, window | huge vs 1 MiB arenas |
|---:|---|---:|
| 2 | 1e18, 1e10 (x4) | -12.1% (8/8) |
| 2 | 1e18, 1e11 | -3.5% (overlapping) |
| 2 | 1e17, 1e11 | -2.9% (4/4) |
| 2 | 1e17 / 1e16, 1e10 (x3) | +0.4% / +0.9% (noise) |
| 6 | 1e18, 1e11 | -0.5% (noise) |
| 12 | 1e18, 1e11 (x3) | +10.5% (6/6 worse); a second round -1.0% (overlapping) |
| 12 | 1e15, 1e11 (x4) | -5.3% vs +5.1% in two rounds: noise |

Real at 2 threads from 1e17 up (the 1e10-window figure is mostly the
activation of 50M base primes filing into that write set), nothing at 6,
and against it at 12 in one of two rounds. So it is a runtime decision in
tuning.hpp, on with one thread per core (the same condition as the whole-L2
base and the whole-L1d sub-block), off under HT pairs; `--tune huge=1|0`
forces it and the startup log says which. Checked after the gate: 2
threads 1e17 `huge=0` +3.3% (4/4), 12 threads 1e18 `huge=1` -1.0%
(overlapping). The i7-620M and i5-3470 (512-entry STLBs) and the 2-vCPU
Xeons are the machines it is meant for; unmeasured there yet.

Prefetching the push target as well (`ERA_BIG_PFPUSH`: the tail block of
the slot the entry 16 positions ahead will file into, from its table row
and step computed early): dev PC, 1e11 windows, 2 threads +8.9% at 1e18
and +10.7% at 1e17 (4/4 worse), 12 threads noise. The extra table row per
entry and a prefetch of a line that is written only once cost more than the
push miss; the segment-byte prefetch stays alone. Knob kept, default off.

The three Emerald Rapids operators (b38faee, 2 vCPU one per core, so the
automatic choice is the huge pages; `--tune huge=0` as B, 1e17 tail, 1e11
windows, x3): +6.5% (3/3, the host with the tightest reps: 27.27-27.47 s
against 25.48-26.05 s), +1.2% and +1.3% (both overlapping). Same sign on
all three, 1-6% for turning them off. The i7-620M at 4 threads (HT, the
gate off) measured auto against `huge=0`, i.e. itself: +1.5%, its noise
floor; its real cases (`huge=1` at 4 threads, auto at 2) are pending.
benchmark_ab.sh and benchmark_mini.sh now print the `sparse ring` and
`med64:` startup lines in the config, which the operators missed.

The i5-13500 (20 threads, HT pairs, the gate off) with `--tune huge=1`
forced, x3: +20.4%, every B run above every A run (25.48 / 23.41 / 19.44 s
against 19.07 / 18.91 / 18.79 s), the B runs falling from one rep to the
next -- the cost of 2 MiB pages under 20 threads is partly the kernel
finding and zeroing them (compaction on first touch), and partly the HT
pairs. The gate (one thread per core only) is right on both ends: -1..-6%
where it is on, +10..+20% where it would have been on by default.

The i5-3470 (b38faee): at 4 threads (one per core, so auto already has the
huge pages) `huge=1` against auto is itself, -2.3% / -2.0% overlapping at
1e17 / 1e18 -- that machine's noise floor. At 2 threads, auto (on) against
`--tune huge=0`: +4.1% for turning them off, 6/6, the tightest groups
that machine has produced (37.63-37.81 s against 39.18-39.33 s). The
512-entry STLB reads as expected.

The i7-620M (b38faee, 512-entry STLB, HT): at 4 threads, where the gate
is off, `--tune huge=1` is -0.8% at 1e17 and -1.3% at 1e18 (6/6 both); at
2 threads, where it is on, `huge=0` is +0.9% (6/6). The huge pages help
that core under HT too, by about a point: its TLB is a third of the newer
cores' and the HT conflict that costs 10-20% on the i5-11400F and i5-13500
doesn't dominate there. Not worth a rule (sysfs doesn't say the STLB size,
so it would be one by CPU family); `--tune huge=1` is the way to take it.

### Sparse tier: marking a prime's further hits in the same segment in a loop before re-filing it (tried, reverted, 2026-10-04)

The one structural difference left against EratBig after the pairs, the
prefetch, the 4 KiB blocks and mod-2310: EratBig keeps marking a prime
(`while (sieveIndex < sieveSize)`) and re-files it once per segment, while
`process_big` marks one hit and re-files the entry every time -- into the
current slot's own tail when the next hit is still in this segment, to be
read back later in the same pass (an 8-byte copy, a tail update, a block
now and then). With the cutoff at 1/4 the primes between K/4 and K/2 have
2-4 hits per segment, ~15-20% of the tier's hits at 1e14-1e15, so a loop
(`-DERA_BIG_LOOP=1`, `make bigloop`: `while (pos <= modsb)` after the first
hit, in the unrolled path and the scalar one) looked worth 3-5% on the
one-thread-per-core tails. Dev PC, `BIN_B=./eratostenes_bigloop`, counts
identical:

| threads | N, window | loop vs re-file |
|---:|---|---:|
| 2 | 1e14, 1e10 (x3) | +14.9% (6/6 worse) |
| 2 | 1e15, 1e10 (x3) | +10.5% (6/6 worse) |
| 12 | 1e15, 1e11 (x2) | -0.3% (overlap) |
| 12 | 1e18, 1e11 (x2) | +19.6% (4/4 worse; 23.75 / 19.45 vs 18.08 / 18.05 s) |

Clearly worse where it was meant to help. The re-file is not waste: it is
what keeps every hit in the batched, two-entries-per-iteration stream,
where one entry's table row, segment RMW and push overlap another's; the
loop turns a multi-hit prime back into a dependent chain (table row ->
position -> table row) with a data-dependent exit per entry, inside the
hot path. EratBig's IPC lead is not this. Reverted to the re-file; the flag
stays, like the bands and the medium pairs, as an A/B knob.

### Sparse tier: two entries per iteration in `process_big`, and a segment-byte prefetch 16 entries ahead (both kept, 2026-10-02)

The per-tier map put the uncontended gap in the sparse tier's IPC (2.36 vs
EratBig's 2.84 for the same instructions per hit). EratBig processes a
bucket's primes two at a time; `process_big` went one entry at a time, each
iteration a dependent chain (entry load, table row, segment byte, tail
pointer, store). The mod-2310 loop now takes two entries per iteration: both
entries' loads, table rows and segment RMWs are issued before either push,
and the pushes stay in order (a shared slot's second push reads the tail the
first just wrote). `-DERA_BIG_PAIRS=0` restores the single-entry loop.

Dev PC, cycles:u, ABAB x3 per cell, counts identical:

| | single | pairs | |
|---|---:|---:|---:|
| 1e15 tail, `-t 2` (1/2) | 145.6-147.5G | 142.8-144.4G | -2.2% |
| 1e18 tail, `-t 2` (1/2) | 238.0-239.3G | 234.0-236.8G | -1.4% |
| 1e15 tail, `-t 12` | 326.7-334.3G | 324.0-330.6G | -1% (noise) |
| 1e18 tail, `-t 12` | 738-784G | 746-774G | tie |

Small, but every uncontended run with pairs beat every one without, and
nothing got worse. Generalized afterwards to `ERA_BIG_UNROLL` entries per
iteration (constexpr loops, same instructions as the handwritten pair:
266.60G vs 266.62G); 4 is worse than 2 at `-t 2` (150.7G vs 146.6G median,
register pressure) and a tie at `-t 12`, so 2 stays.

Two follow-ups on the same loop the same day:

- Sparse tier before med64/medium in the segment (its RMWs on a segment the
  med64 state stream hasn't evicted yet): L2 misses -7% at `-t 2`, cycles
  -1.4% / +1% (`-t 2` / `-t 12`) -- overlapped misses again. Not kept.
- Prefetch of the segment byte of the entries `ERA_BIG_PF` positions ahead
  in the bucket (`pos` is the entry's own bits, no table lookup needed, and
  that `s[pos] |= mask` is the load that misses), `-DERA_BIG_PF=0/8/16/32`,
  cycles:u, 4 runs each at 1e15, 2 at 1e18:

  | | 0 | 8 | 16 | 32 |
  |---|---:|---:|---:|---:|
  | 1e15 tail, `-t 12` | 326.7-338.1G | 321-337G (-2.3%) | **317-323G (-2.3..-3.3%)** | 322-335G |
  | 1e18 tail, `-t 12` | 756-773G | | **722-725G (-5%)** | 695-734G |
  | 1e15 tail, `-t 2` (1/2) | 141.7-142.9G | 140.9-144.4G | 142.8-144.6G | |
  | 1e18 tail, `-t 2` (1/2) | 236-248G | | 230-236G | 233-236G |

  Every `-t 12` run at 16 beat every run at 0. The complement of the pairs:
  with one thread per core the segment sits in L2 and there is nothing to
  prefetch (a tie); with two threads sharing an L2 the sparse RMWs miss and
  the prefetch covers them. 16 kept as the default.

### i5-13500 Topdown at `-t 2`: the residual gap is bad speculation; `minsegs` 1; medium bands as an option (2026-10-02)

Native Topdown L1 (image `dev`, `--privileged`, `perf stat -a -C 0,2 -e
'{cpu_core/slots/,cpu_core/topdown-*/}'`), both programs on P-cores 0 and 2,
last 1e11 below 1e15, 2 threads:

| slots | eratostenes | primesieve | delta |
|---|---:|---:|---:|
| total | 696.1G | 658.7G | +37.4G |
| retiring | 322.1G (46.3%) | 334.4G (50.7%) | -12.3G |
| bad speculation | 128.3G (18.4%) | 95.5G (14.5%) | **+32.8G** |
| frontend bound | 68.2G (9.8%) | 63.3G (9.6%) | +5.0G |
| backend bound | 177.4G (25.5%) | 166.8G (25.3%) | +10.6G |

The back-end is even; ~90% of the slot gap is bad speculation (the WSL
estimate for the dev PC had put it at 37%, with L2 stalls as the rest: the
two cores differ). That reopens the predicated medium-tier bands, which cut
branch misses in half on the dev PC but cost more ALU work than they saved
there; Raptor Cove has the wider back-end and the bigger speculation bill.
Re-added behind `-DERA_MED_BANDS=1` (default 0, factor 1.2, plain loop above
8 expected hits): dev PC `-t 2` 1e15 tail, branch-misses 0.866G -> 0.509G,
cycles 143.2G -> 151.2G (+5.6%), as before. To be A/B'd on the i5-13500.

`minsegs` on the i5-13500, 1e10, 20 runs each (`make run`): default 4
median 0.195 s, `minsegs=1` 0.185 s; the runs are bimodal (0.13-0.15 s when
the tail lands well, 0.19-0.20 s when it doesn't) and 1 hits the fast mode
6 times in 20 against 3. Small, nothing lost on the dev PC: default now 1.

### Where the losses are after the sparse-tier changes (2026-10-02, end of day)

Three sandbox sessions on 51523dc (4 KiB blocks, pairs, prefetch), the
i5-13500 with the blocks and pairs, the dev PC with everything, all tails
`REPS=2` (the last 1e11 below N) unless noted:

| machine | 1e13 | 1e14 | 1e15 | 1e16 | 1e17 | 1e18 |
|---|---:|---:|---:|---:|---:|---:|
| i5-11400F, 12 threads | 0.80x | 0.73x | 0.74x | 0.77x | 0.81x | 0.87x |
| i5-13500, 20 threads | 0.97x | 0.94x | 0.96x | 0.99x | 1.02x | 0.96x |
| Xeon Emerald Rapids, 2 vCPU (quiet host) | 0.97x | 1.00x | 1.00x | 1.04x | 0.96x | 0.98x |
| Xeon @ 2.80GHz (32 KiB L1d, 1 MiB L2), 2 vCPU, two hosts | 1.10-1.12x | 1.08-1.09x | 1.04-1.13x | 1.16x | 1.12-1.15x | 1.00-1.04x |

Full counts on Emerald Rapids (three hosts, 53d3073): 1e10-1e12 0.97-1.01x,
1e13 0.93-0.95x. The i5-13500's full 1e15 with the day-before code (steals,
bitmap; no blocks yet): 40,664.78 s against 40,976.93 s.

What moved today: on the 2.80GHz Xeon the 1e15-1e18 tails went from
1.17-1.28x to 1.00-1.16x (blocks + pairs, -5..-7% on the same host); on the
server from 1.01-1.05x to 0.96-1.02x. The prefetch is neutral with one
thread per core (A/B on Emerald Rapids: -2.4..+1.5%, noise), as predicted.

What is left: one machine, the 2.80GHz Xeon, loses 4-16% everywhere,
including the 1e13 and 1e14 tails where the sparse tier barely runs -- so
its dense tiers lose too, which they don't on Emerald Rapids (48 KiB L1d,
2 MiB L2). Its L1d is 32 KiB (sub-block 32 KiB whole, small_limit 4K) and
its L2 1 MiB (512 KiB segment); same per-thread regime as the dev PC at
`-t 2`, which also loses 1.19x at 1e14. Everything that was tried on the
dense tiers today (bands, slices, cutoffs, primesieve's split) failed on
the dev PC; that CPU hasn't been profiled (no PMU in the VM).
Everything else is at parity or better, and the server's 1e10 (1.09x,
tail balance) still waits for the `minsegs` repetitions.

### `.db` output: the `.blk` sidecar, blocks written by the sieve threads (kept, format 3, 2026-10-02)

Follows from the entry below: the single SQLite writer was the `.db` limit on
both machines, in kernel time, not CPU. The compressed blocks now go to a
`.blk` file next to the `.db` (block_file.hpp): a block's place is one
`fetch_add` on a shared offset and the thread that compressed it `pwrite`s
it there -- parallel, lock-free, no gaps, no merge. The `.db` keeps the index
(one row per block: start_index, count, start_prime, offset, len; format 3),
plus the sidecar's name and size, which `nth_prime` checks; it reads a block
with one `pread`. The `blocks`/`block_data` split is gone (no BLOB to keep
away from the start_index UPDATEs). Dev PC, 1e11, 12 threads:

| destination | before (BLOBs in SQLite) | after (.blk) |
|---|---:|---:|
| tmpfs | 6.1-7.2 s | 4.4-4.8 s (-30%) |
| WSL virtual disk | 18.9-26.3 s | 6.1-10.6 s (-60..-70%) |
| user CPU | 50-55 s | 47-50 s |
| size | 2019 MiB | 1990 MiB + 3.3 MiB index |

On tmpfs ~10 of 12 cores are now busy; what remains is the extraction + gap
encoding (36% of the CPU, ~30 cycles per prime, one prime at a time) and zstd
(16%). `make test` (positions against primecount, text vs .db round trip)
passes; a `.blk` truncated by one byte is rejected by name/size. Server
(NVMe RAID0, 1e12: 77 s before, 43 s CPU floor) not yet measured. A single
`.blk` for now: ext4 caps a file at 16 TiB (1e15 is ~15.5 TiB), so sharding
the sidecar (a file number in the index) is the follow-up if that disk is
ext4.

### `.db` output: where the time goes (2026-10-02)

Count vs `.db` at 1e11 on the dev PC (12 threads): 1.93 s / 22.6 s user vs
12.4-26.3 s / 50 s user to the WSL virtual disk -- 4.4 of 12 cores busy; to
tmpfs 6.1-7.2 s / 53-57 s user, 8.8 cores busy. i5-13500 at 1e12 (20
threads, NVMe RAID0): 21.5 s / 414 s user vs 77.4 s / 825 s user + 41 s sys,
11.2 of 20 cores busy, 260 MB/s. So `.db` costs 2.1-2.4x the count's CPU
(extraction + gaps + zstd) and 3-4x its wall: the single SQLite writer is the
limit on both machines, and not for CPU -- perf (user cycles) puts libsqlite3
at 2.45%, libzstd 15.8%, our code 78.5%, the writer thread at 1.4% -- but for
kernel time: with WAL every page is copied twice, 4-5 s of sys on tmpfs for a
2 GB file, and the producers wait on the full queue. Of the user CPU, 36% is
`sieve_chunk<GapBlockSink>` (ctz extraction + `write_k` gap encoding, ~30
cycles per prime), more than any sieve tier. `PRAGMA page_size` 16K/64K
(fewer overflow pages per ~35 KB blob): 64K +78% file size (3602 vs 2019 MB)
and slower, 16K +6% size and no faster; 4K kept. A parallel block file
(producers `pwrite` their compressed blocks at offsets handed out by an atomic
counter, SQLite keeping only the index) would remove the writer, but changes
the format `nth_prime` reads.

### Startup: the dev PC's "7-8 ms overhead" was exec from /mnt/c (2026-10-02)

`./eratostenes 1` (parse and exit) takes 7-9 ms from the repo on /mnt/c (WSL's
9p mount) and 1 ms from ext4; a 1e8 run 10-13 ms vs 5 ms (primesieve 5-8 ms).
Our own startup is ~2 ms: topology detection 0.5-0.8 ms (sysfs, 12 CPUs), 12
threads ~1 ms, presieve tables 0.1 ms, dynamic loader 0.1 ms. The i5-13500
inside Docker: `./eratostenes 1` 1-4 ms, `primesieve 1` 5-25 ms, so its 1e10
loss isn't startup either (see the minsegs entry). Small-N timings on the dev
PC should run a copy of the binary from /home.

### i5-13500 server gap vs primesieve: medium-tier call count, sparse cutoff 1/2 gated on per-thread L2 (2026-09-28)

Context: dev PC (i5-11400F, symmetric) is now below primesieve at every N in
README.md#benchmarks (0.92-0.98x); the server (i5-13500, 6P+HT + 8E, 20
threads) is not, and the gap grows with N: 0.98x (1e12), 1.05x (1e13), 1.14x
(1e14). All measurements below ran on the server through Docker (the only way
to run there), one at a time, with `ERATOSTENES_START` tails of 1e14.

**Method caveat -- 1% tails are wall-clock-unreliable.** `ERATOSTENES_START`
keeps the full-run chunk width, so a 1% tail gets `threads*150*0.01` = ~1.5
chunks per thread: the second, half-empty round leaves cores idle. Measured:
99e12..1e14 on P-cores, eratostenes 459s user / 47.5s wall = 9.7 of 12 cores
busy vs primesieve 11.95 (dev PC: `ERATOSTENES_DEBUG_IDLE` idle=16-17%, 18
chunks). Every wall-clock tail ratio taken that way (1.29x P-only, 1.19x
E-only, 1.20x all) was mostly this artifact. Use 10% tails (`START=90e12`:
~15 chunks/thread, idle 1.6% measured, ratio 1.18x vs the full run's 1.14x)
or compare `cycles:u`/user time. Tried forcing >= 32 chunks/thread in tail
mode (dev PC, ABBA on the 1e14 1% tail): idle 17% -> 0.7%, but user time
+16-23% and wall unchanged (72.3/73.3s -> 70.3/76.2s) -- per-chunk setup is
~0.4s of CPU per chunk, far more than estimated. Reverted. Side lead, not
followed up: that setup is ~1.5% of a full 1e14 server run (3000 chunks).
**Refuted (2026-09-28):** measured directly on the dev PC (temporary
`ERATOSTENES_CHUNKS_PER_THREAD` knob, removed again), same range, `-t 6`
pinned one thread per core, user time, A/B/C/C/B/A:

| range | chunks | user time (2 reps) |
|---|---:|---:|
| last 1% of 1e14 | 9 | 404.2 / 398.3s |
| | 72 | 427.1 / 424.0s |
| | 288 | 428.7 / 424.4s |
| last 0.1% of 1e15 | 6 | 582.8 / 578.1s |
| | 29 | 573.1 / 573.1s |

72 -> 288 chunks costs +0.9s over 216 chunks, ~4ms each (within noise); at
1e15, more chunks is even slightly cheaper. The jump from 9 to 72 chunks is
not setup: with 9 chunks on 6 threads, cores go idle and the remaining ones
turbo higher, so the same work costs fewer CPU-seconds. That is also what
the "+16-23% user time" above really measured. Per-chunk setup (activating
every base prime: a few divisions each) is ~0.01% of a full run. Closed.

**Ruled out (server):**
- Work distribution: already ruled out earlier (idle 2.1%, see above); 1.6%
  on the 10% tail.
- Hybrid cores: 10% tail, 20 threads, `perf stat` split by PMU (counts
  un-scaled by perf's hybrid enable%): cycles 1.14x on `cpu_core` AND 1.14x
  on `cpu_atom`; branch misses 1.96x / 2.08x. Same gap on both core types.
- Shared-resource contention from E-cores (ring/L3 clock): P-cores alone
  (`--cpuset-cpus=0-11 -t 12`, 10% tail) are already +18% cycles:u
  (19.37e12 vs 16.45e12), no better than with the E-cores running. (An
  earlier +4% from the 1% tail pointed the other way; not reproduced,
  unexplained.)
- Segment size (1% tail, wall, single reps, so only relative): P-cores
  256KiB 49.38s / 512KiB 51.14s / 1MiB 70.78s; 20 threads 256KiB 45.01s /
  512KiB 43.35s. Neither closes the gap; the auto 512KiB stays (this closes
  the "not yet validated on the i5-13500" note of the segment-doubling entry
  in arg_parser.hpp below, at least for 1e14).

**Main signal -- L2 misses on P-cores.** P-cores only, 10% tail:

| | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| cycles:u | 19.37e12 | 16.45e12 | 1.18x |
| L1-dcache-load-misses | 1547e9 | 1648e9 | 0.94x |
| LLC-loads (= L2 misses) | 76.9e9 | 5.5e9 | **14x** |
| LLC-load-misses | 0.56e9 | 0.72e9 | 0.78x |

L1 is as good or better, L3 misses fewer -- but 1 in 20 L1 misses gets past
L2 (95.0% L2 hit) vs 1 in 300 for primesieve (99.7%). ~71e9 extra L3 round
trips at ~60-70 cycles is enough to be most of the 2.9e12 extra cycles even
with overlap. Full machine: LLC-loads 11x on P-cores, only 1.9x on E-cores.
Also ~2x branch misses throughout (medium tier's loop exits, known), worth
about half the extra cycles on P-cores in the 20-thread run.

Suspects for what overflows L2 (1.25MB per P-core, shared by 2 HT threads,
each with a 512KiB segment = 1MB in segments alone; primesieve 2x256KiB):
the segment itself, the medium tier's per-prime state (266k primes walked
every segment), the sparse buckets.

**Result: it's the segment, but the L2 misses are NOT the cost.** Same four
counters, P-cores, 10% tail, `-s 7864320` (256KiB) vs the 512KiB auto run
above:

| | 512KiB | 256KiB | change |
|---|---:|---:|---:|
| cycles:u | 19.37e12 | 20.39e12 | **+5.2%** |
| L1-dcache-load-misses | 1547e9 | 1479e9 | -4.4% |
| LLC-loads | 76.9e9 | 12.5e9 | **-84%** (2.3x primesieve, was 14x) |
| LLC-load-misses | 0.56e9 | 0.44e9 | -22% |

L2 hit rate 95.0% -> 99.2%, i.e. the segment (2 HT x 512KiB in a 1.25MB L2)
is what overflowed L2. `perf record -e LLC-loads:u` on the 512KiB 1% tail
agrees: the samples are spread over every tier that writes the whole segment
-- sparse `process_big` 30.6%, medium `cross_off_medium<*>` 39.4%,
`process_med64<*>` 25.4%, `Presieve::fill` 4.5% -- and ~0 in the small tier
(`cross_off<*>`, 0.1%), which works inside an L1 sub-block. No single tier's
own state stands out.

But removing ~64e9 L3 round trips made cycles go *up* 1.0e12. Halving the
segment also reshuffles tiers (med64 29138 -> 15096, medium 266008 -> 139714,
sparse 368632 -> 508968 primes), so that shift costs something, but for the
L2 misses to have been "most of the gap" they'd have had to cost ~45 exposed
cycles each and be outweighed by the tier shift -- implausible. They are
overwhelmingly hidden: segment traffic is scattered stores/RMWs with no
dependency chain, so the OoO core overlaps them (memory-level parallelism).
The earlier "71e9 x 60-70 cycles = most of the 2.9e12" estimate was wrong:
it assumed exposed latency. 14x LLC-loads is a symptom, not the bottleneck;
the auto 512KiB stays (consistent with the wall-clock segment sweep above).

**TopDown level 1 -- the gap is mostly bad speculation.** P-cores, 10% tail,
512KiB auto, slots in e12 (wall 461.3s vs 381.8s = 1.21x):

| | eratostenes | primesieve | excess |
|---|---:|---:|---:|
| slots | 58.33 | 49.51 | +8.82 (1.18x) |
| Retiring | 26.54 (45.4%) | 28.35 (57.3%) | -1.81 |
| Bad Speculation | 11.61 (19.9%) | 5.45 (11.0%) | **+6.16 (2.13x)** |
| Frontend Bound | 7.57 (12.9%) | 6.38 (12.9%) | +1.19 |
| Backend Bound | 12.73 (21.8%) | 9.32 (18.8%) | +3.41 |

We retire *less* work (consistent with -6% instructions), and lose it on
wasted speculation: bad-spec alone is ~70% of the net gap, matching the 2x
branch misses. Backend is +3.4e12 (~39%) -- not nothing, but the 256KiB run
showed the L2 misses aren't it, so if it matters it's core-bound or other
memory. Part of the frontend excess is likely resteers after mispredicts, so
fixing the mispredicts should also claw some of that back.

Recipe (perf 6.1 hybrid): `-M TopdownL1` is broken here (mixes `cpu_atom`
events, EINVAL on `topdown-retiring`); per-thread mode refuses topdown
events. Works: `perf stat -a -C 0-11 -e '{cpu_core/slots/,cpu_core/topdown-retiring/,cpu_core/topdown-bad-spec/,cpu_core/topdown-fe-bound/,cpu_core/topdown-be-bound/}' -- <cmd>`.

**Level 2 (eratostenes, same conditions, 472.9s):** Bad Speculation 19.3% is
19.2% branch mispredict / 0.1% machine clears -- all of it branches. Backend
23.4% = 10.4% memory + 13.0% core. Heavy ops 10.1% of slots. (primesieve's
level 2 not measured yet.)

**Mispredicts per tier** (`perf record -e branch-misses:u`, P-cores, 1% tail,
15.26e9 total -- the same total as the earlier 15.3e9 vs primesieve's 7.6e9).
Per-call rates use 63,578 segments (1e12 / 15,728,640):

| tier | share | misses | per call |
|---|---:|---:|---:|
| medium `cross_off_medium<*>` | 59.7% | 9.11e9 | 0.54 per prime per segment |
| med64 `process_med64<*>` | 20.0% | 3.05e9 | 1.65 per prime per segment |
| small `cross_off<*>` | 16.7% | 2.55e9 | |
| `sieve_chunk` (counting) | 2.3% | 0.35e9 | |
| sparse `process_big` | 1.3% | 0.19e9 | |

The medium tier alone mispredicts more than all of primesieve (9.1e9 vs 7.6e9).
0.54 per call is the known loop exit (see the sparse-cutoff and
segment-doubling entries); med64's 1.65 matches the branchless-tail entry
above, which already showed that fixing med64/small that way costs more in
stores than it saves.

**primesieve, same conditions** (7.61e9 total): `EratSmall` 30.1% (2.29e9),
`EratMedium` 68.0% (5.17e9), `EratBig` 1.1%. Its tiers (`config.hpp`):
small p <= L1d * 0.2 (~9.8k), medium p <= sieve bytes * 3.0 (786k on 256KiB,
i.e. >= ~2.7 hits per segment), everything above goes to EratBig. That's
~62k medium primes x 127k segments = ~7.9e9 calls -> **~0.66 mispredicts per
call, worse than our 0.54.** Our loop isn't the problem; the call count is:
medium + med64 = 295k primes x 63.6k segments = 18.8e9 calls, 2.4x theirs,
because our medium tier runs down to ~1 hit per segment (`p < seg_k_width`,
4.19M on 512KiB) where primesieve stops at ~2.7 (the equivalent cutoff here
is ~0.375 x seg_k_width = 1.57M).

This is the `sparse_limit` question again (three reverts above, all on the
dev PC at 1e12/1e13, where the sparse population grew 2.6x from a small
base). At 1e14 the regime differs: sparse is already 369k primes, and the
raised-cutoff sweep above was still falling at 1x (1.5x was only +1.3%), so
the minimum may sit below 1x there. `ERATOSTENES_SPARSE_NUM/_DEN` (lowering
only, default 1/1) added to re-sweep it; pi(1e11) exact at /16 and /64,
pi(1e12) exact at 3/8, `make test` green.

**Server sweep** (P-cores, 1% tail of 1e14, single runs, identical counts):

| NUM/DEN | cutoff | medium / sparse | cycles:u | branch-misses:u | wall |
|---|---:|---|---:|---:|---:|
| 1/1 | 4.19M | 266008 / 368632 | 1751.4G | 15.19e9 | 48.81s |
| 3/4 | 3.15M | 196610 / 438030 | 1682.4G (-3.9%) | 13.38e9 (-12%) | 47.91s |
| **1/2** | 2.10M | 125672 / 508968 | **1646.0G (-6.0%)** | 11.10e9 (-27%) | 46.94s |
| 3/8 | 1.57M | 89329 / 545311 | 1657.9G (-5.3%) | 9.91e9 (-35%) | 46.89s |
| 1/4 | 1.05M | 52086 / 582554 | 1732.2G (-1.1%) | 8.63e9 (-43%) | 48.27s |

U-shaped, minimum around 1/2 to 3/8 (the two are within single-run noise): below
that the sparse tier's per-hit bucket cost (copy + re-file per hit) overtakes
the medium tier's per-call mispredict. At 1e14 the cutoff is worth ~6% of
cycles -- about a third of the gap to primesieve.

**10% tail confirmation (server, P-cores, ABBA):**

| | 1/1 | 1/2 | 1/2 | 1/1 | delta (means) |
|---|---:|---:|---:|---:|---:|
| cycles:u | 19.291T | 18.846T | 18.853T | 19.310T | **-2.3%** |
| branch-misses:u | 150.5e9 | 110.0e9 | 110.0e9 | 150.6e9 | -27.0% |
| wall | 453.66s | 445.42s | 446.28s | 460.75s | -2.5% |

Reps agree to ~0.1%, so the win is real, but a third of the single-run 1%-tail
figure (-6%) -- trust this one. Gap to primesieve (16.45T): 1.173x -> 1.146x.

**Dev PC, full 1e13, ABBA (wall):** 1/1 364.79s, 1/2 355.55s, 1/2 356.51s,
1/1 365.11s -> **-2.4%**, pi(1e13) exact, controls within 0.1%. 1/2 creates
72036 sparse primes here -- the same count as the old 256KiB-segment regime
where `/4` regressed 3 times; with the doubled segment and today's sparse tier
it wins.

**Briefly kept, then reverted (same day, see the dev PC follow-up below):** default cutoff 1/2 whenever the sparse tier already exists
(`sparse_regime` = isqrt(N) >= the pre-doubling `seg_k_width`, the condition
that doubles the segment); 1/1 below that (N < ~4.4e12 on 256KiB), where 1/2
would create sparse primes in an unmeasured regime. `ERATOSTENES_SPARSE_NUM/
_DEN` still override. Open: re-sweep at 1e15, where sparse dominates and
the optimum may move.

**Mispredicts per tier after the change** (server, P-cores, 1% tail, 11.10e9
total vs 15.26e9 before; primesieve 7.61e9):

| tier | before | after | primesieve |
|---|---:|---:|---:|
| medium | 9.11e9 (0.54/call) | 4.76e9 (0.60/call) | EratMedium 5.17e9 |
| med64 | 3.05e9 (1.65/call) | 3.06e9 | |
| small | 2.55e9 | 2.54e9 | EratSmall 2.29e9 |
| sparse | 0.19e9 | 0.37e9 | EratBig 0.08e9 |
| counting | 0.35e9 | 0.36e9 | ~0 |

Medium now mispredicts less than EratMedium. The remaining ~3.5e9 excess
sits in small + med64 (5.60e9 vs 2.29e9 EratSmall plus whatever share of
EratMedium covers primes below our med64_limit, 349k -- not separable from
the profile). med64 at 1.65 per call is the worst per-call rate left; its
limit (seg_k_width/12) and small_limit were jointly tuned before the segment
doubling and the new cutoff, so a re-sweep via ERATOSTENES_MED64_*/SMALL_* at
1e14 is the cheap next step.

**1e15 sweep** (server, P-cores, last 0.1% of 1e15, single runs, 1.66M sparse
primes at 1/1):

| NUM/DEN | medium / sparse | cycles:u | branch-misses:u | wall |
|---|---|---:|---:|---:|
| 1/1 | 266008 / 1656010 | 2366.5G | 15.31e9 | 54.46s |
| 3/4 | 196610 / 1725408 | 2366.4G (0.0%) | 13.58e9 | 57.62s |
| 1/2 | 125672 / 1796346 | 2372.1G (+0.2%) | 11.29e9 | 57.49s |
| 3/8 | 89329 / 1832689 | 2439.2G (+3.1%) | 10.20e9 | 58.52s |
| 1/4 | 52086 / 1869932 | 2514.4G (+6.2%) | 8.86e9 | 59.90s |

The optimum moved up as expected: at 1e15 lowering buys nothing in cycles:u.
Wall is +5.6% at 1/2 with flat user cycles -- either the tiny tail's chunk
imbalance or kernel time cycles:u can't see (more sparse entries -> more
bucket blocks -> page faults). Pending: 1/1 vs 1/2 ABBA on the 1% tail of
1e15 with user/sys time and page-faults before deciding whether 1/2 needs an
upper N bound.

**med64_limit re-sweep at 1e14** (sparse 1/2 default, 1% tail, single runs):

| MED64 NUM/DEN | med64 / medium | cycles:u | branch-misses:u | wall |
|---|---|---:|---:|---:|
| 1/12 (default) | 29138 / 125672 | 1717.3G | 11.13e9 | 46.55s |
| 1/8 | 42589 / 112221 | 1700.4G (-1.0%) | 11.77e9 | 48.11s |
| 1/16 | 22199 / 132611 | 1690.4G (-1.6%) | 10.66e9 | 47.68s |
| 1/24 | 15096 / 139714 | 1704.7G (-0.7%) | 10.22e9 | 47.60s |
| 0 (off) | 0 / 154810 | 1692.4G (-1.5%) | 10.17e9 | 48.08s |

Flat: the same default config measured 1646.0G in the sparse sweep above, so
run-to-run noise on the 1% tail is ~4% here, larger than every delta in the
table. No change. Note on method: on this HT machine the 1% tail's ~20% idle
also changes how often a thread shares its core with a busy sibling, which
moves cycles:u itself -- cycles:u is NOT immune to the tail artifact on the
server; decide on 10% tails with ABBA (the sparse 1/2 decision was).

**Dev PC follow-up (ABBA, wall, all counts exact):**

| run | 1/1 | 1/2 | 1/2 | 1/1 | delta (means) |
|---|---:|---:|---:|---:|---:|
| full 3e12 (below `sparse_regime`; 1/2 adds 48321 sparse) | 87.42s | 86.37s | 86.63s | 87.62s | -1.2% |
| 1e14, 10% tail | 591.14s | 637.45s | 660.19s | 603.05s | **+8.7%** |

The 1e14 result contradicts the server's -2.3% on the same slice: both 1/2
runs are slower than both controls, so not drift. The dev PC (i5-11400F,
512KiB L2/core, 12MB L3) loses where the server (1.25MB L2 per P-core, 24MB
L3) wins -- the three earlier reverts were also all on this machine. **Reverted**
to 1/1 by default (knob kept): +8.7% would turn the 11400F's 0.98x at 1e14
vs primesieve into ~1.07x. Next: full-machine (20 threads) server ABBA, then
a causal gate (per-thread L2?) validated on more machines.

**Causal check, same machine:** dev PC, 1e14 10% tail, `-t 6` pinned one
thread per physical core (`taskset -c 0,2,4,6,8,10`, same segment), ABBA:
1/1 711.01s, 1/2 710.34s, 1/2 709.48s, 1/1 707.17s -> **+0.1%, a tie** (vs
+8.7% with 12 threads). So the loss comes from two HT threads sharing a core's
caches. Per-thread cache vs result so far:

| config | L2 per thread | L3 per thread | 1/2 vs 1/1 |
|---|---:|---:|---:|
| dev PC, 12 threads | 256KiB | 1MiB | +8.7% |
| dev PC, 6 threads pinned | 512KiB | 2MiB | +0.1% |
| server P-cores, 12 threads | 640KiB | 2MiB | -2.3% |

Monotonic in L2 per thread; L3 per thread can't tell the last two apart. Not
separated from memory bandwidth per thread either (6 threads also halves the
demand). The server's full machine (20 threads: P 640KiB, E 512KiB L2 per
thread, 1.2MiB L3 per thread) is the next discriminating test.

**1e15, 5% tail ABBA (server, P-cores, `START=950e12`, ~47 min per run):**

| | 1/1 | 1/2 | 1/2 | 1/1 | delta (means) |
|---|---:|---:|---:|---:|---:|
| cycles:u | 114.91T | 114.59T | 115.99T | 115.29T | +0.2% |
| cycles:k | 0.194T | 0.208T | 0.206T | 0.193T | (0.2% of user) |
| branch-misses:u | 765.6e9 | 562.7e9 | 563.6e9 | 765.9e9 | -26.4% |
| page-faults | 217579 | 139006 | 139535 | 217360 | -36% |
| user | 32491s | 32216s | 32565s | 32418s | -0.2% |
| wall | 2812.43s | 2784.71s | 2813.46s | 2806.55s | -0.4% |

A tie (the two 1/2 reps differ by 1.2%, more than the delta). The 0.1%-tail
"+5.6% wall" was the tail artifact, not kernel time: sys is <0.5s and 1/2
even page-faults less. At 1e15 the ~200e9 saved mispredicts are paid back in
full by the extra sparse-tier work. So on the server 1/2 is -2.3% at 1e14
and neutral at 1e15.

**Server full machine (20 threads, no cpuset), 1e14 10% tail, ABBA:**

| | 1/1 | 1/2 | 1/2 | 1/1 | delta (means) |
|---|---:|---:|---:|---:|---:|
| wall | 416.33s | 409.09s | 409.22s | 416.71s | **-1.8%** |
| user | 7734.7s | 7684.8s | 7677.5s | 7871.9s | -1.6% |
| cpu_core cycles:u | 24.25T | 23.98T | 24.13T | 24.72T | -1.7% |
| cpu_atom cycles:u | 19.90T | 19.92T | 19.35T | 19.88T | -1.3% |
| branch-misses:u (core + atom) | 271.7e9 | 198.5e9 | 197.5e9 | 272.0e9 | -27% |

(cycles are multiplexed, ~62%/38% enabled -- scaled estimates.) Reps within
0.1% on wall. At 1.2MiB L3 per thread (close to the dev PC's 1MiB at 12
threads, which lost 8.7%) this still wins, so L3 per thread is not the
driver. L2 per thread fits all four points: 256KiB loses, 512KiB (dev PC 6
threads; server E-cores) ties, 640KiB (server P-cores) wins. Candidate gate:
smallest detected L2 share per CPU >= 512KiB (server yes, 11400F no).

**Where the time goes at 1e15** (server, P-cores, 0.1% tail, cycles:u, current
`main` image with 1/2): `process_big` 39.1%, med64 22.9%, medium 18.7%, small
and the rest ~19%. The sparse tier becomes the main cost past 1e14. `perf
annotate` of `process_big` (skid puts each cost on the instruction after the
one that stalls):

| where | share of `process_big` |
|---|---:|
| `or %dil,(%r11,%rax,1)` -- the `s[pos] \|= mask` RMW into the segment | ~49% |
| load of `tail_[slot]` (+ its test) | ~9% |
| store of the entry into the target block (`vmovq`) | ~8% |
| next-block prefetch | ~7% |
| entry load, table lookup, step math | the rest (~27%) |

Half of the sparse tier is waiting on the scattered byte write into the
512KiB segment -- two HT threads' segments (1MiB) plus the bucket streams
share a 1.25MB L2. At 1e14 a 256KiB segment cost +5% cycles (medium's per-segment
fixed cost doubles); at 1e15 the balance may flip. Next: 256KiB vs 512KiB at
1e15.

**Server, full 1e13, full machine, ABBA (wall):** 1/1 307.20s, 1/2 306.33s,
1/2 306.24s, 1/1 305.64s -> -0.04%, a tie (controls differ by 0.5%). The
~5% gap to primesieve at 1e13 is not in the medium/sparse boundary.

**Adopted, gated:** 1/2 when `sparse_regime` AND the smallest detected
per-CPU L2 share is >= 512KiB (i5-13500: E-core 512KiB, P-core 640KiB ->
on; i5-11400F: 256KiB -> off, unchanged). Every measured server point is
neutral or better (-1.8% at 1e14 full machine); nothing changes on the dev
PC. The 512KiB threshold sits on the measured tie point, not between two
measured wins -- a 384KiB-per-thread machine is unmeasured and gets 1/1.

(Original note, superseded:) the dev PC at 1e13 -- with the doubled segment, 1e13 has no
sparse primes by default (sqrt = 3.16M < 4.19M) and 1/2 would create ~71k,
exactly the regime where the /4 attempts regressed.

Server perf recipe: `eratostenes:dev` image ships `linux-perf`; run with
`docker run --cap-add SYS_ADMIN` (host `perf_event_paranoid=3`, a
Debian/Ubuntu-only level that `PERFMON` does not pass). perf 6.1 syntax:
unprefixed events (`cycles:u,...`), it splits them into `cpu_core`/`cpu_atom`
itself; `cpu_core/x:u/` is a syntax error.

### `small_limit` cutoff tuning

Measured on an i5-11400F (48KiB L1d), cycles:u at N=1e12 vs the old table/onfly
tiers: 2414G -> 1730G (32KiB), 1693G (48KiB), 1798G (64KiB), 1935G (128KiB), 1931G
(512KiB, i.e. no sub-blocking); cutoff at /4 and /1 both lost to /2 at every size --
a lower cutoff leaves too many hits on the slower medium loop, a higher one pays
the unrolled loop's unpredictable entry/exit on primes with too few hits to
amortize it.

**Re-checked (2026-09-24)** after the medium tier's mod-210 stepping made medium
~14% cheaper per hit: hypothesis was that a cheaper medium tier should pull
`small_limit` down (fewer primes classified small, more ceded to the now-cheaper
medium). Measured (i5-11400F, perf stat cycles:u, N=1e12, single run at a time):
/2 (current, 1.4753T) vs /3 (1.4842T, +0.6%, instructions:u +4.3%) vs *2/3 i.e.
K=1.5 (1.4920T, +1.1%, despite instructions:u -2.2%) -- both directions lost. The
per-hit gap between small (~2 instructions) and medium (~8-9, even after the
mod-210 cut) is still ~4x, far bigger than medium's 14% improvement, so the optimal
cutoff didn't move. /2 confirmed still optimal.

### Small cutoff 1/2 of the sub-block on a 256 KiB L2 core (kept, 2026-10-06)

`make benchmark-mini` on the i5-3470 (1e10 window, single runs, its own
warning about noise) had one row outside the pack: `--tune small=1/2`
0.97 s against auto 1.02-1.03 s. The reason it could be real: on that core
the small tier ends at ~4K (526 primes: 1/4 of the 16 KiB half-L1d
sub-block, and the cutoff stays there when `finish_threads` gives the
sub-block the whole L1d) while primesieve's EratSmall runs L1-blocked to
~23K, so the 4K-23K band goes through the med64 kernel, which on Ivy
Bridge misses L1 on every byte (the 2026-10-04 entries). `make
benchmark-ab`, x3 interleaved against auto: **1e13 tail (1e11 window)
-2.8%, every B below every A**; `small=1/1` -1.1% with overlap, so 1/2 is
the optimum; **1e12 tail -2.9%, 1e11 tail -1.6%, 1e15 tail -0.6%, all
every-B-below-every-A; 1e14 tail 0.0%** (the sparse tier dominates there
and the small cutoff doesn't touch it). Rule: `small_den = 2` on a core
whose L2 is 256 KiB or less (the same proxy as the half-L2 and
whole-segment-med64 rules), `--tune small` overrides, startup line `small
cutoff: 1/2 of the sub-block (...)`, test.sh checks it with forced caches.
The modern cores keep 1/4: it was re-tuned jointly with med64 on the
i5-11400F (2026-09-26) and the i5-13500's own benchmark-mini (1e11 window)
had `small=1/2` inside its +-3-5% noise. The same mini run on the i5-13500
found nothing else outside the noise, and the i5-3470's other knobs
(sparse 1/4, med64 1/12 or 1/4, NTA) all lost or tied: the static rules
are at the optimum these two machines can measure, so a per-machine tuning
profile has nothing to collect yet.

The same `small=1/2` on the rest of the fleet, x3 interleaved, 1e11 (9e10
window) / 1e12 (1e11 window): **Emerald Rapids sandbox -3.6% / -2.6%,
every B below every A**; Xeon @ 2.80GHz +0.9% / 0.0% (overlap); i5-13500
(20 threads, HT, 24 KiB sub-block) +1.9 / +1.6 / +0.3 / -4.2% at
1e11..1e14 (all overlap); and the dev PC's own table above (2026-10-01,
`-t 1`, 48 KiB sub-block) has the 12K cutoff **+3.4% / +2%** worse than 6K.
Same sub-block, same cutoff, opposite signs on Golden Cove and Rocket Lake:
the small cutoff's optimum is a property of the microarchitecture, not of
any cache size, so there is no rule to write (the one above, for L2 <= 256
KiB, stays). Left as the manual knob: `--tune small=1/2` on an Emerald
Rapids-class VM for runs up to ~1e12. A per-machine tuning profile
(`make tune` writing a config file) was considered and declined: the
project's configuration stays derivable from caches, threads and CPU
flags, like primesieve's, with `--tune` for long runs on a known machine.

### Cache-topology sizing: per-CPU-minimum step (kept)

On a hybrid P-core/E-core CPU, `detect_l2_cache_bytes()`/`detect_l1d_cache_bytes()`
always read cpu0 -- if cpu0 happens to be a (bigger-cache) P-core, every thread,
including E-core ones, gets sized for cache they don't actually have that much of.
Fix: detect every CPU's own fair L2 share (`CpuCacheTopology`) and, if any of them
is SMALLER than what cpu0 alone gave us, use that smallest one instead -- for every
thread, uniformly, not per-thread.

An earlier per-thread version (`sched_getcpu()` + a per-CPU table) measured SLOWER
on the actual target hardware (i5-13500, 2026-09: ~28-30s vs ~26-27s at N=1e12)
even though it was the mathematically "fairer" per-thread value. The uniform,
smallest-wins version tracks a run where the buggy first version of the per-thread
code (an unrelated arithmetic bug, since fixed, that ended up dividing every
thread's share by an EXTRA 2 on top of the fair-share division) measured fastest of
all (~25-26s): not because the bug's exact numbers were special, but because a
smaller, safely-under-budget segment (skipped once for every thread, not
per-thread-recomputed) seems to matter more on real many-thread-contended hardware
than hitting each core's own "fair" cache share exactly. See git history for the
full A/B trail (dev PC and server) behind this. Skipped when the user already
forced a value on purpose (`-s`, `--l2-bytes`, `--l1-bytes`) or detection found
nothing (non-Linux, sysfs unavailable).

The recompute itself is gated on GENUINE heterogeneity (some CPU's share smaller
than cpu0's own), not run unconditionally: on a uniform machine every share is
equal, so the minimum trivially equals cpu0's, and re-deriving through
`seg_k_width_from_l2_bytes` -- whose own /2 margin is deliberately extra-
conservative, validated for real P/E-core contention (see that function's own
entry below) -- would apply that same extra margin machine-wide for no reason,
on hardware where it's only ever been measured to help (i5-13500) and was NOT
re-validated to help (this project's own i5-11400F data on this margin question
is mixed -- see git history). Only touch anything when the machine actually has
more than one cache domain.

**L1d switched from minimum to maximum (kept, 2026-09-27).** The minimum rule was
tuned for the segment (L2) and had been applied to the small tier's L1 sub-block
too. On the i5-13500 that sized every thread's sub-block for the E-cores' 32KiB
L1d (16KiB sub-block, small_limit 4096) instead of the P-cores' 48KiB. Server A/B
at N=1e12, Docker, 20 threads, 3 interleaved reps with 30s gaps:

| rep | default (L1 min, 16KiB) | `--l1-bytes 49152` (24KiB) | + `--l2-bytes 1310720` |
|---|---:|---:|---:|
| 1 | 24.10s | 23.59s | 24.58s |
| 2 | 24.02s | 23.65s | 24.45s |
| 3 | 24.45s | 23.90s | 24.64s |
| mean | 24.19s | **23.71s (-2.0%)** | 24.56s (+1.5%) |

The P-core L1d wins every rep; also forcing the P-core L2 (segment 7.86M -> 19.7M)
loses every rep, so the minimum stays right for the segment. Now: segment from the
smallest L2 share, sub-block from the LARGEST L1d, both still uniform for every
thread (threads migrate between core types at runtime, so per-thread sizing by the
core a thread starts on isn't reliable). No-op on uniform machines (dev PC).

### Medium/sparse cutoff raised above `seg_k_width` (tried, reverted, 2026-09-27)

The cutoff compares a prime's *value* with `seg_k_width`, a width in *wheel
indices*; a segment spans `seg_k_width * 30/8` numbers, so sparse primes between
2.1M and 7.86M (on a 256KiB segment) still hit every segment 1-3 times, which
looked like bucket work the (now cheaper, byte-position) medium tier could do
better. Every earlier attempt on this cutoff *lowered* it; this raised it, via an
`ERATOSTENES_SPARSE_NUM/_DEN` multiplier. Measured on the last 1% of 1e14
(`ERATOSTENES_START=9.9e13`, cycles:u, identical counts across all variants,
pi(1e11)/pi(1e12) exact with the raised cutoff):

| cutoff | medium / sparse primes | cycles:u |
|---|---|---:|
| 1x (current) | 139714 / 508968 | 2.910T (control rerun 2.890T) |
| 1.5x | 210652 / 438030 | 2.949T (+1.3%) |
| 2x | 280050 / 368632 | 3.083T (+5.9%) |
| 3x | 415605 / 233077 | 3.535T (+21%) |
| 4x | 548266 / 100416 | 4.060T (+39%) |

Monotonically worse. The sparse tier handles 1-3 hits per segment fine; a medium
prime pays a fixed cost every segment (8-byte state read and write, plus ~0.5 loop
exit mispredicts per call at 1e14), which is what dominates medium there. With
the three lowering attempts, the cutoff has now been measured in both directions:
`p < seg_k_width` stays.

### EratBig-style sparse tier: forcing a power-of-2 segment width, and `sparse_limit = seg_k_width/4` (all attempts reverted)

The sparse tier's EratBig-style rewrite (see `segment_sieve.hpp` above) needs the
segment width in BYTES to be a power of 2 for its bucket-slot math to be a
shift/mask instead of a division. `base_limit >= seg_k_width` is a conservative
check for "will any base prime actually end up sparse" -- when false, no prime is
classified sparse and the width is left exactly as auto-tuned.

**First try, reverted**: idea 1 from an external review's second round
(2026-09-24) -- decouple the medium/sparse cutoff from the segment width itself
(`sparse_limit = seg_k_width/4` instead of always `p >= seg_k_width`), on the theory
that primesieve's own EratMedium/EratBig split (`FACTOR_ERATMEDIUM=3.0`) keeps a
wider flat tier and pushes only the very sparsest hits (<1/segment on average) to
the bucket ring, and that this project's medium tier's worst offenders -- per the
reviewer's rdtsc-per-tier breakdown at N=1e14, 64% of cycles -- are exactly those
closest to `seg_k_width`, paying a full `DenseState` touch most segments for zero
hits.

Measured (i5-11400F, perf stat cycles:u, single run at a time, natural auto -s):
N=1e12 -- 1.4674T -> 1.4523T cycles:u (-1.0%, medianos 75773->40665, dispersos
0->35108); N=1e13 -- 19.583T -> 20.325T cycles:u (+3.8%, a real regression,
medianos 152886->40665, dispersos 72036->184257 (2.6x)), cache-misses:u 15.26B ->
29.35B (+92%). The sparse tier's own bucket-ring sizing (`BLK_BYTES`,
`SPARSE_BLOCK_ENTRIES=128`) was tuned for the EXISTING population, not one 2.6x
bigger. A gain at 1e12 that reverses at 1e13 is directionally the same failure
mode as the reverted 64-list medium attempt, just in a different tier -- reverted
for the same reason: real at the N tested, but the wrong direction for this
project's actual E14+ target.

**Retry (2026-09-25)**: re-ran the exact same `sparse_limit = seg_k_width/4` cutoff
WITHOUT re-tuning the ring first (wanted to re-confirm the baseline number on this
machine before spending time on `BLK_BYTES`) -- as expected, reproduced the same
population split (medianos 152886->40665, dispersos 72036->184257) and the
same-shaped regression, slightly worse this time: cycles:u 19.139T->20.713T
(+8.2%), cache-misses:u 19.16B->31.09B (+62%). Reverted again.

**Taken (2026-09-25, follow-up session)**: retuned `BLK_BYTES` 1024->4096
(128->512 entries/block, matching the ~2.6x population growth) and re-ran the same
`sparse_limit = seg_k_width/4` cutoff. Correctness held (pi(1e12) exact vs
primecount) and it's a real, reproducible win at N=1e12 (wall-clock, no perf access
this session): ~30.8-31.0s vs a ~31.3-31.5s baseline, consistently 2 reps each. But
at the *natural* N=1e13 cliff -- the actual N this change targets -- it reproduced
as a regression across 3 separate runs against 2 clean baseline runs,
non-overlapping ranges: experiment 437.60s/489.73s, baseline 415.33s/430.17s --
i.e. retuning the block size fixed the catastrophic +8.2%/+34% cycles:u blowup
from the two attempts above, but didn't close the gap into a win; a real, still-
negative effect remained. Reverted a third time (`BLK_BYTES` back to 1024, cutoff
back to plain `seg_k_width`).

Root cause, not further isolated (would need perf's cache-miss counters,
unavailable that session): the 2.6x bigger sparse population's total memory
footprint (population x `sizeof(DenseState)` = population x 8 bytes) doesn't shrink
just because each block holds more entries -- `BLK_BYTES` only changes how that
footprint is grouped/traversed, not its size, so it was never going to fully offset
a genuinely bigger working set living in the bucket ring. Three strikes now
(unretuned/128 entries, unretuned/128 entries again, retuned/512 entries), all
regressing at 1e13 specifically. Don't re-propose `sparse_limit` independent of
`seg_k_width` without a fundamentally different fix for the population's memory
footprint itself, not just how it's grouped into blocks.

### `--start`: the primes in the rounded-down head of the first word were counted (bug, fixed 2026-10-04)

`split_ranges` rounds a `--start` down to a multiple of 64 wheel indices
(the dense tiers need word-aligned segments), and nothing removed the
primes between that boundary and the start from the count: up to 63
indices (~240 numbers) of head. Every benchmark tail starts at a multiple
of 10^k (`999999990000000000`, ...) whose index happens to be a multiple
of 64, and the test suite's unaligned start (`9999000001`) rounds down by
exactly one index to a composite, so it never showed; it did at
`--start 19999900000000000` (10 mod 30, 44 indices of head): 3 extra
primes (...923, ...929, ...971) against primesieve and primecount. Fix:
`SieveConfig::skip_below_k` (the index of the first number >= start, 1
without `--start`) and `SegmentSieve::set_skip_below_k`; `sieve_and_emit`
marks the indices below it composite before extraction, which also covers
the old "index 0 is the number 1" line. Test: that 2e16 tail on both wheel
paths against primecount.

### `--start`: the start itself was dropped when its wheel index was 63 mod 64 (bug, fixed 2026-10-05)

The other end of the same rounding. `split_ranges` took the first chunk's
start as `wheel_count_upto(start) / 64 * 64`, but `wheel_count_upto(start)`
is the index of the first number *above* start: when start is on the wheel
that is one past start's own index, and when that index is 63 mod 64 the
rounding lands one word *after* start instead of on its word. `skip_below_k`
(above) was already `wheel_count_upto(start - 1)`, so the two disagreed by one
index exactly there, and a prime start was lost. Found by a sweep of
`--start` values against `primesieve START N -c` on the laptop (2026-10-05,
code review): 239, 2399 and 4799 (indices 63, 639, 1279) each counted one
prime short of primesieve at N = 1e6; 241, 251, 1979, 2401, 4801 agreed. A
corollary: `240 --start 239` produced an empty `ranges` and `main` indexed
`ranges.back()` -- a segfault. One start in 64 that is itself prime, so
neither the benchmark tails (powers of ten) nor the suite's starts had hit
it. Fix: `start - 1` in `split_ranges` and in `tuning.hpp`'s `first_k`
(the narrow-segment skip used the same expression), plus an explicit
empty-`ranges` exit in `main`. Tests: `--start 239` and `2399` at 1e6,
`240 --start 239` (1) and `241 --start 239` (2).

### `ByteCounter`: digit count without `to_chars` (tried, tie, reverted, 2026-10-05)

The text output's counting pass (`count_worker`, `ByteCounter`) ran a full
`std::to_chars` per prime only to read the digit count off the returned
pointer. Replaced with a cached decade: `[lo, hi)` and its digit count from
the previous prime, two compares per call (the primes of a chunk come in
increasing order, so both predict), a recount from scratch when the value
falls outside. Byte-identical output at 1e8. Laptop (12T, WSL2, wall of
the `count:` line, old/new interleaved, 1e9): 12 threads 0.12-0.16 s both;
1 thread, 8 pairs, old min/median 0.55/0.56 s, new 0.56/0.57 s (max 0.61).
A tie or a hair worse; the conversion was never the cost. What is: the
count pass at one thread takes 0.56 s where the count-only run of the same
N takes 0.20 s (primesieve: 0.12 s), so `emit_values` (the ctz walk and the
wheel index -> value rebuild per prime) plus the sink is ~7 ns per prime,
two thirds of the pass; the write pass (0.9 s) adds `to_chars` and `pwrite`
on top, ~14 ns per prime over the sieve. `perf record -e cpu-clock` (WSL2
has no hardware counters) can't split it further: with `-flto` the whole
pass is one inlined symbol, `sieve_chunk<ByteCounter>` (40% of the text
run) and the write pass's worker lambda (44%), the `cross_off<PR>` kernels
that are separate symbols in a count-only profile don't appear at all. A
real look at `emit_values` needs it pinned `noinline` first, as the tier
kernels are. Reverted to `to_chars`.

## arg_parser.hpp

### `--zstd-level` default: 1 (kept, 2026-09-28)

The default (3) was picked on general zstd knowledge -- entropy coding, which is
most of the achievable ratio on this near-random gap-byte stream, barely depends
on compression level, so higher levels mostly buy slower builds, not smaller
files. That reasoning was later checked directly: `--zstd-level 1` vs. the
default 3, interleaved x3 on the dev PC, measured ~5-8% faster (16.7-19.6s vs.
17.8-20.9s) with essentially identical output size (0.629 vs. 0.630
bytes/prime), but wasn't made the default at the time.

With wheel-index gaps (see
[gap_encoding.hpp](#gap-encoding-wheel-index-deltas)) level 1 is both faster
and *smaller* than 3: 1e11 2.108 vs 2.182 GB, 13.3s vs 24.1s; 1e12 20.23 vs
20.94 GB, 218.1s vs 273.8s (dev PC, 2 reps each, table in that entry). Level 3's
LZ match search finds nothing to match in a near-iid byte stream and its
block-splitting costs a little ratio. Made the default.

### Sub-block size: half the L1d, not all of it (kept, 2026-09-27)

The small tier's sub-block was the full detected L1d (48KiB on the dev PC),
chosen before med64 existed (see the `small_limit` cutoff-tuning entry below:
32/48/64KiB measured, 48KiB won then). Re-checked after spotting that
`--l1-bytes 32768` ran faster than the native 48KiB. Decomposed at N=1e12 (dev
PC, cycles:u, 2 interleaved reps each), since `--l1-bytes` moves both the
sub-block and `small_limit` (= sub-block/4):

| sub-block | `small_limit` | cycles:u | vs 48KiB native |
|---:|---:|---:|---:|
| 48KiB | 12KiB | 1.3680T | -- |
| 24KiB | 12KiB | 1.3148T | -3.9% |
| 48KiB | 6KiB | 1.3861T | +1.3% |
| 24KiB | 6KiB | 1.2859T | **-6.0%** |
| 16KiB | 4KiB | 1.2988T | -5.1% |

The sub-block size is the driver; `small_limit` shrinking with it adds ~2% more,
but shrinking `small_limit` alone at the old sub-block hurts -- the same kind of
coupling the small/med64 joint sweep found. Likely mechanism: a sub-block that
fills all of L1d leaves no room for the small tier's own per-prime state and the
presieve window, which then evict the sub-block every pass. Checked across N
(3 reps at 1e10/1e11, 2 at 1e13, interleaved):

| N | 48KiB | 24KiB | delta |
|---|---:|---:|---:|
| 1e10 | 9.651G | 8.210G | **-14.9%** |
| 1e11 | 116.00G | 102.40G | **-11.7%** |
| 1e12 | 1.3680T | 1.2859T | **-6.0%** |
| 1e13 | 18.120T | 17.583T | **-3.0%** |

No sign flip; the gain is largest where the small tier is the biggest share of
the work, which is small N -- this was most of the small-N overhead against
primesieve. Now `sub_block_from_l1_bytes()` = L1d/2, used by both the global
and the hybrid-topology path; `--l1-bytes` still means "the L1d size". Not
re-swept: `small_limit`'s own divisor and med64's fraction were tuned at the old
48KiB sub-block -- a joint re-sweep at 24KiB may find a slightly different
optimum. Not yet measured on the server (L1d 48KiB P / 32KiB E -> 16KiB
sub-block, which measured -5.1% here).

### Sub-block: the whole L1d when each thread has a core to itself (kept, 2026-10-01)

The half-L1d entry above was measured with every hardware thread busy, where two
SMT siblings share each L1d: half of it IS each thread's whole share. With one
thread per core that rule leaves half the L1d unused. Found from the 2-vCPU Xeon
KVM sandbox (Emerald Rapids, 48KiB L1d, `Thread(s) per core: 1`), which lost
1.16x/1.08x to primesieve at 1e10/1e11 with `-t 1`, while the dev PC wins at
`-t 12`. Dev PC thread sweep (era/primesieve wall, mean of 3): 1e11 -t 1 1.11x,
-t 6 1.02x, -t 12 0.84x. perf at 1e11: primesieve's L1d misses grow 2.17x from
-t 1 to -t 12 (eratostenes 1.31x) -- primesieve sizes its L1 chunk for a whole
core (`Erat.cpp`: EratSmall chunk = full L1d, never halved for SMT), which wins
alone on a core and loses under SMT; ours did the opposite.

Dev PC, `-t 1` (a single thread owns its core), cycles:u, 2-3 reps (<1% spread),
via `--l1-bytes` (sub-block = half of it) and `--tune small=a/b`:

| sub-block | `small_limit` | 1e10 | 1e11 |
|---:|---:|---:|---:|
| 24KiB (default) | 6KiB | -- | -- |
| 32KiB | 8KiB | -5% | -4% |
| 40KiB | 10KiB | -6% | -5% |
| 48KiB | 12KiB | -6% | -5% |
| 56KiB / 64KiB | 14 / 16KiB | +3% / +8% | +3% / +7% |
| 48KiB | 6KiB (`small=1/8`) | **-9.4%** | **-7.0%** |

`small=1/6..1/12` at 48KiB are all within ~1% of each other; letting
`small_limit` grow with the sub-block is what loses. Tails at `-t 1`: 1e12
(last 2%) -3.2%, 1e13 (last 0.2%) -4.5%, counts identical. Wall-clock vs
primesieve at `-t 1`: 1e10 1.25x -> 1.09x, 1e11 1.105x -> 1.03-1.04x (cycles
46.5G vs primesieve's 46.4G). Same flags on the Xeon (`-t 1` and `-t 2`, 3 reps
each, 36 runs, counts OK): -4.9..-5.6% in all four N/thread combinations, 1e11
-t 1 ties primesieve (15.02s vs 14.98s). The same 48KiB sub-block at `-t 12` on
the dev PC is **+16.5%** cycles, so it can't be the default where SMT siblings
share the L1d.

First version (6846baf): whole L1d only when every CPU's L1d has exactly one
logical CPU (sysfs `shared_cpu_list`: no SMT anywhere) and all L1d sizes are
equal. On the Xeon, real build, `-t 2`, interleaved vs 078f780 and primesieve
12.0 (counts OK): 1e10 1.16x -> 1.06x, 1e11 1.09x -> 1.04x, 1e12 1.04x ->
**0.97x**, 1e13 last 1% 0.98x -> **0.93x** (new vs old -4..-8%).

That rule looks at the hardware, not at how many threads run. The i5-13500
server (native Linux, P-core siblings (0,1)..(10,11)), 1e11 `-t 6`, cpu_core
cycles:u, 3 reps (<1% spread), whole L1d forced via the flags above:

| placement | 24KiB | 48KiB | delta |
|---|---:|---:|---:|
| `taskset 0,2,4,6,8,10` (6 P-cores, 1 thread each) | 51.54G | 48.39G | **-6.1%** |
| `taskset 0-5` (3 P-cores, 2 SMT threads each) | 94.56G | 119.54G | **+26.4%** |
| unpinned `-t 6` | 51.55G | 48.47G | **-6.0%** |
| unpinned `-t 4` | 51.30G | 48.04G | **-6.4%** |

Unpinned runs match the one-per-core placement exactly, with <0.5% of the
threads' time on E-cores: the scheduler spreads threads one per core, P-cores
first. Generalized rule kept: the sub-block is the whole L1d when `-t` <= the
number of physical cores with the largest L1d (each CPU sharing an L1d
instance counts 1/sharers of a core); otherwise half. It subsumes the first
version (no SMT, uniform L1d: cores = CPUs) and keeps E-cores out (hybrid
with HT off: only the P-cores count). Defaults are unchanged on SMT machines
at full thread count (dev PC 12 > 6, server 20 > 6); `-t` <= 6 on either now
gets 48KiB. `small_limit` keeps the half-L1d value; `--l1-bytes` still means
"the L1d size" and skips this rule. The startup log prints
`sub-block: whole L1d (T threads <= C cores with the largest L1d)` when it
fires. (`-t 6` on the dev PC under WSL2 was inconclusive: Hyper-V doesn't
honour sibling pairs, so in-guest `taskset` can't place one thread per core.)

Also checked from primesieve's sizing: its small-N segment (sqrt(N)*2 bytes, a
multiple of the L1 chunk: 192KiB at 1e10) is -2..-3% vs our 256KiB at 1e10
`-t 1`, near noise and only for N <= ~1e10 (from ~2e10 up both reach the
L2-derived cap) -- not pursued.

### Auto segment width: dropping the `isqrt(limit)` cap (kept)

An earlier version additionally capped the auto segment width at `isqrt(limit)` --
the smallest width that keeps EVERY base prime dense, avoiding the sparse tier
altogether below the N where that width exceeds the L2 budget. That's still
correct, but it stopped being the right default once the small tier's L1 sub-block
was decoupled from segment size: before that change, a smaller segment directly
meant less L2 traffic for every tier, so "smaller is better below the cap" made
sense; the small tier is now already confined to L1 regardless of segment size, so
shrinking the segment below the L2 budget no longer helps it and only adds fixed
per-segment cost (walking every active prime's state, entering/exiting each tier's
loop, presieve fill) more often than necessary, for the medium and sparse tiers
that DO still scale with segment count.

Measured (perf stat cycles:u, i5-11400F): dropping the isqrt cap and always using
the L2 budget is 12.4% faster at N=1e11, 6.0% faster at N=1e12 -- both regimes
where `isqrt(limit)` used to be the smaller (binding) value (isqrt gave
~41KiB/~130KiB arrays there, versus the 256KiB the L2 budget allows). Past the N
where `isqrt(limit)` alone would already exceed the L2 budget (roughly 1e13+ on
this machine), this change is a no-op: the L2 budget was already the smaller,
binding value even with the old `min()`, so dropping isqrt from the comparison
doesn't change the result there.

That's a DIFFERENT question from how big the budget itself should be in that
regime -- this project already measured removing the /2 halving (using the full L2
instead of L2/2) as a regression at N=1e13 (see this file's git history) -- so the
/2 stays.

### Segment width doubled once the sparse tier exists (kept, 2026-09-27)

At 1e14 the medium tier was the costliest one (39% of cycles) and bound by a fixed
cost per prime per segment (state load/store, ~0.5 loop-exit mispredicts), not by
its hits. A segment twice as wide pays that half as often. Measured with `-s
15728640` (512KiB) against the auto 256KiB, 2 interleaved reps per point, counts
identical everywhere (`ERATOSTENES_START` slices, see `main.cpp`):

| point | 256KiB | 512KiB | delta (wall) |
|---|---:|---:|---:|
| last 1% of 1e14 | 82.93 / 82.36s | 68.42 / 69.95s | -16% |
| middle of 1e14 ([4.95e13, 5e13]) | 36.09 / 36.22s | 33.55 / 31.80s | -10% |
| last 5% of 1e13 | 22.70 / 23.00s | 21.38 / 21.11s | -7% |
| full 1e12 | 26.73 / 26.62s | 28.39 / 28.40s | **+6.5%** |
| full 1e13 (ABBA) | 376.08 / 378.05s | 365.44 / 369.74s | -2.5% (cycles:u -0.7%) |

It wins where sparse primes are active and loses where they aren't (1e12: sqrt(N) <
seg_k_width, the wider segment only adds cache pressure with two hyperthreads per
L2). Rule, `main.cpp`: when `isqrt(N) >= seg_k_width` (some base prime would be
sparse) and the width is automatic, double it -- i.e. the whole per-thread L2
share instead of half. 1e11/1e12 keep 256KiB; 5e12 and up get 512KiB. Clean 1e13
run afterwards: 348.67s (README best was 361.19s; primesieve 354.854s -> 0.98x).
`make test` green. Also tried in the same session: `-t 6` (no hyperthreading) on
the 1e14 tail, 104.4s vs 82.6s, +26% -- HT still pays at that scale.

Not yet validated on the i5-13500, where the per-thread share is the E-core one
(512KiB) and there are 20 threads. (2026-09-28: 1e14 tail on the server, 20
threads, 512KiB beat 256KiB, 43.35s vs 45.01s -- see main.cpp's "i5-13500
server gap" entry.) A finer version (wide segment only for chunks
past seg_k_width^2, where sparse primes actually start) could also recover the
loss in the first ~44% of a 1e13 run -- done, see the next entry.

### Narrow segment for the chunks below narrow² (kept, 2026-09-28)

The finer version of the entry above. When the width has been doubled, a chunk
whose every number is below narrow² (narrow = the pre-doubling width, i.e. half the
final power-of-2 one) has no active prime >= narrow, since activation is by p². It
runs exactly the non-sparse configuration instead: narrow segment, 1/1 cutoff,
med64_limit on the narrow width, via a second `TierSet` picked per chunk in
`main.cpp` (its sparse list only holds primes that never activate there). Chunks
straddling narrow² stay wide. `ERATOSTENES_NARROW_EARLY=0` disables it.

Correctness beyond `make test` (which never reaches the sparse regime): forced
with a small `--l2-bytes` so it kicks in at small N -- 1e11 (1236/1800 chunks
narrow), 1e10 (773/1800), 3e9 `-t 3` (161/450) exact against primecount, and a
1e10 `.db` built that way matched `primecount --nth-prime` at 52 positions.

Dev PC, full 1e13, same binary, ABBA (791 of 1800 chunks narrow, counts exact):

| | off | on | on | off | delta (means) |
|---|---:|---:|---:|---:|---:|
| wall | 372.20s | 368.17s | 371.33s | 380.55s | **-1.8%** |
| user | 4450s | 4401s | 4441s | 4551s | -1.8% |

Both "on" runs beat both controls, wall and user agree; the ceiling was ~2.9%
(1e12's 6.5% over 44% of the range). The share of narrow chunks is
narrow²/N: ~44% at 1e13, 4.4% at 1e14, so it matters for 5e12..~3e13 and
fades above. Not yet measured on the server.

### `seg_k_width_from_l2_bytes`'s extra /2 margin, applied on top of an already-per-thread L2 share (kept, counterintuitive)

This function's own /2 margin is applied in `main.cpp`'s per-CPU-minimum step even
against an already-per-thread L2 share (not just the raw machine-wide value the
single-CPU fallback passes) -- applying this same /2 on top of a share that's
already divided by how many logical CPUs share that L2 looked like double-counting
the same headroom at first (and briefly was fixed away as a bug) -- but measured,
on the actual target hardware (i5-13500, many real threads contending for shared
L3/memory bandwidth), the smaller resulting segment is reliably faster than the
"fair share, no extra margin" version, not slower. See git history for the full
A/B trail behind reversing that "fix".

## presieve.hpp

### Period-sized tables: fill in 4 KiB chunks with wraparound (kept, 2026-09-29)

Each table used to be stored unrolled `max_seg_k_width` bits past its period, so a
segment's window was one contiguous read. That made each table as big as the
segment: 16 × ~512 KiB ≈ **8.3 MB** on the i5-13500 (512 KiB segment in the sparse
regime), 4 MB on the dev PC -- not the ~123 KB the header comment claimed. Every
segment, every thread streamed a segment's worth of every table (8 MB on the
server) through L2, evicting the segment and the med64/medium state right before
those tiers ran. This fits the server's LLC-loads at 14x primesieve's, spread over
med64/medium/sparse rather than presieve itself (see the i5-13500 gap entry in
main.cpp), and the gap opening at 1e13, where the segment doubles.

Now `fill()` covers `dst` in `PRESIEVE_CHUNK_BYTES` = 4 KiB chunks: each table's
byte offset advances one chunk and wraps by its period in between, so a table only
needs its period plus one chunk (~190 KB for all 16) -- primesieve keeps its
pre-sieve buffers period-sized the same way. The inner loops are unchanged (4
tables per pass, unaligned 8-byte loads, vectorized). Startup also stops
allocating and marking 4-8 MB of tables.

Correct: `make test`, pi(N) vs primecount up to 1.2e11, and every A/B row below.
Only `Presieve::fill` changed size in `nm`. Dev PC (i5-11400F), `perf stat
cycles:u`, 3 interleaved reps against the previous commit's binary:

| | base | compact tables | delta |
|---|---:|---:|---:|
| 1e13, 10% tail (`START=9e12`) | 1858.1G | 1833.3G | -1.3% |
| 1e12 full | 1220.7G | 1211.0G | -0.8% |

Full runs after the change (together with the `process_big` entry below, which
doesn't run on this machine at these N -- 0 sparse primes): 1e11 2.09s -> 2.05s,
1e12 25.46s -> 25.05s, 1e13 343.79s -> 336.33s (0.94x primesieve). Server A/B
pending (1e14 10% tail).

### `fill()`: skip the `self_k` correction loop when it can't possibly match (kept, 2026-09-26)

`Presieve::fill()` ran a small loop (one entry per pre-sieve prime, ~30-35
total) over `self_k` on *every single call* -- once per sub-block, so many
times per segment -- to patch the one absolute position where each table's
periodic pattern is wrong (a prime marked composite by its own multiple-of-1
hit). But every `self_k` value is a wheel-index of a prime <= 163, so it can
only ever fall inside a segment at or near the very start of the whole
range -- past that point the loop runs every single time with no chance of
ever matching, pure wasted comparisons. Fix: track `max_self_k` (computed
once in `build_presieve`) and skip the loop entirely whenever
`k_low > max_self_k`, which is true for the overwhelming majority of
segments in any real run.

Found while auditing the existing code for redundancy (user's own framing:
architecture-independent, not chasing another hardware-specific micro-opt
after the med64 mod-210 dead end above). Correctness held (`make test`
green, pi(1e12) exact). Measured (dev PC, i5-11400F, `perf stat
cycles:u,instructions:u`, N=1e12, 4 interleaved reps each side, cooldown
between runs):

| metric | baseline (4 reps avg) | with skip (4 reps avg) | delta |
|---|---:|---:|---:|
| cycles:u | 1.345253T | 1.344509T | -0.055% (within the ~±0.3% run-to-run spread) |
| instructions:u | 1.306988T (identical all 4 reps) | 1.306824T (identical all 4 reps) | -0.0125%, real and deterministic |

`instructions:u` being bit-identical across every repeat on each side (not
just close) confirms the removed work is real, not noise -- but at ~0.0125%
of total instructions, it's an order of magnitude below what this dev PC's
cycles:u noise floor (~0.3%) can distinguish from zero. Unlike the
`SegmentSieve`-per-worker entry above (also inconclusive on cycles:u), this
one was kept anyway: the fix is provably correct by construction (the
skipped loop cannot ever do anything once `k_low > max_self_k`), adds no
new abstraction or API surface (one struct field, one guard), and can never
measure worse than the baseline it replaces -- there's no complexity or risk
being traded for the unproven cycles:u win, unlike the worker-reuse change's
API/indirection cost. **Kept.**
**Why:** distinguishes two shapes of "measured as inconclusive on cycles:u":
a change whose entire justification IS the performance claim (worker reuse
avoiding allocator overhead -- reverted when that claim didn't hold), versus
a change that's independently correct/harmless and only incidentally also a
(too-small-to-see) performance win. The bar this project holds performance
claims to doesn't need to block a free, provably-safe removal of dead work.
**How to apply:** don't expect this fix alone to move any wall-clock number
visibly -- it won't, at any N, since it's a fixed tiny fraction of an
already-small (~5.6% of cycles, per `perf-cache-scaling-validated` project
memory Finding 4) part of the pipeline. It's correctness/cleanliness kept
cheap, not a lead worth re-measuring later.

### Extending pre-sieve coverage past prime 163 (tried three ways, all reverted)

`Presieve::fill()` does one shift-and-OR pass per *group* over the whole segment
every single segment, a cost that's fixed per group regardless of that group's
table size or which primes are in it -- so the real cost of adding N more groups is
proportional to N, not to how well-sized their tables are.

- **Attempt 1**: paired 167+173 and 179+181 with *each other* (wrong -- badly
  sized) into 2 new groups. Cycles +1.6-1.8%, IPC 1.57->1.52, wall-clock flat, at
  N=1e12.
- **Attempt 2**: paired each of 167/173/179/181 with a small partner reused from an
  existing group instead (41*167=6847, 43*173=7439, 47*179=8413, 53*181=9593 --
  correctly sized, matching the ~7-10KB the rest of the grouping targets) into 4
  new groups. Worse, not better: cycles +4.3%, IPC 1.57->1.49, wall-clock +3.7%.
  Properly-sized tables didn't help because table size was never the driver of
  `fill()`'s per-group cost -- group *count* was, and this version added twice as
  many groups as attempt 1.
- **Attempt 3**: the "one big group" idea, `{167, 173, 179, 181}` as a single 17th
  group (period_k ~935MB table -- fill()'s cost was shown to track group *count*,
  not table size, so this was meant to cost the same as any other single group
  despite the huge table). Tried on an i5-11400F: +1.4% wall-clock at N=1e12
  (51.73s vs 51.03s, single rep, no sparse tier present at that N either way, so
  this isolates the presieve change alone). Still a regression -- group *count*
  wasn't the whole story after all (likely the table's own memory footprint/TLB
  pressure, streamed fresh every segment since 935MB doesn't fit any cache level,
  costs more than the 2-4 wheel hits/segment it would have saved). Reverted
  without spending the ~15min needed to also check N=1e13.

Both attempts 1-2 reverted; a real win here would need fewer new groups (e.g. one
group covering all four new primes at once, at the cost of a much bigger table --
tried as attempt 3, also reverted) or restructuring `fill()` so a group's cost
scales with how often its primes actually hit rather than a fixed full-segment
pass -- out of scope for what's been tried so far.

## Makefile


### Compiler flags and PGO, re-asked with the A/B tooling (nothing, 2026-10-04 night)

`make benchmark-flags` with compile flags as the variants (DEFS is appended
to CXXFLAGS, so `-O2` overrides `-O3`), dev PC, 12 threads, 1e11 windows,
x2: 1e13 `-mprefer-vector-width=512` -0.3%, `-funroll-loops` +0.6%, `-O2`
+2.0%, `-fno-plt` -0.6%, `-march=x86-64-v3` -1.5%; 1e15 +0.7% / -1.9% /
+0.3%; all overlapping. At 2 threads with 1e10 windows the same flags read
+11% to -5%, also overlapping: that window is too short at 2 threads for
anything under 10%. PGO (`make pgo`, the Makefile's 1e9-1e11 training
set, the binary copied aside and the default rebuilt): 12 threads 1e13 tail
+2.4% (overlapping), 1e15 -1.9% (4/4), 2 threads 1e13 noise, full 1e12
23.27 / 23.34 s against 23.13 / 23.13 s. Nothing to take; `-O3
-march=native -flto=auto` stays, PGO stays unadopted.
### PGO training set: a natural 1e13 pass (tried, reverted, 2026-09-25, follow-up session)

Considered adding a natural `1e13` training pass (no `-s` override) to also give
the sparse tier real-proportion data instead of just the forced-tiny-width passes
already in the training set. Doesn't scale: an instrumented (unoptimized,
atomic-counter) build running a full 1e13 count-only pass took 45+ minutes on the
dev PC with no sign of finishing, vs seconds for the forced-small-width runs --
the instrumented binary is far slower than release, and at 1e13 that cost becomes
prohibitive per `make pgo` invocation. Killed before completion; not worth the
build-time cost for one training pass among several.

### Two power regimes on every machine: burst and sustained (open, 2026-10-04)

The i5-13500 server gave 21.37 s at 1e12 in one round and 22.83 s (best of
5, 22.83-22.89) in the next, primesieve unchanged at 25.3 s. The user
confirmed the 21.37 was real and taken cold: the first run after the
machine had been idle. The i5-1235U laptop shows it as a staircase: the
1e13 tail x3 back to back went 7.20 / 7.97 / 10.19 s (primesieve 7.63 /
10.71 / 10.54), with the chip never getting hot. The i5-3470 moved 4-7%
on the tails between sessions with its own build tied against itself (see
the i5-3470 follow-up under main.cpp).

This is the turbo budget, not temperature: PL2 for the tau window (~28 s
by Intel default), then PL1. A run shorter than tau that starts from idle
runs entirely in the burst regime; anything after ~30 s of load, or a run
of minutes, is in the sustained one. The two regimes differ by ~6% on the
server and up to 40% on the 15 W laptop. The earlier conclusion that
neither desktop throttles (clocks >= 4.2 GHz, < 70 C) was about
temperature and stands; the budget drop is a separate mechanism and the
clocks do move with it.

Consequences for the tables:

- `benchmark.sh` runs primesieve first and the REPS of eratostenes after:
  at 1e10-1e12 primesieve gets the burst and we get the sustained regime.
  `benchmark_tails.sh` interleaves the pairs, so its ratios share a regime
  and are the more stable ones.
- Rows measured in different regimes are not comparable (the server 1e12
  21.37 vs 22.83, the tower's 1e14-1e16 tails, the laptop's whole table).
- Decision (user, 2026-10-04): the benchmarks ignore the power windows.
  No warm-up, no RAPL in the tables. Both scripts run interleaved pairs
  alternating the order, both programs REPS times, and print each one's
  mean: with reps the means converge on the sustained speed by
  themselves, which is the one a count of hours sees. The power mechanism
  stays here as the reading rule for cross-session differences, not as a
  benchmark parameter.

Measured on the i5-1235U (`/sys/class/powercap/intel-rapl/intel-rapl:0`):
PL1 15 W with a 32 s window, PL2 55 W (2.4 ms window), peak 70 W. Cold,
1e10 x5 interleaved: 0.31-0.32 s vs primesieve 0.332-0.336 (0.95x). After
a 70 s warm-up (a 1e12 count), everything in PL1: 1e10 0.41 vs 0.452
(0.91x); tails x2 interleaved 1e13 10.09 / 10.643, 1e14 12.22 / 12.510,
1e15 14.11 / 14.555, 1e16 16.17 / 16.763, 1e17 18.61 / 19.170 s
(**0.95-0.98x across the board**). The earlier table's 0.75-0.88x came
from a primesieve at 0.439 s for 1e10 that no regime reproduces today
(0.333 cold, 0.452 sustained). The laptop's section in BENCHMARK.md is
now the sustained round and says so in its description line.

The i5-13500 server, three manual `make run` from idle (each a fresh
container, minutes apart): 1e10 0.14, **1e11 1.49 s (the REPS=5 round:
1.88-1.89)**, 1e12 21.62 (round: 22.83-22.89). A 21% burst at 1e11 and
5% at 1e12 means a short PL1 window on that board (seconds, not 32):
RAPL readout and a 1e11 x10 series with a power/MHz trace pending there.
Note the scripts' own older note (1e10 ~0.19 s cold vs ~0.13 warm on the
same server): that is the frequency ramp from idle, a second effect with
the opposite sign at the 0.1 s scale.

### PGO overall: measured on the dev PC, not adopted on the production server

Measured on the dev PC (perf stat cycles:u, count-only, N=1e10..1e13, before the
two forced-sparse training runs were added): ~2-4% fewer cycles, consistent in
direction across the whole range -- see README.md#benchmarks.

**VERDICT (2026-09-25, production server, via `docker-pgo`/`run-pgo`, real
hardware not the dev PC): NOT ADOPTED.** N=1e12: several PGO runs clustered
~24.5-25s vs the non-PGO README figure of 24.51s -- no visible separation from
run-to-run noise. N=1e13: 328.49s (PGO) vs 330.38s (non-PGO, README) -- a 0.57%
difference, again indistinguishable from single-run wall-clock noise. Unlike the
dev PC, the server has no perf/cycles:u available to look past that noise the way
this project normally would -- so on the one machine this was actually built for,
PGO's payoff can't even be confirmed, let alone justified against its real costs: a
longer build, and an image/binary tied to the exact machine it's compiled on. Kept
as opt-in infrastructure (not part of default `make`/`make docker`) since it's
harmless sitting unused, but don't re-run this validation again without a new
reason to expect a different answer -- this was checked on real production
hardware, not extrapolated from the dev PC.
