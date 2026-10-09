# Research log: tried, measured, kept or reverted

Every optimization this project tried, with its verdict and the numbers that
decided it: the record behind the source comments, which say what the code
does and link here for why. [ALGORITHM.md](ALGORITHM.md) explains the design,
[BENCHMARK.md](BENCHMARK.md) has the current times against primesieve.

This is the condensed log (2026-10-07): each entry keeps what was tried,
where, the decisive numbers and the verdict. The full log, with every table,
profile and dead end (~5300 lines), is in git history:
`git show 752050c:docs/RESEARCH.md`.

**How things were measured.** Unless an entry says otherwise: the dev PC
(i5-11400F, 6C/12T) with `perf stat -e cycles:u`, cycles rather than wall
time, which drifts several percent between runs on a shared or thermally
limited machine. The other machines: the server (i5-13500, 6P+HT and 8E, 20
threads), the tower (i5-3470, Ivy Bridge, 4 cores without HT, 256 KiB L2),
the laptop (i5-1235U under WSL2: callgrind instruction counts and wall time
only) and three 2-vCPU claude.ai VMs (two Emerald Rapids, one Cascade Lake
Xeon @ 2.80GHz). A/B runs are interleaved, A B A B ... (scripts/benchmark_ab.sh).

**Names.** Entries keep the knob names they were measured with. The
`ERATOSTENES_*` environment variables became flags or `--tune` keys on
2026-10-01 (`--start`, `--debug-idle`, `--tune small|med64|sparse=a/b`). The
cleanup of 2026-10-07 removed the compile-time experiments that lost
everywhere (`ERA_MED_BANDS`, `ERA_MED_PAIRS`, `ERA_BIG_LOOP`,
`ERA_BIG_PFPUSH`, `ERA_FPDIV`, `ERA_ACT_BATCH`, `ERA_BLK_COLOR`,
`--tune med64s`), the old paths kept as A/B controls (`ERA_BIG_PFSPREAD`,
`ERA_BIG_FASTBLK`, `ERA_ACT_KCUT`, `ERA_BIG_UNROLL`, `--tune big2310`,
`--tune minsegs`) and PGO, each step checked with callgrind function by
function; `scripts/perf_ab.sh` and `make benchmark-flags` were folded into
`scripts/benchmark_ab.sh`. The knobs left are `ERA_BIG_PF`, `ERA_ACT_IDX` and `ERA_BLK_BYTES`
(src/sparse_tier.hpp). Since then the sparse tier lives in sparse_tier.hpp
(it was in segment_sieve.hpp) and the base segment rule in `plan_sieve` (it
was in `parse_args`); the entries keep the old file names.

## Milestones

What is in the code today, by the day it went in:

- **2026-09-24**: the sparse tier rewritten EratBig-style, a bucket ring of
  pooled blocks ([entry](#sparse-tier-eratbig-style-rewrite-adopted-2026-09-24-isolated-test-of-point-1-from-an-external-review-opus-55));
  the medium tier on the [mod-210 multiplier wheel](#cross_off_medium-mod-210-multiplier-stepping-2026-09-24)
  with a [class-specialized layout](#cross_off_medium-class-specialized-layout-kept-2026-09-24).
- **2026-09-26**: the [med64 band](#medium-tier-64-list-restructuring-scoped-to-a-bounded-sub-band-med64_primes-kept-2026-09-26),
  its cutoff [tuned jointly with the small one](#small_limit-re-tuned-jointly-with-med64_limit-kept-2026-09-26).
- **2026-09-27**: the [segment doubled once the sparse tier exists](#segment-width-doubled-once-the-sparse-tier-exists-kept-2026-09-27);
  the [sub-block at half the L1d](#sub-block-size-half-the-l1d-not-all-of-it-kept-2026-09-27);
  the medium tier on [byte positions](#cross_off_medium-byte-positions--doubled-tables-kept-2026-09-27);
  the sparse tier's [next-block prefetch](#sparse-tier-prefetch-the-next-block-of-the-chain-once-per-block-kept-2026-09-27).
- **2026-09-28**: a [narrow segment for the early chunks](#narrow-segment-for-the-chunks-below-narrow-squared-kept-2026-09-28);
  the [server gap](#i5-13500-server-gap-vs-primesieve-medium-tier-call-count-sparse-cutoff-12-gated-on-per-thread-l2-2026-09-28)
  traced to the medium tier's call count, hence a sparse cutoff of 1/2 from
  512 KiB of L2 per thread;
  [zstd level 1](#--zstd-level-default-1-kept-2026-09-28).
- **2026-09-29**: the [pre-sieve from period-sized tables](#period-sized-tables-fill-in-4-kib-chunks-with-wraparound-kept-2026-09-29);
  the medium state as [struct of arrays with a gated prefetchnta](#cross_off_medium-struct-of-arrays-state--gated-prefetchnta-kept-2026-09-29);
  med64's [checked loop](#med64-eratmedium-style-checked-loop-cross_off_checked-kept-2026-09-29)
  and [prefetchnta](#med64-prefetchnta-on-the-state-stream-kept-2026-09-29);
  `process_big` [from ~12 loads per hit to 5](#sparse-tier-process_big-loads-per-hit-12---5-kept-2026-09-29).
- **2026-09-30**: med64 on the [mod-210 wheel](#med64-mod-210-stepping-on-the-checked-loop-cross_off_checked210-kept-2026-09-30);
  the medium `qp` as [1-byte deltas](#cross_off_medium-qp-as-1-byte-deltas-kept-2026-09-30);
  the sparse tier on the [mod-2310 wheel](#sparse-tier-mod-2310-multiplier-wheel-kept-2026-09-30).
- **2026-10-01**: [contiguous runs, the sieve carried across chunks, and steals](#run_parallel_chunks-contiguous-runs-the-sieve-carried-across-chunks-steals-kept-2026-10-01);
  the [whole L1d as sub-block with one thread per core](#sub-block-the-whole-l1d-when-each-thread-has-a-core-to-itself-kept-2026-10-01);
  [.db extraction in wheel indices](#db-extraction-in-wheel-indices-gapblocksinkwrite_k-kept-2026-10-01).
- **2026-10-02**: the [.blk sidecar](#db-output-the-blk-sidecar-blocks-written-by-the-sieve-threads-kept-format-3-2026-10-02)
  (format 3); the [segment ceiling](#segment-ceiling-half-the-l2-per-thread-within-16-32-x-l1d-kept-2026-10-02);
  [4 KiB ring blocks](#per-tier-cycles-against-primesieve-at--t-2-and-the-sparse-tiers-block-size-4-kib-kept-2026-10-02);
  `process_big` [two entries per step and a segment-byte prefetch](#sparse-tier-two-entries-per-iteration-in-process_big-and-a-segment-byte-prefetch-16-entries-ahead-both-kept-2026-10-02);
  [steals priced from the run's own measurements](#run_parallel_chunks-steals-priced-with-the-runs-own-measurements-kept-2026-10-02).
- **2026-10-03**: the [whole-L2 base segment with one thread per core](#whole-l2-base-segment-one-thread-per-core-no-sparse-tier-kept-2026-10-03);
  the sparse cutoff at 1/4 [from 1 MiB of L2 per thread](#sparse-cutoff-14-from-1-mib-of-l2-per-thread-kept-2026-10-03)
  and [by the L3 per active thread](#one-thread-per-core-the-medium-tiers-per-call-cost-and-the-sparse-cutoff-by-active-threads-2026-10-03-evening);
  the [32 x L1d cap](#base-segment-capped-at-32-x-l1d-a-vm-whose-sysfs-reports-the-hosts-l3-as-l2-kept-2026-10-03).
- **2026-10-04**: the next-block prefetch [spread over the block](#sparse-tier-next-block-prefetch-spread-over-the-current-block-kept-2026-10-04);
  [huge-page ring arenas](#sparse-ring-arenas-as-2-mib-huge-pages-with-one-thread-per-core-kept-2026-10-04);
  med64 at [1/6 of the segment](#one-thread-per-core-the-medium-tiers-per-call-cost-and-the-sparse-cutoff-by-active-threads-2026-10-03-evening),
  and [all of it on a 256 KiB L2](#i5-3470-profile-at-1e12-the-med64-tier-over-the-whole-l2-segment-is-59-of-the-cycles-open-2026-10-04);
  a [--start counting bug](#--start-the-primes-in-the-rounded-down-head-of-the-first-word-were-counted-bug-fixed-2026-10-04) fixed.
- **2026-10-05**: [sparse activation from the bitmap index](#sparse-activation-from-the-bitmap-index-18-fewer-instructions-per-prime-kept-2026-10-05),
  gated on BMI2; a [second --start bug](#--start-the-start-itself-was-dropped-when-its-wheel-index-was-63-mod-64-bug-fixed-2026-10-05) fixed.
- **2026-10-06**: `process_big` [in groups of 4 entries](#sparse-tier-process_big-in-groups-of-4-entries-no-per-iteration-edge-tests-kept-2026-10-06);
  the [small cutoff at 1/2 on a 256 KiB L2](#small-cutoff-12-of-the-sub-block-on-a-256-kib-l2-core-kept-2026-10-06);
  [half the whole-L2 width with few base primes](#half-the-whole-l2-width-with-few-base-primes-on-every-one-per-core-machine-kept-2026-10-06).
- **2026-10-07**: the cleanup above; the [JCC-erratum flag](#cascade-lake-branches-kept-off-32-byte-boundaries-jcc-erratum--wa-mbranches-within-32b-boundaries-measured-not-adopted-2026-10-07)
  measured on Cascade Lake and not adopted (no CPU-model rules);
  [ranges with -o and nth_prime queries](#db-format-4-ranges-and-nth_prime-as-a-query-tool-kept-2026-10-07) (.db format 4);
  narrow windows: [no filing past N](#activation-sparse-primes-with-no-multiple-up-to-n-arent-filed-kept-2026-10-07)
  and the [base primes on the wheel bitmap](#base-primes-sieved-into-the-wheel-bitmap-with-the-main-sieves-kernels-kept-2026-10-07);
  [tails to the 64-bit ceiling checked against primecount](#tails-up-to-the-64-bit-ceiling-at-6-threads-checked-against-primecount-2026-10-07).
- **2026-10-08**: error messages name the largest N; [threads vs tail height](#threads-vs-tail-height-dram-bandwidth-caps-the-sparse-tier-near-264-a-memory-budget-kept-2026-10-08)
  measured (DRAM bandwidth past ~1e18) and a memory budget, `--max-mem`.
- **2026-10-09**: the medium [prefetchnta gate re-measured](#medium-prefetchnta-gate-re-measured-with-the-5-byte-state-kept-2026-10-09)
  with the 5-byte state: unchanged; the `.db` encoder [over the whole segment, its state in locals](#db-encoder-over-the-whole-segment-its-state-in-locals-kept-2026-10-09);
  the text output's [counting pass from the prime count](#text-output-byte-counts-from-the-prime-count-where-a-chunks-numbers-share-a-digit-count-kept-2026-10-09).

## Contents

- [Small and med64 tiers](#small-and-med64-tiers)
- [Medium tier](#medium-tier)
- [Sparse tier](#sparse-tier)
- [Activation and the medium/sparse cutoff](#activation-and-the-mediumsparse-cutoff)
- [Segment and sub-block sizing](#segment-and-sub-block-sizing)
- [Threads, scheduling and whole-run profiles](#threads-scheduling-and-whole-run-profiles)
- [Wheel and stepping tables](#wheel-and-stepping-tables)
- [Pre-sieve](#pre-sieve)
- [Output: gap encoding and the .db format](#output-gap-encoding-and-the-db-format)
- [Compiler, build and PGO](#compiler-build-and-pgo)
- [Open threads](#open-threads)

## Small and med64 tiers

The dense tiers. The small tier (`cross_off<PR>`: mod-30, an unrolled 8-hit cycle) runs per L1 sub-block
on the primes below `small_limit` = 1/4 of the sub-block (1/2 when the L2 is 256 KiB or less). med64
(`cross_off_checked210<PR>`: EratMedium's checked loop on the mod-210 wheel, 384 (class, phase) lists of
double-buffered 8-byte entries read with `prefetchnta`) runs over the whole segment on `[small_limit,
med64_limit)`, `med64_limit` = 1/6 of the segment (all of it when the L2 is 256 KiB or less).

### Small tier's unrolled loop applied to medium-hit-count primes (measured, not adopted)

The reasoning behind the small/medium split itself. `cross_off`'s unrolled 8-hit cycle costs
~2 instructions/hit against ~9-12 for the medium tier's generic stepping, but only once its
unpredictable entry (the jump into the switch) and exit are amortized over enough hits: on
medium-range primes at N=1e11 it took ~5.6x the branch misses, a net regression despite 38%
fewer instructions. Hence `small_limit` (next entry).

### `small_limit` cutoff tuning

i5-11400F (48 KiB L1d), cycles:u at 1e12 against the old table/onfly tiers: 2414G -> 1693G with
48 KiB sub-blocks (1730G at 32 KiB, 1798G at 64 KiB, 1931G without sub-blocking). /4 and /1
both lost to /2 at every size: lower leaves too many hits on the slower medium loop, higher pays
the unrolled entry/exit on primes with too few hits. Re-checked 2026-09-24, after mod-210 made
the medium tier ~14% cheaper per hit: /3 +0.6%, *2/3 +1.1%; the ~4x per-hit gap (~2 vs ~8-9
instructions) still dominates. Superseded by the joint re-tune with `med64_limit` (2026-09-26).

### Medium tier: 64-list restructuring, retry with a block-pool allocator (2026-09-24, idea 2 from an external review, Opus 5.5, second round)

The 64-list idea (primes marked with `cross_off<PR>`, re-filed every segment into 64 (class,
entry phase) lists) on the whole medium tier, retried on the `Blk` block pool instead of a
`std::vector` per list to avoid the vector version's footprint growth. 1e12: instructions:u
-26.7%, cycles:u -0.25% (IPC 1.33 -> 0.97); 1e13: cycles:u +8.3%, cache-misses:u +102%. At
1e10, with cache misses negligible in both, instructions -23.8% and cycles +0.03%: the cost is
`cross_off<PR>`'s unpredictable entry/exit switch on few-hit primes, not memory. Reverted; the
idea won once scoped to a bounded band (`med64_primes`, below). Side note: the flat tiers'
8-byte `erat::DenseState`, walked in place every segment, replaced a 40-byte/prime `delta[]`
table tier plus the `ONFLY_CORRECTION` loop: -26% cycles:u at 1e11, -30% at 1e12 (dev PC).

### `cross_off`: narrowing locals to uint32_t (tried, reverted, 2026-09-25)

Every local in `cross_off` fits well under 2^20, so `uint32_t` looked like a free encoding win.
perf stat, 2 reps: cycles:u +1.47% at 1e11, +0.95% / +1.09% at 1e12, and instructions:u went up
(+3.2% / +2.5%, deterministic). Not isolated; mixed 32/64-bit pointer-offset arithmetic
apparently blocks something GCC does with 64-bit locals. Reverted to `uint64_t`.

### Medium tier: 64-list restructuring scoped to a bounded sub-band (`med64_primes`, KEPT, 2026-09-26)

The birth of med64: the same 64-list idea (byte marking via `cross_off<PR>`, 64 (class, entry
phase) lists, double-buffered) scoped to `[small_limit, med64_limit)`, `med64_limit =
seg_k_width * MED64_NUM / MED64_DEN`, run by `SegmentSieve::process_med64<PR>` between the
small and the flat medium tier. The whole-tier attempts regressed at 1e13 because their cache
misses (+95.8% from 1e12 to 1e13) tracked the medium population (+101.8%), which grows until
sqrt(N) passes `seg_k_width`; a band bounded well below that saturates early (dev PC:
`[24576, 786432)` holds 60,221 primes at both 1e12 and 1e13). Dev PC (WSL2), cycles:u against
no med64, 2 reps: 1/8 **-5.5% / -5.6%** at 1e12 and **-4.07% / -4.17%** at 1e13, ahead of 3/8,
1/4 and 1/16 (-3.94% at 1e13), with 1/2 a regression; cache-misses:u grew only +55.5%.
**Kept at 1/8**; the cutoffs moved the same day (next entry), and the kernel became
`cross_off_checked` (2026-09-29), then `cross_off_checked210` (2026-09-30).

### `small_limit` re-tuned jointly with `med64_limit` (KEPT, 2026-09-26)

med64's lower bound is `small_limit`, so the two cutoffs stopped being independent. Dev PC,
cycles:u: a single-rep 1e12 grid (`small_limit` /2 to /8 x med64 1/16, 1/8, 1/4, ~±1% noise)
showed a smaller `small_limit` wanting a smaller med64 fraction. Best point `small=1/4,
med64=1/12`: ~2.3% below the 1/2 + 1/8 default at 1e12 (3 reps), and at 1e13 (2 interleaved
reps, no overlap) cycles:u **-2.1%**, instructions:u -2.5%, branch-misses:u **-13.7%**, with
fewer primes in med64 (14,428 vs 20,275) paying the residual exit mispredict. **Kept:
`small_limit` = `L1d/4`, `med64_limit` = `seg_k_width/12`.** Single 1e13 reps around it
(1/3 + 1/10 +0.25%, 1/5 + 1/16 +1.35%, 1/6 + 1/20 +1.48%, 1/8 + 1/24 +1.28%) confirmed it:
the 1e12 trend toward smaller cutoffs stops at 1e13. `small_limit` is still 1/4 on modern
cores; `med64_limit` moved to 1/6 on 2026-10-04.

### med64: mod-210 stepping, two variants (tried, both reverted, 2026-09-26, external review, Opus 5.5)

A per-tier profile (VM emulating the server's cache, 1e14) put small + med64 at 44% of cycles,
both on the mod-30 wheel while medium and sparse step mod 210 and skip the presieved multiples
of 7 (~1/7 of hits); estimated "realistic 2-3% net". One 210 period is exactly 7p bytes, and
`cross_off`'s unrolled shape needs a per-call 48-entry offset table
`o[w] = qp*(M210[w]-1) + C210(pr,w)`. Dev PC, cycles:u: built with 47 independent multiplies
(`cross_off210`) **+9.4%** at 1e12 and +8.6% at 1e13 (the table is rebuilt on almost every
call of a few-hits tier); with the review's 5 registers `qp*{2,4,6,8,10}` and cumulative
offsets (`cross_off210b`) **+17.2%** at 1e12 (52 ops instead of 47, in a serial chain). Both
reverted. Superseded 2026-09-30: on the checked loop, which has no per-call table, mod-210 won
(`cross_off_checked210`, below).

### Small tier: mod-210 stepping as 7 unrolled mod-30 copies (tried, reverted, 2026-09-27)

One 210 cycle is 7 consecutive mod-30 cycles, so `cross_off210<PR>` emitted 7 copies of
`cross_off`'s 8-store body with the 8 multiples of 7 removed at compile time: no extra setup.
After three codegen rounds (an `asm` barrier against GCC spilling the 48 hoisted sums, one end
check per copy, stores through an opaque pointer), dev PC at 1e11: stores -7%, instructions
-4.5%, **cycles:u flat** (101.0G vs 100.9G). A microbenchmark shows why: the mod-30 loop (~30
uops, served by the Loop Stream Detector) retires 1.04 stores/cycle, the L1 store-commit
ceiling; the 7-copy loop (~217 uops) misses the LSD, runs from DSB + MITE and retires 0.84.
Reverted; a retry needs the body under the LSD size (untested on Golden Cove). Swept
alongside: 1/4 + 1/12 still optimal at the 24 KiB sub-block; smaller sub-blocks worse.

### `cross_off`: branchless tail for the small and med64 tiers (tried, reverted, 2026-09-27)

At 1e11 med64 took ~47% of branch-misses:u (~1.6 per call) and the small tier ~33%, mostly the
checked tail's data-dependent exit. 7 unconditional stores, the out-of-range ones into a pad
past `end`: both tiers branch-misses -36% but **cycles:u +3.4%** (the small tier is
frontend/store-bound); med64 only, cycles:u -1.7% / +0.4% / 0.0% at 1e10 / 1e11 / 1e12, noise.
Reverted. The diagnosis it left: med64 takes 77% of all L1 load misses at 1e11 (~85% of its
hits, 95.5% of them served by L2), ~5.4 cycles:u per hit against ~2.8 for the small tier.

### med64: EratMedium-style checked loop, `cross_off_checked` (kept, 2026-09-29)

The 1e14 server profile put ~8 points (per 100) of the gap to primesieve in med64: the same
cost per hit as EratMedium with far more hits per call (12-682 vs 3-214), so the per-call cost
was the suspect. `cross_off` enters through 8 stack offsets and runs checked chain + unchecked
8-hit loop + checked chain (1.65 mispredicts per call); EratMedium jumps into a switch and
runs one loop with one check per hit (~1). On a wide core that compare issues beside the store
for free, so `cross_off_checked<PR>` copies EratMedium's shape (switch into `for (;;)`, 8
per-class distances); the small tier keeps `cross_off`, where the unchecked cycle still pays.
Dev PC, ABBA against `fe6260a`: cycles:u **-1.7%** at 1e12 (branch-misses -11.5%,
instructions +5.3%), -1.6% at 1e13. Server: 1e13 305.02s against the README's 308.04s
(-1.0%). Kept; the kernel became `cross_off_checked210` the next day.

### med64: re-filing without `std::vector::push_back` (tried, reverted, 2026-09-29)

After each call `push_back` reloaded end and capacity through `this` (the byte stores may alias
them). A pre-sized POD buffer with 8 local tail pointers (1 load per call instead of 3): dev
PC, 3 reps against `8b0174e`, instructions -0.8% / -1.05% (1e13 tail / 1e12), cycles:u +0.2%.
Reverted: the tier is bound by its byte stores and the exit mispredict. Follow-up (2026-10-01,
on `cross_off_checked210`): at 1e11 `-t 1` the small+medium tiers retired +8.8% instructions
against primesieve 12.7, all per call (~40 vs ~10-20 in EratMedium). A per-phase exit callback
got `emplace_back` outlined (+3.7% cycles:u); raw pointer arrays cut instructions 1.0-1.7% for
cycles:u between -1.3% and +0.1% and 630 -> 2241 asm lines. Both reverted: that instruction
gap costs only ~1.5% of cycles (46.2G vs 45.5G).

### med64: `prefetchnta` on the state stream (kept, 2026-09-29)

med64's double-buffered state (~29k primes x 8 bytes, ~2 x 233 KB per thread from 5e12 up) is
the largest non-segment stream through L2. `prefetchnta` 32 entries ahead on the read side of
`process_med64` (NT stores ruled out: 8 interleaved output streams per class are too many for
the write-combining buffers). Dev PC: 1e12 +0.5%, 1e13 10% tail -0.6%, 1e14 5% tail -3.0% with
reps spreading up to 7%. Cache counters showed a good run without NTA identical to the NTA
runs, so less L2 pollution is not the mechanism; pooling 12 clean 1e14 runs, NTA is -2.6%
cycles / -2.3% wall (28 of 36 cross-pairs, p ~ 0.09), and the three slowest runs, all without
NTA, carry ~2x the L3 misses: it seems to make the tier less sensitive to a bad memory state
(not proven). Best of N against the README: 1e13 -1.1% (322.55s vs 326.03s). **Kept, default
on, no gate** (knob since removed). Method note: a single dev PC 1e14 tail can land ~5% slow
with no code change, in cycles:u too; pair A/B tails with mechanism counters and best-of-N
full runs.

### med64: mod-210 stepping on the checked loop, `cross_off_checked210` (kept, 2026-09-30)

Today's med64 kernel. The checked loop has no per-call offset table: each hit only needs the
step from phase w to w+1, `qp*dm + corr` with dm in {2,4,6,8,10}. `cross_off_checked210<PR>`
keeps `qp*2/4/6/8/10` in 5 registers and takes `dm`, `corr` and the mask from `big::TABLE` as
compile-time constants per case: a 48-case switch into a `for (;;)`, one bounds check per hit,
skipping the 1/7 of hits whose multiplier is a multiple of 7 (presieved; every med64 prime
is > 163). Lists keyed by (class, entry phase): 8 x 48 = 384, filed by `activate_med64_210` under
`pr*48 + w`; ~10.3 KB of code against ~6.5 KB on mod-30. Dev PC, same binary, knob on/off:
1e12 cycles:u **-3.6%** (instructions -4.0%, stores -4.5%), 1e13 10% tail -1.7%, 1e14 tail
inside the noise (instructions -2.6%). Best of N against the README: server 1e12 23.00s ->
21.14s (**-8.1%**), 1e13 299.80s -> 281.98s (**-5.9%**, primesieve 292.554s: 1.02x -> 0.96x);
dev PC 1e13 -0.8%. The server's larger gain fits med64's larger share there (~33 of 100 at
1e14), not measured directly.
- Cutoffs re-swept (dev PC, 2026-09-30): `MED64_DEN` 6 and 8 tie, 16 +1.5% at 1e12;
  `SMALL_DEN` 6 / 8 +0.7% / +2.0%: below ~6k the small tier's unrolled cycle still beats med64.
- Skipping the presieved multiples of 11 (2026-10-01, a `t11` counter in the state, those hits
  stored to a scratch byte): L1 misses -4.6 to -8.8% but ~2 instructions more per hit, cycles:u
  +11.3% at 1e11 `-t 1`, +4.7% at 1e12. Reverted: removing hits with per-hit work doesn't pay.
- The full mod-2310 wheel (2026-10-01, `cross_off_checked2310<PR>`, 480 cases, 3840 lists,
  still 4 instructions per hit): instructions -0.9 to -1.7%, cycles:u **+18.4%** at 1e11
  `-t 12`, +17.5% at 1e12: ~8 x 17 KB of switch code doesn't fit the frontend the SMT siblings
  share. Closed: med64 stays on mod 210 (the mod-30 med64 path was removed later).

### med64 tier crossed off per L1 sub-block (tried, reverted, 2026-10-04)

With the cutoffs of 2026-10-03/04 (med64 1/6) med64 dominated the dense regime: dev PC, last 1e10
below 1e13, one thread, 44% of the cycles and 67% of the L1 data misses, 3.2 cycles per hit
against the small tier's 1.16. `process_med64s` crossed the band's lower part (primes below 2x
the sub-block: 17% of the primes, 64% of the hits) per L1 sub-block. Off was 9.4-24.6% faster
at every thread count (every rep): the whole-segment misses overlap (independent RMWs), while
the band adds an entry copy and a kernel entry per prime per sub-block. A miss that overlaps
is not a cost. Off by default, neutral on the i5-3470 too; removed in the 2026-10-07 cleanup.

### i5-3470 profile at 1e12: the med64 tier over the whole-L2 segment is 59% of the cycles (open, 2026-10-04)

Tower, Ubuntu live USB, 1e12, 4 threads, against primesieve 12.12 (128 KiB sieve): 1008G vs
952G cycles:u (82.15s vs 76.50s) with 15% fewer instructions and 25% fewer mispredicts: the
gap is memory. med64 is 59% of the cycles (small tier 21%): it spans ~3.8K to ~350K over the
whole 256 KiB segment, every hit an L1 miss served by an L2 that also streams the med64/medium
state, which Ivy Bridge's 168-entry ROB doesn't hide; primesieve's EratSmall runs L1-blocked
to ~23K. The L1-blocked band (`--tune med64s`) was neutral; half the L2 cut L2 misses 71% and
cycles 4% at 1e11 but cost +9.0% at the 1e12 tail even with the populations equalised (twice
the per-segment visits of every med64/medium entry, ~8 cycles each); `perf annotate` showed our
kernel and `EratMedium::crossOff_7` alike per hit, both bound by the segment-byte RMW missing
L1. The structural difference is our separate medium tier: `--tune med64=1/2 | 1/1` is -5.9% /
-6.7% at 1e12 on the i5-3470 (3/3) but +7.7% / +8.1% on the i5-11400F, the rest of the fleet
inside the noise (Ivy Bridge pays the medium tier's table-driven stepping, Rocket Lake pays
med64's state traffic). Result, two rules in `tuning.hpp` for an L2 of 256 KiB or less:
**med64 takes the whole segment** (the first 1/2 gate measured -5.4% / -5.6% at the 1e12 /
1e13 tails against 1/6; 1/1 ties it at 1e13, -6.7% vs -5.9% at 1e12), and **the base segment
stays at half the L2 while the base primes are at most 40,000** (half L2 with every prime in
med64: 1e10 -8.7%, 1e11 -4.8%, 3e11 +1.2%, 1e12 +5.5%; crossover ~2.5e11). The tower's dense
regime went from a 7-14% loss to a tie, its 1e14-1e16 tails +9-17%.

### i5-3470 follow-up: med64 gate at the tails, the +6% that was not code, and the 1e12 window profile (2026-10-04)

- The whole-segment gate at the sparse-regime tails (512 KiB segment, 2x the L2): whole and 1/2
  tie, 1/6 loses 3.3% (3/3) at 1e15 and 1e16. Gate kept.
- Tails 4-7% above the 62fc592 table with only the gate changed: the 62fc592 build against
  d8bc352 tied at the 1e15 tail, THP was fine, `--tune huge=0` still ~1% slower: machine
  state, not code. primesieve's big tails moved more (1e17 +18%, 1e18 +14%) and over five
  interleaved reps the next day spread 21.2-23.0s at 1e16 while eratostenes stayed within 0.1s.
- Counts after the half-L2 rule: 1e10 1.12x -> 1.03x, 1e11 1.06x -> 1.02x, as predicted.
- 9e11-1e12 window, 4 threads: cycles:u tie (104.8G vs 105.5G) with 21% fewer instructions,
  but 3.5x primesieve's LLC loads (2.03G vs 0.58G; its 128 KiB sieve stays in L2). That is the
  remaining dense gap here, mostly hidden; the lever, if any, is fewer state streams through
  L2, not a smaller segment. Open.

### Small cutoff 1/2 of the sub-block on a 256 KiB L2 core (kept, 2026-10-06)

On the i5-3470 the small tier ends at ~4K (1/4 of the 16 KiB half-L1d sub-block) while
EratSmall runs L1-blocked to ~23K, so the 4K-23K band goes through med64, which misses L1 on
every byte there. `make benchmark-ab`, x3 interleaved, `--tune small=1/2` against the auto: 1e13
tail **-2.8%**, 1e12 -2.9%, 1e11 -1.6%, 1e15 -0.6% (every B below every A), 1e14 0.0%;
`small=1/1` -1.1% with overlap. **Rule: `small_den = 2` on a core whose L2 is 256 KiB or less**
(the same proxy as the half-L2 and whole-segment med64 rules; startup line `small cutoff: 1/2
of the sub-block (...)`, checked by test.sh). Elsewhere the sign depends on the
microarchitecture: Emerald Rapids sandbox -3.6% / -2.6% (1e11 / 1e12), Xeon @ 2.80GHz and
i5-13500 in the noise, dev PC +3.4% / +2% for 12K vs 6K. No cache-derived rule fits, so modern
cores keep 1/4 and `--tune small=1/2` stays a manual knob (Emerald Rapids-class VM up to
~1e12); a per-machine tuning profile (`make tune`) was declined to keep the configuration
derivable from caches, threads and CPU flags.

### med64: segment-byte prefetch K * qp ahead (tried, reverted, 2026-10-06)

The dense regime's profile is the med64 kernel (51% of the cycles at 1e11, laptop emulating the
Emerald Rapids plan), bound by the segment-byte RMW missing L1. The cheapest prefetch: one
`prefetcht0 (s + K * qp, i)` per hit in `cross_off_checked210`, no lookup (mean step 4.4 qp,
so K = 9 / 13 / 20 is ~2 / 3 / 5 hits ahead). Interleaved: Emerald Rapids plan, 1e11, x3
**+14.3% / +12.6% / +8.6%**; default plan, 1e12 tail, `-t 12`, x5 **+12.5% / +19.8% /
+18.6%**. The out-of-order core already overlaps these RMWs; one more instruction per
~5-instruction hit costs more. Segment-byte software prefetch is closed for both dense tiers;
knob removed.

## Medium tier

`cross_off_medium` takes the primes above the med64 band (p >= seg_k_width / 6 by default; with at
most 256 KiB of L2 med64 takes the whole segment and this tier is empty) up to the sparse cutoff: a few hits per segment, one call per prime per segment, one list per
residue class. A prime is a byte position stepped through the 48 mod-210 multiplier phases by
one packed `PACK210[PR][w]` word, its state is struct-of-arrays (`dyn = (pos << 6) | w`, `qp`
as 1-byte deltas) with gated `prefetchnta`. Cost: 22 instructions per prime visit, 13 per hit;
bound by each prime's loop-exit mispredict and the pos -> table -> pos chain, not by hits.

### `cross_off_medium`: 4-way interleaved stepping (tried, reverted)

Four primes' independent chains in one loop, to hide mispredict/load latency (not SIMD;
AVX-512 is fused off on the i5-13500). cycles:u +13.2% at 1e11, +14.3% at 1e12: instructions
+25.6%/+30.8%, branch-miss rate barely moved, and a lane with fewer hits pays an `if (aN)`
check until the last lane finishes. Reverted.

### `cross_off_medium`: EratMedium-style 64-list restructuring

Reuse `cross_off<PR>` byte marking with 64 lists keyed by (class, entry phase), so the phase
switch is a compile-time jump per list. Attempt 1 (never committed): 1e12 -11.5% cycles:u,
1e13 only -5.3% while the cache-miss rate doubled; reverted on that trend. Attempt 2
(2026-09-25, fresh double-buffered rewrite, dev PC): 1e12 -5.8%, 1e13 **+3.25%** (instructions
-18.9%, cache-misses:u +95.8%). Closed for the whole tier: fewer instructions for more cache
pressure loses as the population grows. Later kept for a bounded sub-band only, as the med64
tier (entry on the 64-list restructuring scoped to a sub-band).

### `cross_off_medium`: mod-210 multiplier stepping (2026-09-24)

Medium primes are > 163 and the presieve always covers 7, so multipliers divisible by 7 are
redundant: stepping through the 48/210 phases coprime to 210 instead of 8/30 skips ~14% of
candidate hits (the sparse tier's big-wheel trick). A first version chained lookups through a
loaded "next" field and regressed despite fewer instructions; the kept one indexes by a
register phase `w`. cycles:u: 1e11 -2.6%, 1e12 -2.3%, 1e13 -0.13% (noise; a same-day
`seg_k_width` change had moved large medium primes to the sparse tier). Kept: never worse
than flat, no memory cost. Its bit-granularity tables were replaced on 2026-09-27.

### `cross_off_medium`: class-specialized layout (kept, 2026-09-24)

As primesieve's `EratMedium` (`crossOff_7/11/.../31`): PR is a template parameter, one list per
class (`medium_[8]` in `segment_sieve.hpp`, like `small_[8]`) instead of a flat list with a
runtime `ri`. The correction row becomes a compile-time offset, one multiply less per prime
per segment; the per-hit `qp * gap` stays (per-prime deltas over 48 phases would cost more
than 1-3 hits per segment recoup). 8 lists, no migration between them, unlike the 64-list
attempt. The population saturates once sqrt(N) passes `seg_k_width` (between 1e12 and 1e13
on the dev PC) and results have flipped sign across that point: measure both.

### `cross_off_medium`: mod-2310 stepping, considered, not implemented (2026-09-25, external review, Opus 5.5)

Mod-2310 (480 phases) removes ~9% more candidate hits, but the tables grew to ~30KB (sparse)
and ~17.3KB (medium), near the E-cores' 32 KiB L1d, and the sparse tier's 32-bit state would
keep 20 bits of `qp` (max prime ~31.46M < isqrt(1e15)), a correctness limit. Update
2026-09-30: the sparse half was kept once its entry became a 64-bit word and the table 15 KiB.
Update 2026-10-01, medium half tried and reverted: sharing `big::TABLE2310` (`--tune
medium2310`, `-t 12`): 1e13 tail -0.6% cycles, 1e14 tail +1.1%, 1e12 +1.0%. 9% fewer hits in
the largest tier (40% of dev PC cycles at 1e13) moved cycles by noise: it is bound by each
prime's loop-exit mispredict, not its hits. Don't retry for hit count alone.

### `cross_off_medium`: 2-ahead software prefetch (tried, reverted, 2026-09-25, follow-up session)

`perf annotate` at 1e13 put 66-67% of the loop's cycles after the segment store (~30% L1
misses), so hit N+2 was prefetched one iteration early, by a k/k1/k2 chain or by two-step
tables. Both cut the L1 miss rate (up to -10.8pp) and both cost ~10-11% cycles:u (+10.1% at
1e12, +11.8% at 1e13; two-step tables +5.8% to +11.4%): the extra lookups cost what the
latency saves. Reverted; closes software prefetch of the segment store.

### `cross_off_medium`: byte positions + doubled tables (kept, 2026-09-27)

At 1e12 on the dev PC medium ran 38% of all instructions at IPC 1.41, the one
instruction-bound marking tier (~17 instructions per hit).
- v1: byte position + per-(class, phase) mask, `s[pos] |= MASK210[PR][w]`, step
  `qp * DM210[w] + CORR210B[PR][w]`, instead of `k >> 6`, a variable shift and a 64-bit RMW.
- v2: tables hold two 48-phase cycles, the loop does `if (++w == 96) w = 48` (never taken with
  med64 on) and folds `w` once per call. ~11 instructions/hit. cycles:u 1e12 -3.3%, 1e13
  **-3.4%** (ABBA), instructions -11.8%; the gain grows with the tier's share of the run.
- v3: one `PACK210[PR][w] = mask | dm << 8 | corr << 16`, 2 loads per hit instead of 4.
  cycles:u -0.37% at 1e12, -0.5 to -1.1% at 1e13. Kept: same direction, one table not three.

`GAP_K210`/`ONFLY_CORRECTION210` were removed. `med64_limit` re-swept after v3: 1/12 stays.

### `cross_off_medium`: struct-of-arrays state + gated `prefetchnta` (kept, 2026-09-29)

Dev PC, 1e14 tail: medium had 43% of cycles and 40%/50% of L2/L3 misses, mostly on
`s[pos] |= mask`: the segment, evicted by the tier's own 8-byte-per-prime state (~1.1 MB per
thread, rewritten every segment; ~1 MB x 20 threads fills the i5-13500's L3).
- SoA: `dyn[i] = (pos << 6) | w` (rewritten) and `qps[i] = qp` (read-only, stays clean, no
  writeback); `pos` fits 26 bits, checked by `SegmentSieve`'s constructor.
- `prefetchnta` on both streams, 32 entries ahead, once per prime (not per hit), so they reach
  L1 without being allocated in L2.

cycles:u vs `8b0174e`, SoA / SoA + NTA: 1e12 tie / +0.8%; 1e13 tail -2.0% / **-6.1%**; 1e14
tail -5.2% / **-12.1%** (wall -13.8%). NTA only pays once the state overflows the caches, so
it is on when medium primes x 8 bytes exceed the per-thread L3 share (`MEDIUM_NTA_MIN_PRIMES`,
131,072 on the dev PC; `ERATOSTENES_MEDIUM_NTA=0/1` forces it).

### `cross_off_medium`: `qp` as 1-byte deltas (kept, 2026-09-30)

After SoA each prime still streamed 4 read-only bytes of `qp` per segment from L3/DRAM. Lists
are sorted by p and never reordered, and the largest same-class gap below sqrt(1e15) is
52 * 30, so `medium_qd_[pr]` holds `uint8_t` deltas from `medium_qp_base_[pr]` and
`cross_off_medium` does `qp += *qds` per prime (the hit loop is unchanged); activation throws
if a delta exceeds 255. 5 bytes per prime per segment instead of 8. Dev PC, ABBA vs
`c5ec94f`: 1e14 tail **-4.5%** cycles:u, 1e15 tail **-8.9%** mean / -4.5% min (L3 misses
-44%), 1e13 tail -1.2%, 1e12 tie. `MEDIUM_NTA_MIN_PRIMES` still assumes 8 bytes per prime
(re-measured: [next entry](#medium-prefetchnta-gate-re-measured-with-the-5-byte-state-kept-2026-10-09)).

### Medium prefetchnta gate re-measured with the 5-byte state (kept, 2026-10-09)

The gate turns the medium prefetchnta on at medium primes x 8 bytes >= the per-thread L3
share (131,072 primes on the dev PC), sized when the state was 8 bytes. At 5 bytes a gate
of share / 5 (209,715) would differ only where the medium tier holds 131K-210K primes: on
the dev PC, 12 threads at N ~7e12-1.4e13 (171,173 at 1e13, ~856 KB of state, under the
1 MiB share). Dev PC, `benchmark_ab.sh`, last 1e11 below 1e13, 8 interleaved pairs,
cycles:u:
- 12 threads, 171,173 medium primes (gate on): off is **+1.05%** median, +0.76% mean,
  lower in 0/8 sorted pairs. The prefetch still pays below the share.
- 6 threads, 99,137 medium primes (gate off): on is +0.74% median, -0.47% mean, 4/8:
  neutral, so no reason to lower the gate either.

Kept at 8 bytes per prime: share / 5 would switch it off where it wins ~1%.

### Medium tier in fixed-iteration bands, predicated hits (tried, reverted, 2026-10-02)

The data-dependent exit is one mispredict per prime per segment, 60% of the i5-13500's branch
misses at 1e14. Bands of primes sharing one fixed, predicated trip count (`--tune medband`)
cut branch misses by 51% (`-t 12`) and 45% (`-t 2`), but cycles rose +11% and +1.5%: ~11G of
mispredicts removed against ~+30G of predicated work. The exit mispredict is cheap, overlapped
with the s[pos] misses; the cost is work per hit. Reverted.

### `cross_off_medium`: two primes per iteration (tried, not adopted, 2026-10-04)

`cross_off_medium_pairs` (`-DERA_MED_PAIRS=1`, `make medpairs`) interleaves two consecutive
primes of a class while both are in the segment, to overlap their dependent chains. Dev PC
A/B: worse or noise everywhere (1 thread 1e12 +5.3%, 12 threads 1e13 +3.4%, both 4/4 worse):
the loop is bound by its exit mispredict and per-call cost, and the drains add exits. Kept as an
off knob until the 2026-10-07 cleanup removed it.

### `cross_off_medium`: per-hit phase-wrap test dropped when the plan bounds the hits (tried, neutral, reverted, 2026-10-05)

`perf annotate` at the 1e15 tail (dev PC): **22 instructions per prime visit, 13 per hit**,
two of them the `++w == 96` test, unneeded with med64 on (at most 7 hits per segment). A
`WRAP` template parameter chosen at plan time: instructions:u -0.9%, cycles:u equal, wall
+1.8%/+0.9% (overlapping). Two of thirteen per hit buy nothing on a mispredict- and
latency-bound loop. Reverted; the counts stay as the tier's cost model.

## Sparse tier

Base primes with about one hit per segment or fewer, on primesieve's EratBig design: each waits
as one packed 8-byte entry in the ring slot of the segment of its next hit, in chains of 4 KiB
blocks from pooled arenas (2 MiB huge pages with one thread per core). `process_big` drains one
slot per segment on a mod-2310 multiplier wheel, two entries per iteration in groups of 4, with
a segment-byte prefetch 16 entries ahead and the next block prefetched across the current one.
The code now lives in `src/sparse_tier.hpp`, class `SparseTier`, same method names.

### Sparse tier (original design, before the EratBig rewrite above): stepping-math attempts 1-5

Each sparse prime is scheduled into the future segment of its next hit (a ring of queues), so a
segment only looks at the primes due in it. Five attempts at removing the division by runtime p:
shared-table stepping (attempts 1, 3) and an AoS relayout (2) lost (attempt 3 at natural 1e13:
cycles:u +12.7%, its 24-byte per-prime state nearly tripled a randomly accessed array); storing
`m` (4, superseded) gave -3.0%. **Kept: attempt 5**, qp/pr recomputed by compile-time-constant
divisions and k+j packed in one word: proxy (1e12, `-s 500000`, 84% sparse) cycles:u -8.2%,
natural 1e13 wall -0.7%. All of it later replaced by the pooled blocks and the rewrite.

### Sparse tier: `process_big`/`process_sparse_bucket` split into its own noinline function

Inlined with the dense tiers, the function spilled a loop-invariant (`num_buckets_`): too many
values live at once. A `noinline` function gets its own register allocation: 1e12 with a third
of base primes forced sparse, cycles -5..-9%, IPC 1.07 -> 1.14-1.19. The call is skipped when
the run has no sparse primes. **Kept.**

### Sparse tier design, current: fixed-size pooled blocks (attempt 6)

The idx-indexed intrusive list gave way to fixed-size blocks from a pool, one chain per ring
slot, as in EratBig. The entry carries all its state, so re-scheduling copies it to the target
slot's tail block: draining and appending are both sequential, no pointer chase at random idx,
8 bytes per live entry (attempt 5 needed 12). Still the design; the entry was later repacked
into one 64-bit word (mod-2310 entry) and the blocks grown to 4 KiB.

### `SPARSE_BLOCK_ENTRIES` tuning: 1024 vs. 128

1024 entries (8 KiB, EratBig's default) won at natural 1e13 (-10.2% cycles:u) but lost +8.3% on
the forced-sparse proxy, whose many ring slots each held a mostly empty block. 128 (1 KiB) won
on both: proxy -8.4%, natural 1e13 cycles:u 23.16T -> 20.78T (-10.3%). **Kept**, superseded by
4 KiB on 2026-10-02. A block-size change must be checked in both regimes.

### Sparse tier attempts 7-10 (all tried, reverted)

- **7**: auto width rounded down to a power of 2 (slot division -> shift): cycles:u flat at 1e12
  and 1e13, cache-refs +12.9% at 1e13. Later a requirement of the EratBig rewrite anyway.
- **8**: prefetch `s[(it+4)->pos]`: cycles:u +3.5% / +1.5%; the L2-sized segment has no long
  miss to hide. (A 16-ahead version was kept on 2026-10-02 at 12 threads.)
- **9**: 4-way software pipelining: IPC up, but instructions:u +3.1% (remainder loop), cycles:u
  +1.1% both reps.
- **10**: primesieve v12.7's sliding window instead of the modular ring: proxy -0.25/-0.33%, natural
  1e13 +1.93%; the shift is paid on every call, and most calls have nothing due.

### Sparse tier: EratBig-style rewrite (adopted, 2026-09-24, isolated test of point 1 from an external review, Opus 5.5)

The original tier was swapped for an EratBig-style one: byte marking, a mod-210 multiplier wheel
(48/210 phases instead of 8/30; multiples of 7 land on composites the small tier's pass for 7
already crosses off), and pointer-aligned blocks (a tail on a block boundary means "full",
primesieve's `Bucket` trick, no count field). The slot math needs a power-of-2 segment width in
bytes (checked in the constructor, the width floored when the tier is used), so the rewrite's
numbers bundle that rounding, which alone was no win (attempt 7). Adopted; the small and medium
tiers untouched. The wheel became mod 2310 on 2026-09-30.

### Sparse tier: prefetch the next block of the chain, once per block (kept, 2026-09-27)

At E14+ the tier's state fits no cache (~15.6 MB per thread at E15) and a slot's blocks are
scattered (LIFO free list), so the streamer restarts at every block. `process_big` now issues
`prefetcht1` for the whole next block when it loads `next_blk`. Dev PC: 1e13 `-s 1000000`
(~20 MB, DRAM-bound) cycles:u **-7.6%**, IPC 1.10 -> 1.19; natural 1e13 -0.8%. **Kept**,
replaced by the spread version on 2026-10-04. Follow-up `prefetchnta` (2026-09-30): +6.1%
cycles, L3 misses +40%, reverted: a drained block is the next write target of another slot and
must still be in L2, or every rewrite pays an RFO.

### Attempt 11: shrinking the live entry from 8 to 7 bytes (tried, reverted, 2026-09-27)

On the theory that the tier is memory-bound (the E14 ratio gap grows with its population): idx 9
+ qp 21 + pos ~18-20 bits fit 7 bytes, with an explicit tail count per slot since 7 can't divide
a power-of-2 block. Correct, but on the proxy cycles:u **+10.4%**, instructions:u **+16.8%**
(identical across reps): the unaligned 7-byte stride defeats clean loads/stores. Code removed.
Don't re-propose a sub-8-byte entry without vectorized pack/unpack or an SoA layout.

### Segment processed in parts for the sparse tier (tried, reverted, 2026-09-29)

i5-13500 1e14 profile (primesieve = 100): the whole gap is in primes >= 6k, med64 ~+8 and sparse
~+5 (~20% dearer per hit than EratBig); excess slots 57% backend, 37% bad speculation. A 256 KiB
segment (server) helped sparse (-9.4%), small and presieve but hurt med64 (+9.4%) and medium (+44%).
Tried: keep 512 KiB, process presieve/small/sparse in `ERATOSTENES_SPARSE_PARTS` parts. Server:
parts=1 +3.1% cycles, parts=2 -0.6% vs the old binary; dev PC +4%. Reverted. Method note: the
restructuring had changed GCC's inlining; before an A/B of a structural change, `nm -C -S` both
binaries, diff the hot functions, and keep the previous commit's binary as a control.

### Sparse tier: `process_big` loads per hit, ~12 -> 5 (kept, 2026-09-29)

The hit loop is load-port bound and did ~12 loads per hit (entry as two fields, table row as 4
bytes, three spilled constants, `tail_.data()` reloaded behind the aliasing `s[pos]` store).
Now: `big::TABLE64` (one `uint64_t` per row, as `PACK210`), the entry as one 8-byte `memcpy`,
`tail_.data()` hoisted, the new-block path out of line (`new_block`, noinline). 5 loads per hit.
Dev PC neutral (no sparse primes to 1e13 at cutoff 1/1); aimed at the i5-13500, where
`process_big` is ~30% of cycles at 1e14 and ~39% at 1e15. **Kept**; the one-word entry is what
made mod-2310 possible.

### Sparse tier: mod-2310 multiplier wheel (kept, 2026-09-30)

All primes up to 163 are presieved, 11 included, so hits `p*m` with `11 | m` are redundant.
Stepping `m` mod 2310: 480/2310 = 0.2078 vs 48/210 = 0.2286 candidates, **-9.1% sparse hits**,
ground EratBig (mod 210) doesn't cover. Entry packing: `idx` (ri*480 + w) bits 0-11, `pos`
12-35 (log2(segment bytes) <= 24, checked), `qp` 36-63 (p up to ~8e9); `big::TABLE2310` is 8 x
480 rows of 4 bytes (`mask | dm << 8 | corr << 16 | next << 20`), 15 KiB, generated and
range-checked at compile time. A 16-bit-row table cost 43 instructions per hit vs 37; with the
next index stored it's 38. `process_big<bool W2310>`, mod-210 behind `--tune big2310=0` (removed 2026-10-07). Dev PC
(1.66M sparse at 1e15): 1e15 0.1% tail cycles:u -5.1% mean / -4.2% min, 1% tail -7.7% / -6.3%,
L3 misses -32% (the shared bucket stream shrinks for all tiers). Full 1e14 4185.20 s vs the
README's older 4530.74 s, ~3.5% from this. **Kept.** Not extended to medium (1-3 hits per
segment, no room for 480 phases) or med64 (a 480-case switch).

### Per-tier cycles against primesieve at `-t 2`, and the sparse tier's block size: 4 KiB (kept, 2026-10-02)

Dev PC, 1e15 tail, `-t 2`: below 2.1M the programs are even (83.7G vs ~82G); the gap is the
sparse tier, 57.9G at IPC 2.36 vs EratBig's ~43G at 2.84 (same instructions per hit). On the
i5-13500 they tie (process_big 38.3G vs ~35G), so the tier suffers where the segment is the
whole L2. Block size (`-DERA_BLK_BYTES`, cycles:u): 1e15 tail 4 KiB vs 1 KiB **-3.9%** at
`-t 2`, **-3.6%** at `-t 12`, 8 KiB worse than 4 KiB in both; 1e13 +0.9%. **4 KiB kept**: fewer block
boundaries, and the pool still fits beside the segment. Dev PC tails -5..-11% at 1e15-1e18,
Emerald Rapids sandbox -6..-8% at 1e16-1e18.

### Two more `-t 2` probes on the sparse tier and the presieve (both tried, neither kept, 2026-10-02)

- `--tune big2310=0` (3 KiB table vs 15 KiB) at `-t 2`: a tie (148.0G vs 146.4G); at `-t 12`
  mod-2310 keeps its -4%. The table isn't what lowers the sparse IPC. Default kept.
- `Presieve::fill` in one pass over 16 tables instead of 4 of 4: +2..+5.5%. Reverted.

### Sparse tier: two entries per iteration in `process_big`, and a segment-byte prefetch 16 entries ahead (both kept, 2026-10-02)

Like EratBig, two entries per iteration: both loads, table rows and segment RMWs before either
push, pushes in order (a shared slot's second push reads the first's tail). Dev PC cycles:u:
`-t 2` -2.2% (1e15) and -1.4% (1e18), a tie at `-t 12`; 4 per iteration lost to register
pressure (`ERA_BIG_UNROLL`), so 2. Prefetching the segment byte of the entry `ERA_BIG_PF` = 16
ahead (`pos` is in the entry): `-t 12` 1e15 -2.3..-3.3%, 1e18 -5%, every run below every PF=0
run; a tie at `-t 2`, where the segment sits in L2. Both **kept**. Running the sparse tier
before med64/medium: L2 misses -7%, cycles -1.4% / +1%, not kept.

### Sparse tier: next-block prefetch spread over the current block (kept, 2026-10-04)

`perf annotate` put **22% of `process_big`'s cycles on the next-block prefetch**: 64 `prefetcht1`
in a burst at each 4 KiB boundary overflow the miss queue and stall. Spread instead
(`ERA_BIG_PFSPREAD`): during entries 0..255, prefetch line idx/4 of the next block. Dev PC, 12
threads, every B below every A: -4.8..-5.3% at the 1e14-1e17 tails, -6.6% at 6 threads; cycles:u
-3..-4.4% with +3.4% instructions. i5-13500: neutral at 1e14-1e16, -1.1% / -2.3% at 1e17/1e18.
i5-3470: **-3.9%** (3/3). **Kept**; refined by the groups of 4 on 2026-10-06.

### Sparse ring arenas as 2 MiB huge pages, with one thread per core (kept, 2026-10-04)

The ring's active write set grows with isqrt(N) (~1024 tail blocks, 4 MiB of pages at 1e18).
Smaller blocks lost (1 KiB +11.7%, 2 KiB +1.3%). Arenas as 2 MiB `MADV_HUGEPAGE` regions, dev
PC: 2 threads -12.1% at 1e18 (1e10 window), -2.9% at 1e17; 12 threads +10.5% in one round,
-1.0% in another. So tuning.hpp turns them on with one thread per core only (`--tune huge=1|0`
forces). Turning them off costs +1.2..+6.5% on the Emerald Rapids sandboxes and +4.1% on the
i5-3470 at 2 threads; forcing them on the i5-13500 at 20 threads costs +20.4%. **Kept.**
Prefetching the push target (`ERA_BIG_PFPUSH`): +8.9..+10.7% at 2 threads, never on (removed
2026-10-07).

### Sparse tier: marking a prime's further hits in the same segment in a loop before re-filing it (tried, reverted, 2026-10-04)

EratBig marks a prime's hits in a loop and re-files once per segment; `process_big` re-files
after every hit. `-DERA_BIG_LOOP=1`: +14.9% / +10.5% at 1e14/1e15 (2 threads), +19.6% at 1e18
(12 threads). The re-file keeps every hit in the batched two-entry stream; the loop is a
dependent chain with a data-dependent exit. Reverted; the A/B flag was removed on 2026-10-07.

### Sparse tier: `process_big` is issue-bound at 12 threads; the ring's wrap mask and the spills are what is left (open, 2026-10-05)

~47 instructions per hit at ~20 cycles per hit per core at 12 threads: **instruction-bound with
HT**, latency-bound with one thread per core. 21 live values for 15 registers (stack reloads of
`tails`, `bmask`, `modsb`, the next block). Done, by callgrind on the laptop (i5-1235U, WSL2):
- **Wrap mask dropped, kept**: `head_`/`tail_` hold 2 x num_buckets_ slots, a hit files into
  `cur_segment_ + ahead` unmasked, `wrap_ring()` copies the upper half down once every
  num_buckets_ segments. -2.2% Ir; i5-3470 instructions:u -2.7..-3.5% at 1e14-1e17, cycles:u
  -0.5..-2.7% at 1e15-1e17 (1e14 a wall tie); i5-13500 -6.3% cycles at 1e17. No machine loses.
- `tails_cur` (tail array + cursor as one pointer) -2.2% Ir; `bzhi` for `pos & modsb` (BMI2
  only) plus an unconditional spread prefetch (a chain's last block points at itself): together
  223.6 M -> 211.1 M Ir (-5.6%). Committed, in today's code. `ERA_BIG_SUBSHIFT` +1.1% Ir,
  rejected.
Register allocation here is a lottery: every change needs its own callgrind and asm read.

### Sparse tier: `process_big` in groups of 4 entries, no per-iteration edge tests (kept, 2026-10-06)

15-19 of the 82-86 instructions per pair of entries were block-edge tests paid every iteration
(spread `idx < 256`, the `end` test before the segment prefetch). `ERA_BIG_FASTBLK`: the number
of groups of 4 whose 16-ahead neighbours stay in the block is computed once per block; the first
min(groups, 64) groups also prefetch line g of the next block, the last entries go through the
plain pairs. ~37 instructions per hit, `process_big<true>` **-11.4% Ir**. Wall: i5-3470 (one
thread per core) -0.54..-1.94% at 1e15-1e18; i5-13500 (20 threads) -1.2% / **-4.7%** / -1.6% at
1e16/1e17/1e18 (1e15 within noise); the claude.ai sandboxes gain at 1e16-1e18. **Kept**
(the only path since the 2026-10-07 cleanup). Leftovers: the spread's line pointer and one table row still go through the stack.

## Activation and the medium/sparse cutoff

Today a base prime goes to the sparse tier from `seg_k_width / den`, den = 1, 2 or 4, whichever of two
gates lowers it more: the per-thread L2 share (inside the sparse regime only: 1/2 from 512 KiB, 1/4 from 1 MiB)
and the L3 per active thread (L3 / min(threads, sharers): 1/2 from 1.5 MiB inside the sparse regime, 1/4
from 4 MiB, also below it when an octave of base primes lands in the sparse tier); `--tune sparse`
overrides. Sparse primes are a bitmap on the wheel that activation walks with ctz, bounded by one isqrt
per segment; deriving `p` from the bitmap index (`ERA_ACT_IDX`) is on only with BMI2; the 64-bit division
stays (the alternatives below were removed in the 2026-10-07 cleanup).

### EratBig-style sparse tier: forcing a power-of-2 segment width, and `sparse_limit = seg_k_width/4` (all attempts reverted)

The EratBig-style sparse tier needs the segment width in bytes to be a power of 2 (slot math by
shift/mask); the width is forced only when `base_limit >= seg_k_width`, i.e. when some base prime will
actually be sparse, and left as auto-tuned otherwise. On top of it, an external review (2026-09-24)
proposed `sparse_limit = seg_k_width/4`, after primesieve's EratMedium/EratBig split: the medium primes
closest to `seg_k_width` touch their state most segments for no hit. Dev PC (i5-11400F, 256 KiB segment
then), cycles:u: 1e12 -1.0%, but 1e13 **+3.8%** (medium 152886 -> 40665, sparse 72036 -> 184257, 2.6x;
cache-misses:u +92%). The unchanged retry (2026-09-25) gave +8.2%. With `BLK_BYTES` 1024 -> 4096 (512
entries per block) 1e12 won in wall (~30.8-31.0 s vs ~31.3-31.5 s) but 1e13 still lost, non-overlapping
(437.60 / 489.73 s vs 415.33 / 430.17 s). Reverted three times; the suspected cause, never isolated, was
the 2.6x bigger sparse population's footprint in the ring, which block size does not shrink. Superseded
in part: once the segment was doubled, the cutoff was lowered under cache gates (the later server-gap, 1/4
from 1 MiB and one-thread-per-core entries); the dev PC at 12 threads still keeps 1/1.

### Medium/sparse cutoff raised above `seg_k_width` (tried, reverted, 2026-09-27)

The cutoff compares a prime's value with `seg_k_width`, a width in wheel indices, while a segment spans
`seg_k_width * 30/8` numbers: on a 256 KiB segment the sparse primes between 2.1M and 7.86M still hit
every segment 1-3 times, which looked like work for the cheaper byte-position medium tier. Raised through
an `ERATOSTENES_SPARSE_NUM/_DEN` multiplier, last 1% of 1e14, cycles:u (control 2.910T / 2.890T): 1.5x
+1.3%, 2x +5.9%, 3x +21%, 4x +39%, counts identical. Monotonically worse: the sparse tier handles 1-3
hits per segment fine, while a medium prime pays a fixed cost every segment (8-byte state read and
write, ~0.5 loop-exit mispredicts per call at 1e14). Reverted; `p < seg_k_width` stays as the upper end.

### i5-13500 server gap vs primesieve: medium-tier call count, sparse cutoff 1/2 gated on per-thread L2 (2026-09-28)

The dev PC was below primesieve at every N (0.92-0.98x); the server (i5-13500, 6P+HT + 8E, 20 threads,
Docker only) was not, and the gap grew with N: 0.98x at 1e12, 1.05x at 1e13, 1.14x at 1e14.

- **Method.** A 1% `ERATOSTENES_START` tail gets ~1.5 chunks per thread and leaves cores idle (9.7 of 12
  busy): the earlier 1.19-1.29x wall tail ratios were mostly that artifact. Decide on 10% tails (idle
  1.6%, 1.18x) with ABBA; even cycles:u moves with the idle share on an HT machine. The suspected
  per-chunk setup cost was refuted on the dev PC (72 -> 288 chunks: ~4 ms each; the 9 -> 72 jump was
  idle cores turboing the busy ones): ~0.01% of a full run.
- **Ruled out:** work distribution (idle 1.6-2.1%); hybrid cores (cycles 1.14x on both `cpu_core` and
  `cpu_atom`); E-core contention (P-cores alone already +18% cycles:u, 19.37e12 vs 16.45e12); segment
  size (no width closes it, 512 KiB stays).
- **L2 misses are a symptom.** LLC-loads 14x primesieve's (L2 hit 95.0% vs 99.7%); a 256 KiB segment
  cut them -84% but cycles:u went **+5.2%**: the scattered segment stores overlap (memory-level
  parallelism).
- **TopDown:** bad speculation 2.13x primesieve's, ~70% of the net gap, all of it branch mispredicts.
  By tier, medium alone has 9.11e9 mispredicts (0.54 per call), more than all of primesieve (7.61e9;
  EratMedium ~0.66 per call). The loop is fine, the call count is not: medium + med64 = 295k primes x
  63.6k segments = 18.8e9 calls, 2.4x primesieve's, because our medium tier runs down to ~1 hit per
  segment (`p < seg_k_width`, 4.19M) where EratMedium stops at ~2.7 (1.57M here).
- **Cutoff sweep** (lowering only, `ERATOSTENES_SPARSE_NUM/_DEN`), 1% tail of 1e14, P-cores, cycles:u:
  3/4 -3.9%, **1/2 -6.0%**, 3/8 -5.3%, 1/4 -1.1%: U-shaped, below 3/8 the per-hit bucket cost wins.
  The 10% tail ABBA: **-2.3% cycles:u, -2.5% wall**, mispredicts -27% (medium now 4.76e9, below
  EratMedium's 5.17e9); gap 1.173x -> 1.146x. 1e15 (5% tail ABBA, ~47 min per run): +0.2% cycles:u,
  -0.4% wall, a tie (the 0.1% tail's +5.6% wall was the tail artifact; sys < 0.5 s). Full 1e13, 20
  threads: -0.04%, a tie. Full machine at 1e14 (10% tail): **-1.8% wall**.
- **The dev PC disagrees:** full 1e13 -2.4% and full 3e12 -1.2%, but the 1e14 10% tail **+8.7%** at 12
  threads, so the briefly-kept default 1/2 was reverted. Pinned `-t 6` (one thread per core): +0.1%, a
  tie. L2 per thread fits every point (256 KiB loses, 512 KiB ties, 640 KiB wins); L3 per thread does
  not (the server's 1.2 MiB per thread wins, the dev PC's 1 MiB loses).
- **Adopted, gated:** 1/2 when `sparse_regime` and the smallest detected per-CPU L2 share is >= 512 KiB
  (i5-13500 on, i5-11400F off). The threshold sits on the measured tie point; 384 KiB is unmeasured.
- Side results: a med64_limit re-sweep at 1e14 was flat inside the ~4% noise of the 1% tail; at 1e15
  `process_big` is 39.1% of cycles:u and half of it is the `s[pos] |= mask` byte RMW into the segment.

Later refined by the 1/4 step from 1 MiB of L2 and by the L3-per-active-thread gate.

### Activation cost at the top of N (2026-10-02)

At the last 1e11 below 1e18 (dev PC, 12 threads) the sieve was ~9% faster than primesieve per wheel
index but paid ~1.6 s more fixed cost (T(W) = F + c*W: F 3.6 s vs 2.0 s): 0.45 s of single-threaded
base-prime phase and ~1.5 s per thread activating 50.5M sparse primes (29 ns, ~130 cycles each).

- Kernel time (10.6 s sys vs 14.4 s user on a 1e9 window, 1.42M page faults from the 406 MB bucket pool
  per thread): primesieve holds the same entries and pays the same.
- Transparent huge pages for the pool: faults 1.42M -> 0.22M but wall 2.3 s -> 4.3 s (direct
  compaction, 12 threads at once). Reverted.
- Batches of 16 with a write prefetch of each target bucket tail: 28-35 ns vs 30-33. Reverted.
- **Kept (e1b177a): base primes as a bitmap on the wheel.** The sparse tier is a run of the bitmap
  (`SparsePrimes`) that activation walks with ctz: 33 MB instead of 406 MB at N = 1e18, sieved in 0.12 s
  instead of 0.41 s, 20-22 ns per prime instead of 23-31. Last 1e10: 1e17 2.29 -> 1.83 s (-20%), 1e18
  3.75 -> 3.30 s (-12%), ~0.45 s; on the 1e11 window a tie within noise.

### Sparse cutoff 1/4 from 1 MiB of L2 per thread (kept, 2026-10-03)

Operators' 2-vCPU VMs (no SMT; Emerald Rapids with 2 MiB of L2 per vCPU, the Xeon 2.80 with 1 MiB),
e379255, last 1e11 below 1e14 and 1e15, x2: `--tune sparse=1/4` against auto (1/2) was -5% at both tails
on the clean Emerald Rapids (4/4) and -2..-4% on the Xeon 2.80. The i5-13500 (640 KiB per thread) had
1/4 behind 1/2 (cycles:u -1.1% vs -6.0% at 1e14, +6.2% vs +0.2% at 1e15) and the dev PC at `-t 2` tied
them: the bucket ring's extra blocks want room in L2. So the per-thread L2 gate got a second step:
**1/2 from 512 KiB, 1/4 from 1 MiB**; the dev PC, i5-13500 and i5-1235U are unchanged, and the startup
log prints the cutoff and why. Confirmed on 691509d (1/4 as default): 1/2 is +3..+4% on the clean
Emerald Rapids, +2.5% on the Xeon 2.80, +2.4% on the noisy host. Tails there: the clean Emerald Rapids
had all six under 1.00x for the first time (0.90-0.99x), the Xeon 2.80 1.02-1.16x. Open side thread: on
the Xeon 2.80 the 1 MiB segment (segment ceiling lifted) on top of 1/4 is -3.2% / -3.8% (4/4), so the
ceiling's `L2 / 2` term has two rounds against it and one for it; next, the same test at 1e16-1e18.

### One thread per core: the medium tier's per-call cost, and the sparse cutoff by active threads (2026-10-03, evening)

`perf record` at `-t 1`, dev PC, last 1e10 below 1e13 (512 KiB segment, no sparse tier yet): 15.39G
instructions / 9.61G cycles (IPC 1.60) against primesieve's 17.00G / 8.22G (IPC 2.07), 1.18x wall. The
whole gap sits above p = 6K (11.9G instructions / 7.52G cycles vs 13.4G / 5.90G): 197,708 medium primes
walked in each of 636 segments, 126M calls at ~60 instructions and ~34 cycles, where EratBig takes the
primes above 786K at IPC 3.6 with no per-segment cost. Lowering the cutoff on the dev PC pays a lot with
1-2 active threads (`-t 1`: 1/4 -10.5% at 1e13, -12.4% at 1e14; `-t 2` 1e14: 1/2 -19.9%, 1/4 -13.8%),
fades at 6 and hurts at 12 (1/4 +13.9% / +13.0% at 1e13 / 1e14): every optimum fits "L3 per active
thread", never looked at by the L2 gate.

- **Kept: the L3-per-active-thread gate** (tuning.hpp): 1/4 when the L3 (sysfs, cpu0's) divided by
  min(threads, its sharers) is >= 4 MiB; below the sparse regime only when an octave of base primes
  lands in the sparse tier (`base_limit >= 2 x seg_k_width / 4`; the power-of-2 fixup handles the new
  sparse tier). Dev PC against the old 1/1: `-t 1` 1e13 -10.0%, `-t 2` 1e13 -10.6%, 1e14 -9.2% (4/4).
  This step stays off at `-t 6`/`-t 12`, on the i5-13500 (1.2 MiB per thread; 1/4 there at 1e13 is
  +3.2%, 3/3), the HT laptops and the 2-vCPU Xeons at 1e13.
- **The 1/2 step, from the i5-3470 tower** (Ivy Bridge, 4 cores, no SMT, 256 KiB L2, 6 MiB L3, the first
  one-thread-per-core metal; tails 0.92-1.01x, counts 1.05-1.14x behind): 1/2 beats 1/1 at 4 threads
  (1.5 MiB each: 1e14 -5.1%, 4/4) and 2 threads (3 MiB: -4.6% / -3.5% at 1e13 / 1e14, 4/4), 1/4 is
  behind 1/2 everywhere. So **1/2 from 1.5 MiB of L3 per active thread**, inside the sparse regime only.
  The dev PC at 6 threads (2 MiB each) now gets it: 1e14 -2.1% / -3.5% (4/4 both), 1e15 noise.
- Refuted the same evening: `-DERA_MED_BANDS=1` (+4..+13% on three Emerald Rapids hosts, +9.0% on the
  i5-13500 at 1e14; an A/B knob only) and the 128 KiB segment on the i5-13500 (+6.5% / +5.4%).
- The tower's dense-regime loss is not the ISA (`-march=ivybridge` on the dev PC: +1.7% / +2.4%), the
  cutoff (1/4 at 1e12: +1.1%, a wash), the segment or the sub-block (16 KiB: +3.7%, 6/6 worse); the one
  knob that moved it is `med64=1/6` (-3.8%, 6/6). **Kept 2026-10-04: med64 default 1/12 -> 1/6**: the
  Cascade Lake -0.8% / -2.8% (overlapping), a tie on the i5-13500, the dev PC's full 1e12 count 23.51 /
  23.93 s vs 24.17 / 25.41 s; 1/4 on the tower (-4.3%) shows the gain saturating at 1/6.

### Activation at the top of N: the integer division is not the cost (tried, reverted, 2026-10-04)

`--debug-idle` prints the activation cost per base prime: dev PC, 1e18 tail (50.8M primes), 2 threads,
12.6 ns per prime, 0.64 s per thread (19% of a 1e10 window, 2% of a 1e11 one). Replacing the `ceil(start
/ p)` integer division with a double division and a +-1 fixup made it **16.6 ns (+32%)**, counts
identical. On Rocket Lake the divider is already fast and the ~40 cycles per prime are the push into a
random ring slot (a cache miss per prime), which primesieve pays too. Reverted; the check on old cores (Nehalem,
Ivy Bridge) followed the same day.

### Activation at the top of N on old cores: 83 ns per prime on Nehalem, two flags to split it (open, 2026-10-04)

The i7-620M (Nehalem) measured **83.0 ns per prime at 2 threads and 128.7 at 4** at the 1e18 tail,
against 11.6-12.6 on the dev PC. Two default-off flags split the suspects, counts identical with each:
`-DERA_FPDIV=1` (double division with exact fixup in `file_sparse`; dev PC 16.2 vs 11.6 ns) and
`-DERA_ACT_BATCH=1` (pushes staged and flushed grouped by `slot >> 6`; later rewritten as one 512-entry
buffer per group of 64 slots, dev PC 13.4 vs 11.5 ns).

- FPDIV: 0.0 / -0.1 / +0.4% on three Xeon @2.10GHz VMs, -0.4% on the Mac, +1.2% on the i5-3470 (20.0
  ns per prime at 2 threads, 24.8 at 4), all overlapping. **The division is closed everywhere.**
- ACT_BATCH: +0.6..+1.6% on the VMs (+8.2%, 3/3, on a 1e10 window: a fixed ~6 ns per prime), +3.1% on
  the Mac, +0.3% on the i5-3470; the per-group version **+7.7% (3/3)** at the i5-13500's 1e18 tail, where
  activation weighs the most (18.6 ns per prime at 20 threads vs 7.6 at 6, ~12% of the tail). Closed.
- `-DERA_BLK_COLOR=8` (cache-set aliasing of the ring's tail lines, starting each block 0-7 lines past
  its header): dev PC -2.7% on a 1e10 window but +1.9% on 1e11, i5-13500 **+6.3%**, i5-3470 +3.1%,
  Cascade Lake -1.4%. Refuted: the aliasing confines the ring's write stream to 1 of 64 L1 sets and 8
  of 512 L2 sets during the sieve phase, a free cache partition; only the activation would gain.
- **Resolved: it is the clock.** `perf` on the i7-620M: 53.8% in `activate`, 0.3% kernel, 94 cycles per
  prime, and 1.16 GHz measured (the battery-less MacBook's SMC caps the CPU). Every Mac number is a
  1.2 GHz Nehalem, primesieve's too, so the ratios stand. The three flags stayed as off knobs until the
2026-10-07 cleanup removed them.

### Sparse activation from the bitmap index: 18% fewer instructions per prime (kept, 2026-10-05)

Callgrind (1e17 tail, 1e8 window, one thread, 17M sparse primes) put `activate` at 1.50 G Ir, ~88
instructions per prime, with `WHEEL_POS[p % 30]` and `(p / 30) << 36` (153M each), the mask rebuilt per
prime and `p * p >= high_n` (89M) avoidable. `file_sparse` now takes the bitmap index `k` and derives
`p / 30` (`k >> 3`), the residue class (`k & 7`) and `p` from it, and the square test became one `isqrt`
per segment bounding the walk (`k_cut`): **1.50 G -> 1.23 G Ir (-18.2%)**, `make test` 87/87. It shows on
every fresh worker start and every steal at the top of N. What remains per prime is the 64-bit `div`,
`m / 2310` and `m % 2310`, `p * m / 30`, two table loads and the push; the base-prime sieve's `comp[k] =
1` (31% of that window) is the next cut if the 1e17-1e18 startup ever matters.

- i5-13500 (20 threads, x7, 1e10 windows): cycles:u -3.9% (7/7) at 1e14 up to **-7.3% (7/7) at 1e18**,
  instructions -3.0..-11.6%.
- i5-3470 (x5): instructions -2.9..-6.5% but cycles **+0.8..+15.5%** (0/5 at every N), wall 3.43 ->
  3.92 s at 1e18; memory events unchanged, 34% of `activate`'s samples on the instruction after the
  `div`. Split into `ERA_ACT_KCUT` and `ERA_ACT_IDX` (eff26f6): the isqrt bound is fine (+0.1 / +0.6%),
  the index derivation is the whole loss (turning it off: -7.1% / -13.1%, 3/3). Mechanism unexplained.
- **Kept gated:** `ERA_ACT_IDX` = 1 only with `__BMI2__` (Haswell and newer), `ERA_ACT_KCUT` on
  everywhere; the tower is back to a tie (+0.5% / +0.3% cycles). Cascade Lake (2026-10-07), which keeps
  the slow divider, still gains (-3.12% / -4.05% at 1e17 / 1e18, 3/3), and Emerald Rapids -2.78% /
  -1.17% at 1e18 (3/3): the gate stays, and a slow divider alone does not explain Ivy Bridge.

### Activation: sparse primes with no multiple up to N aren't filed (kept, 2026-10-07)

A narrow window high up cost ~1.28 s on one thread for the last 1e4 below 1e18 (247 primes) against
0.22 s for primesieve: ~0.8 s for the base primes up to 1e9 (next entry) and ~0.47 s filing all 50.8M
of them into the ring, though only ~8K have a multiple in the window. `file_sparse` already computes
the first multiple p*m in the range; past N (`SparseTier::set_range_end`: N of the whole run, not the
chunk's end, since a worker carries its ring into the contiguous chunks after it) the prime can never
hit a segment and isn't filed -- primesieve's EratBig drops them the same way. Measured on the
pre-cleanup code (dev PC, two runs each, counts equal): 1e4 window below 1e18 1.27 -> 1.00 s on one
thread, ~0.6 -> 0.34 s on 12; 1e8 window 0.73-0.94 -> 0.47-0.58 s on 12; wide windows cycles:u ABBA
at 12 threads: last 1e11 below 1e18 -2.3% (instructions -5.2%: the chunks near N and the stolen pieces
stop filing primes whose next multiple is past it), last 1e10 +0.7% (overlapping).

### Base primes sieved into the wheel bitmap with the main sieve's kernels (kept, 2026-10-07)

`sieve_base_primes` was a byte-per-odd-number sieve (no wheel) emitting every prime through a
`wheel_index` division and an atomic OR into the BasePrimes bitmap: ~0.8 s on one thread up to 1e9,
~4 s up to 2^32. It now sieves the bitmap itself in 32 KiB windows of wheel indices: the pre-sieve
fill (multiples of 7..163, the primes left alone by `self_k`), then `erat::cross_off_class<PR>` for
167..isqrt(limit) from each square up, each word stored inverted; parts of whole words in parallel, no
atomics; the pre-sieve is built before the base primes. Base-prime counts equal primesieve's for isqrt
of 7, 49, 50, 121, 168, 169, 961, 27889, 28224 (the pre-sieve's edge), 1e6, 1e11, 1e12, 1e15, 1e18 and
the 64-bit ceiling (203,280,220). Both entries together on top of the cleanup (0f87f98), last 1e4
below N, dev PC, two runs each: 1e18 1.23-1.39 -> 0.29 s on one thread (primesieve 0.219 s),
0.59-0.62 -> 0.215 s on 12 (0.219 s); the ceiling 5.76-5.99 -> 1.51-1.55 s on one (0.963 s), where
the rest is the activation's walk over 203M base primes. Last 1e11 below 1e18, cycles:u ABBA at 12
threads: -0.4% (overlapping). `make test` 115/115.

## Segment and sub-block sizing

`plan_sieve` (src/tuning.hpp) derives every width from sysfs. The base segment is half of
cpu0's L2 (the smallest per-CPU L2 share on a hybrid), capped at 32 x L1d; doubled once a
base prime would be sparse, under `max(16 x L1d, min(32 x L1d, L2 per thread))`; without a
sparse tier and with one thread per core, the whole L2 share (half of it with <= 40K base
primes). Then the power-of-2 fixup and a narrow segment for the chunks below narrow^2. The
sub-block is half the largest L1d, all of it with one thread per core.

### `SUB_BLOCK_BYTES`: per-thread vs. machine-wide sizing (kept, uniform-with-margin wins)

Sizing each thread for the CPU it runs on (`sched_getcpu()` + a per-CPU table) was slower
on the i5-13500 (2026-09, ~28-30s vs ~26-27s at 1e12) than one machine-wide value from the
smallest domain; a buggy version with an extra /2 was fastest (~25-26s). A smaller,
safely-under-budget size, the same for every thread, wins on contended hardware. Kept.

### Cache-topology sizing: per-CPU-minimum step (kept)

The cache detection reads cpu0 only, so with a P-core cpu0 the E-core threads were sized
for cache they don't have. Now every CPU's fair L2 share is read (`CpuCacheTopology`) and,
if one is smaller than cpu0's, the smallest is used for every thread, uniformly. Only on
genuine heterogeneity (a uniform machine would get the /2 margin twice); skipped when
`-s`, `--l2-bytes` or `--l1-bytes` forces a value. **L1d switched to the maximum
(2026-09-27):** the minimum gave the i5-13500 the E-cores' 16KiB sub-block; the P-cores'
24KiB was -2.0% at 1e12 (20 threads, every rep), while also taking the P-core L2 lost
(+1.5%). Segment from the smallest L2 share, sub-block from the largest L1d.

### Auto segment width: dropping the `isqrt(limit)` cap (kept)

The auto width was capped at `isqrt(limit)` (every base prime dense). Once the small tier's
sub-block was decoupled from the segment, a narrower segment only paid the medium and
sparse tiers' per-segment costs more often: dropping the cap was -12.4% cycles at 1e11 and
-6.0% at 1e12 (i5-11400F), a no-op from ~1e13 up. The full L2 instead of L2/2 measured a
regression at 1e13 then, so the /2 stayed.

### `seg_k_width_from_l2_bytes`'s extra /2 margin, applied on top of an already-per-thread L2 share (kept, counterintuitive)

The per-CPU-minimum step (now in `plan_sieve`) applies this /2 to a share already divided
among the CPUs on that L2. It looked like double-counting and was briefly "fixed", but on
the i5-13500 the smaller segment was reliably faster; the fix was reversed.

### Chunk-width floor: at least 4 segments per chunk (kept, 2026-09-27)

`CHUNKS_PER_THREAD=150` made chunks narrower than a segment at small N (1e10, 12 threads:
1800 chunks of ~1.48M indices vs a ~2.1M-index segment), each paying the full setup.
Floored at `total_k / (4 * seg_k_width)`: dev PC 1e10 -1.4% cycles:u, 1e11 neutral.
Superseded: it became `--tune minsegs` (default 1 from 2026-10-02); since 2026-10-07
`MIN_SEGS_PER_CHUNK` is fixed at 1 and the knob is gone.

### Sub-block size: half the L1d, not all of it (kept, 2026-09-27)

The small tier's sub-block was the whole L1d (48KiB on the dev PC), chosen before med64.
At 1e12 a 24KiB sub-block alone was -3.9% cycles:u, with `small_limit` 6KiB -6.0% (shrinking
`small_limit` alone +1.3%): a full-L1d sub-block likely leaves no room for the tier's state
and the presieve window. Across N: 1e10 -14.9%, 1e11 -11.7%, 1e13 -3.0%, most of the small-N
overhead against primesieve. `sub_block_from_l1_bytes()` = L1d/2. Measured with SMT
siblings sharing each L1d; the whole-L1d entry below covers one thread per core.

### Segment width doubled once the sparse tier exists (kept, 2026-09-27)

At 1e14 the medium tier was 39% of cycles, bound by a fixed cost per prime per segment, so
a segment twice as wide pays it half as often. 512KiB vs auto 256KiB, dev PC: last 1% of
1e14 -16%, last 5% of 1e13 -7%, full 1e13 -2.5%, but full 1e12 +6.5% (no sparse prime, only
cache pressure with two hyperthreads per L2). Rule: when `isqrt(N) >= seg_k_width` and the
width is automatic, double it (the whole per-thread L2 share): 1e11/1e12 keep 256KiB, 5e12
and up get 512KiB. Clean 1e13 348.67s (primesieve 354.854s, 0.98x); i5-13500 1e14 tail
43.35s vs 45.01s. Bounded since by the segment ceiling and the 32 x L1d cap.

### Narrow segment for the chunks below narrow squared (kept, 2026-09-28)

With the width doubled, a chunk entirely below narrow^2 (narrow = the pre-doubling width)
has no active prime >= narrow, since activation is by p^2. It runs the non-sparse
configuration (narrow segment, 1/1 cutoff, `med64_limit` on the narrow width) through a
second `TierSet` picked per chunk; straddling chunks stay wide. Dev PC, full 1e13, ABBA (791
of 1800 chunks narrow): -1.8% wall and user (ceiling ~2.9%). The narrow share is
narrow^2/N (~44% at 1e13, 4.4% at 1e14), so it matters for 5e12..~3e13.

### Sub-block: the whole L1d when each thread has a core to itself (kept, 2026-10-01)

Half the L1d is an SMT sibling's share; one thread per core leaves half unused (primesieve's
EratSmall chunk is the full L1d). Found on the 2-vCPU Emerald Rapids sandbox (48KiB L1d, no
SMT): at `-t 2` 1e12 went 1.04x -> 0.97x. Dev PC `-t 1`: 48KiB with `small_limit` kept at
6KiB -9.4% (1e10) / -7.0% (1e11), but at `-t 12` +16.5%. i5-13500, 1e11 `-t 6`: one thread
per P-core -6.1%, two SMT threads on each of three P-cores +26.4%, unpinned -6.0% (Linux
spreads one per core, P-cores first). Rule: the whole L1d when `-t` <= the physical cores
with the largest L1d, else half; `small_limit` keeps the half-L1d value.

### Segment ceiling: half the L2 per thread, within 16-32 x L1d (kept, 2026-10-02)

The doubling filled the whole L2 share, 2 MiB on the Emerald Rapids sandbox (48 KiB L1d,
2 MiB L2 per vCPU), where 1 MiB or 512 KiB was 3-11% faster on the 1e15..1e18 tails.
primesieve caps at 16 x L1d and below the L2 per thread (api.cpp's `get_sieve_size`); 16 x
L1d alone cost +6% at 1e13. First rule: `max(16 x L1d, min(32 x L1d, L2 per thread / 2))`.
**2026-10-03, the `/ 2` dropped:** on the Xeon @ 2.80GHz (32 KiB L1d, 1 MiB L2) 1 MiB beat
auto 512 KiB at every tail 1e14-1e18 (-0.7..-4.6%), and on Emerald Rapids the two were never
distinguishable (an explicit 1.5 MiB `-s` was silently rounded to 1 MiB by the power-of-2
fixup; the startup log says so now). Ceiling now `max(16 x L1d, min(32 x L1d, L2 per
thread))`, sparse regime only; validated on the Xeon 2.80 at -2.7% (1e14) / -2.2% (1e18).

### `MIN_SEGS_PER_CHUNK` as `--tune minsegs` (knob added, default kept, 2026-10-02)

The i5-13500's 1e10 loss (1.09x) is tail balance: 6.2% idle, E-cores 3x slower than P-cores
at that N, ~12 ms of idle, about the whole gap. The 4-segment floor (~9 ms on an E-core)
sets the tail granularity, and its reason went with the sieve carried across chunks. Dev
PC 1e10, `minsegs=1`: idle 2.9-5.0% -> 1.8-2.8%, wall unchanged. Made 1 the same day on the
server's runs (i5-13500 Topdown entry); a fixed 1, not a knob, since 2026-10-07.

### Whole-L2 base segment: one thread per core, no sparse tier (kept, 2026-10-03)

Half the L2 share is there for an HT sibling. On the 2-vCPU Xeon @ 2.80GHz (1 MiB L2 per
vCPU, no SMT, then 1.00-1.16x on the tails) a 1 MiB base instead of 512 KiB was -8..-13% at
the 1e13 tail on two hosts. Rule: no sparse tier and no more threads than physical cores
with the largest L1d (`one_per_core`): the base is the whole L2 share, within 32 x L1d. SMT
machines are unchanged; the dev PC at `-t 2` with a forced 512 KiB base was neutral.
Validation: Xeon 2.80 -10.5%, 1e13 tail 1.10x -> 1.02x; Emerald Rapids (1.5 MiB) -5% vs
1 MiB on the clean host (3/3). Refined on 2026-10-06 for few base primes.

### Base segment capped at 32 x L1d: a VM whose sysfs reports the host's L3 as L2 (kept, 2026-10-03)

A 2010 MacBook Pro (i7 M620, 32 KiB L1d, 256 KiB L2) under Docker Desktop, whose VM's sysfs
showed the host's 4 MiB L3 as a private L2 per vCPU: the CLI picked a 2 MiB base and ran
the last 1e10 below 1e13 at 2.14x primesieve (17.06 s vs 7.987 s); 512 KiB gave 0.96x. Kept: the auto
base width is capped at 32 x L1d before the sparse regime is evaluated (`-s` bypasses it,
the startup log says when it applied); every real machine measured sits at or under it.
On that VM it gives 1 MiB (1.07x), not the best 512 KiB: no sysfs-fed rule tells it from
the Xeon 2.80 (real 1 MiB L2, 1 MiB best) -- the case for a startup calibration. The same
laptop native picked 256 KiB itself: 0.96-0.97x, tails 0.95-1.07x.

### Half the whole-L2 width with few base primes, on every one-per-core machine (kept, 2026-10-06)

The i5-3470 rule of 2026-10-04 (half the L2 while the base primes are <= 40K, gated on L2
<= 256 KiB) was not about the small L2. `make benchmark-ab` x3, 1e11: Emerald Rapids 768
KiB (half its 1.5 MiB auto) -3.3% (3/3), 1 MiB -1.7%; the Xeon @ 2.80GHz a tie. Rule: one
thread per core, no sparse tier, base primes <= 40K: the base is `min(L2 share, 32 x L1d) /
2`, on any L2. `--l2-bytes` stands in for the share here, so test.sh forces both branches.

## Threads, scheduling and whole-run profiles

`run_parallel_chunks` (src/main.cpp) splits the range into threads x 150 chunks of at least
one segment (a range too small runs on fewer threads; `--start` tails get up to 32 per
thread). Each worker walks a contiguous run, `sieve_chunk` carrying its `SegmentSieve` (a
`thread_local` per tier set) from chunk to chunk; a worker whose run is empty steals the
back of the run that would finish last, sized from measured rates and activation cost.

### dTLB pressure at large N: investigated, ruled out (2026-09-25, external review, Opus 5.5)

Hypothesis: a thread's working set (segment, ~1.2MB of medium-tier state, sparse ring blocks)
with two threads per core sharing one STLB blows the dTLB. Dev PC `perf stat` at 1e13:
0.0043% of loads miss, and even at 20-30 cycles each the ~267M misses are ~0.03-0.04% of
the cycles. Ruled out.

### `run_parallel_chunks`: chunk-granularity idle-time investigation (2026-09-25, external review, Opus 5.5)

Hypothesis: `CHUNKS_PER_THREAD=16` too coarse at large N. `ERATOSTENES_DEBUG_IDLE`: 1.4% /
0.9% idle at 1e12 / 1e13 on the dev PC, 2.1% at both on the i5-13500 (P/E cores): flat in N,
far too small for the ~12% gap of the time, rejected as its explanation. 16 -> 150 still
took idle from 2.1% to 0.2%; the first wall comparisons ran hot after hours of load, but
measured cold (`make run`, isolated) 1e12 23.91 s and 1e13 325.93 s were never worse than
before. **Kept at 150.** Measure cold: later reps of a `REPS` loop run slower.

### `sieve_chunk`: one `SegmentSieve` per worker instead of per chunk (tried, reverted -- neutral on cycles:u, 2026-09-26)

Reusing one `SegmentSieve` per worker instead of one per chunk: dev PC 1e12, cycles:u +0.3%
(noise), the -1.75% wall was drift; glibc's dynamic mmap threshold likely absorbed the
allocation. Reverted. **Follow-up (2026-10-01): kept.** Construction had grown (med64's lists,
sparse arenas): now a `thread_local` cache of up to two instances keyed by `TierSet`, 1e10
`-t 12` -1.6% cycles:u, last 0.1% of 1e13 -2.9%, with instructions down too.

### Top-of-range tails: startup costs that grow with sqrt(N) (kept, 2026-10-01)

The tails lost more the higher the window (dev PC 0.83x at 1e15 to 1.76x at 1e18, server
0.95x to 1.92x): pi(sqrt N) grows to 50.8M at 1e18, where the base-prime sieve took 7.8 s on
one thread and the workers spent most of 13.2 s activating sparse primes eight times per
thread. Changes: `sieve_base_primes` segmented (32 KiB windows; sqrt(1e18) 7.8 s -> 0.86
s), fewer chunks per thread on tails (replaced the same day by the next entry), `classify`
skipping the narrow tier set past narrow^2. Dev PC: 1e18 37.01 -> 20.60 s (1.76x -> 0.96x).

### `run_parallel_chunks`: contiguous runs, the sieve carried across chunks, steals (kept, 2026-10-01)

One chunk per thread left 12.6% idle at the 1e18 tail (dev PC), 10.9-13.5% on the
i5-13500, and a shared counter made every chunk pay a full activation. Now:

- each worker walks a contiguous run of chunks; an idle one takes the back half of the run
  with most chunks left if it spans >= 4 wheel indices per base prime (`STEAL_MIN_K`);
- `sieve_chunk` remembers where its `SegmentSieve` stopped and a chunk starting there skips
  `begin_chunk()`; every chunk but the last is a whole number of segments;
- `sieve_base_primes` runs on all threads, and the sparse tier is a `std::span` over
  `base_primes`.

Dev PC, ABBA: 1e18 tail idle 12.6% -> 2.0-4.1%, wall -4.4%; 1e15 tail -6.5%; full 1e11
-3.8% (no re-activating ~27k primes in ~1800 chunks), 1e12 tie. Also the largest N became
2^64 - 2^32 * 16 (`p * m` reaches start + 14p in activation).

### Tails up to the 64-bit ceiling at 6 threads, checked against primecount (2026-10-07)

Last 1e11 below N, dev PC `-t 6` (WSL capped at 11 GB), 2 interleaved pairs, peak RSS from
`VmHWM`, on the doubled-ring build of the same day before the cleanup:

| N | eratostenes | primesieve | ratio | peak RSS era / ps | primes |
|---|---:|---:|---:|---:|---:|
| 1e18 | 10.46 s | 12.28 s | 0.85x | 2.36 / 2.36 GiB | 2,412,705,071 |
| 2e18 | 11.21 s | 13.46 s | 0.83x | 3.27 / 3.23 GiB | 2,373,074,469 |
| 4e18 | 12.29 s | 14.99 s | 0.82x | 4.53 / 4.51 GiB | 2,334,614,943 |
| 8e18 | 13.46 s | 16.88 s | 0.80x | 6.30 / 6.21 GiB | 2,297,425,776 |
| 1e19 | 13.87 s | 17.60 s | 0.79x | 7.01 / 6.85 GiB | 2,285,738,870 |
| 1.6e19 | 15.19 s | 19.53 s | 0.78x | 8.75 / 8.29 GiB | 2,261,486,119 |
| 18446744004990074879 | 16.29 s | 20.32 s | 0.80x | 9.37 / 8.76 GiB | 2,254,186,542 |

Every count equals primesieve's and pi(N) - pi(N - 1e11) from primecount 7.16 (its own 128-bit
analytic method, no sieve): all seven, the ceiling window included. Memory is ~threads x 8 B x
pi(sqrt N) in both programs (every thread files every sparse prime into its own ring): pi(2^32) =
203,280,221, ~1.6 GB per thread at the ceiling, ~32 GB with 20 threads.

### Threads vs tail height: DRAM bandwidth caps the sparse tier near 2^64; a memory budget (kept, 2026-10-08)

The server's ceiling tail (last 1e11 below 2^64 - 2^32 * 16) took 12.85 s on 3 threads and 11.94 s on
20, primesieve 14.46 and 13.48 s: 6.7x the threads for 7%, at 6.7x the memory. Curves at that tail,
wall time, peak RSS (`VmHWM`) and `--debug-idle`'s activation cost:

| threads | i5-11400F (6C/12T) | activation | i5-13500 (6P+8E/20T) | peak RSS | activation | primesieve 13500 |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 47.07 s | 9.8 ns | 34.04 s | 1.68 GiB | 5.5 ns | 36.81 s |
| 4 | 17.18 s | 11.0 ns | 10.73 s | 6.29 GiB | 6.0 ns | 12.96 s |
| 6 | **14.73 s** | 13.1 ns | **9.17 s** | 9.38 GiB | 7.1 ns | **10.93 s** |
| 12 | 18.91 s | 32.2 ns | 11.84 s | 18.06 GiB | 12.2 ns | 12.67 s |
| 20 | -- | -- | 12.29 s | 29.61 GiB | 17.9 ns | 13.48 s |

Both machines, both programs, fastest at 6 threads -- the core count of one and the P-core count of
the other, and two DDR channels on both. Past it the sparse tier's ring (1.5 GiB per thread) is
served from DRAM at the bandwidth's floor, and every extra thread only adds its activation (203M
primes, each a random write into its ring), which itself slows as the threads share the bandwidth
(5.5 -> 17.9 ns per prime). 14 threads, one per physical core, was 11.72 s: the limit is the
memory, not the cores. How high that regime starts, i5-13500, last 1e11 below N, mean of 2:

| N | 6 threads | 10 | 14 | 20 | best |
|---|---:|---:|---:|---:|---:|
| 1e14 | 4.16 s | 4.12 s | 3.95 s | **3.71 s** | 20 |
| 1e15 | 4.83 s | 4.67 s | 4.57 s | **4.33 s** | 20 |
| 1e16 | 5.35 s | 5.15 s | 5.25 s | **4.97 s** | 20 |
| 1e17 | 6.08 s | 5.92 s | 5.96 s | **5.80 s** | 20 |
| 1e18 | **6.91 s** | 7.11 s | 7.46 s | 7.27 s | 6 |
| ceiling | **9.17 s** | 11.02 s | 11.72 s | 13.15 s | 6 |

The crossing is between 1e17 and 1e18; below it every thread helps. No thread rule is taken from
this: the knee is a property of the memory system, not of anything sysfs reports (`l1_big_cores`
would say 6 on both machines by accident, and 2 on the i5-1235U).

Finding it at run time was tried and reverted (2026-10-08): `run_parallel_chunks` started the
workers in steps (every 4th, every 2nd, all; on from 50M base primes), a later step's workers on a
2-chunk probe off the back of the longest run, and kept a step only if the sum of the running
workers' per-chunk rates rose >= 10%. The first version left a one-chunk run with no running owner
unstolen (wrong counts) and handed late starters full runs (34 activation-priced steals); the
second counted right and decided right on the dev PC at the ceiling tail (3 -> 6 workers +68%, 6 ->
12 +8%, parked) but lost anyway: 21.3 / 20.0 s against 16.5 / 16.1 s with 12 threads and 15.5 /
15.8 s with 6; at the 1e18 tail 11.7 s against 10.8 and 10.4-11.3 s. Every step's probe must
activate pi(sqrt N) primes before it can be measured (2-3 s each at the ceiling, in a 15 s run),
so the measuring costs about what it saves, and leaves the runs unbalanced (finish times 12.4 s to
20.2 s). It would only pay on windows far wider than 1e11. Not pursued either: timing the memory
system itself at startup (random writes from 1, 2, 4... threads) as a proxy for the knee.

What went in is the safety half: `cap_threads_by_memory` (tuning.hpp) estimates a worker at ~8 B x
pi(sqrt N) x 1.02 + 16 MiB and the shared part at the base-prime bitmap + 64 MiB (ceiling: 9.6 GiB
estimated vs 9.24-9.38 measured on 6 threads, 18.9 vs 17.99 on 12) and runs fewer threads when
they wouldn't fit 90% of `MemAvailable`, or `--max-mem` (0 = no limit), before `plan_sieve` so the
per-thread choices see them; the startup log says so. Dev PC: the ceiling tail with `-t 6 --max-mem
8g` ran on 4 threads, 17.41 s (6 threads: 15.84 s, 12: 17.72 s), counts equal. `make test` 122/122.

### `-t 2` gap vs primesieve at the 1e15 tail: profile and sparse cutoff by thread count (measured, not adopted, 2026-10-01)

With one thread per core the gap is flat, 1.19-1.27x from 1e14 to 1e18 (dev PC): 158.8G vs
131.5G cycles with fewer instructions, IPC 1.70 vs 2.10 -- stalls, not work. Our dense tiers
reach p < 4.19M, where primesieve sends everything above ~0.8M to EratBig. Sparse cutoff
1/2: -6.7% at `-t 2` but +8.0% at 6 threads and +13.6% at 12 -- a shared resource (L3 or
memory bandwidth). No thread-count rule fit (an L3-per-active-thread gate came 2026-10-03).

### `run_parallel_chunks`: steals priced with the run's own measurements (kept, 2026-10-02)

The fixed threshold let only 4 steals through on the i5-13500 at the 1e18 tail, 7-9% idle.
`sieve_chunk` now times each fresh start's activation and each chunk's sieving, and the
thief takes the back piece, in whole chunks, that has it and the victim finish together:
activation + piece / its rate = (left - piece) / victim's rate, the victim being the run
that would take longest; no steal when one chunk doesn't pay its activation. i5-13500: 10
steals, idle 3.1%, wall unmoved (each steal re-activates 50.8M primes, ~0.75 s); dev PC:
same decisions. Kept: no loss anywhere, and it adapts by itself.

### `-t 2` gap with the VMs' cutoff (sparse 1/2): L2 misses in med64, slicing med64 per segment (tried, reverted, 2026-10-02)

At 1/2 the 1e15 tail is 1.135x with fewer instructions and mispredicts than primesieve but
2.75x its L2 misses, ~56% in med64 (one mark in three misses in a 512 KiB segment the size of
the L2). Slicing med64 per segment (`--tune med64_parts`) cut the misses (7.9-8.1G -> 5.0G
at N=4) but cost cycles (`-t 2` 158.0G -> 162.7G, `-t 12` +22%): they were overlapped.
Reverted.

### `-t 2` gap: Topdown from raw slot counters (2026-10-02)

From the raw slot counters (WSL breaks `-M TopdownL1`), dev PC, 1e15 tail, cutoff 1/1: 20%
more slots than primesieve while retiring fewer uops; 37% of the gap bad speculation, 63%
nothing issued. primesieve's tier split (256 KiB segment) was +5..+10%; clang 19.1.7 vs GCC
a tie. Nothing adopted.

### i5-13500 Topdown at `-t 2`: the residual gap is bad speculation; `minsegs` 1; medium bands as an option (2026-10-02)

Native Topdown L1, two P-cores, 1e15 tail: back-end even, bad speculation +32.8G slots,
~90% of the gap (the dev PC's WSL estimate said 37%: the cores differ). Predicated medium
bands re-added behind `-DERA_MED_BANDS=1` (dev PC: branch misses 0.866G -> 0.509G, cycles
+5.6%), off by default (removed 2026-10-07). `minsegs=1` on the i5-13500, 1e10, 20 runs: median 0.185 s vs 0.195 s, default 1.

### Where the losses are after the sparse-tier changes (2026-10-02, end of day)

Tails 1e13..1e18 on 51523dc: i5-11400F 0.73-0.87x, i5-13500 0.94-1.02x, Emerald Rapids
0.96-1.04x, Xeon @ 2.80GHz 1.00-1.16x. The Xeon 2.80 loses even at 1e13/1e14, where the
sparse tier barely runs, so its dense tiers lose (as on the dev PC at `-t 2`, 1.19x at
1e14). The whole-L2 base rule took its 1e13 tail to 1.02x the next day.

### Startup: the dev PC's "7-8 ms overhead" was exec from /mnt/c (2026-10-02)

`./eratostenes 1` takes 7-9 ms from /mnt/c (WSL's 9p mount) and 1 ms from ext4; our own
startup is ~2 ms. On the i5-13500 it is 1-4 ms vs primesieve's 5-25 ms, so its 1e10 loss
isn't startup. Small-N timings on the dev PC should run the binary from /home.

### `--start`: the primes in the rounded-down head of the first word were counted (bug, fixed 2026-10-04)

`split_ranges` rounds `--start` down to a multiple of 64 wheel indices and nothing removed
the primes in that head (up to ~240 numbers); `--start 19999900000000000` counted 3 extra.
Fix: `SieveConfig::skip_below_k`, whose indices `sieve_and_emit` marks composite before
extraction; tested on that 2e16 tail against primecount.

### Two power regimes on every machine: burst and sustained (open, 2026-10-04)

The i5-13500 gave 21.37 s at 1e12 from idle and 22.83 s in the next round; the i5-1235U
laptop ran the 1e13 tail 7.20 / 7.97 / 10.19 s back to back. It's the turbo budget, not
temperature: PL2 for the tau window (~28 s by default), then PL1; ~6% apart on the server,
up to 40% on the 15 W laptop. Decision (user): the benchmarks ignore it -- interleaved pairs
in alternating order, REPS times, means converging on the sustained speed; rows from
different regimes are not compared. Open: the server's short PL1 window (1e11 1.49 s from
idle vs 1.88-1.89 s in a REPS=5 round), RAPL readout pending.

### `--start`: the start itself was dropped when its wheel index was 63 mod 64 (bug, fixed 2026-10-05)

`split_ranges` rounded `wheel_count_upto(start)`, the index *above* start, so a start at
index 63 mod 64 landed one word past it: 239, 2399 and 4799 counted one short at 1e6, and
`240 --start 239` segfaulted on an empty `ranges`. Fix: `start - 1` in `split_ranges` and
`first_k`, an explicit empty-`ranges` exit, tests.

### `ByteCounter`: digit count without `to_chars` (tried, tie, reverted, 2026-10-05)

A cached decade instead of a full `std::to_chars` per prime in the text output's counting
pass: byte-identical, a tie (laptop, 1e9, 1 thread: 0.56 vs 0.57 s). The cost is elsewhere:
`emit_values` plus the sink is ~7 ns per prime (0.56 s vs 0.20 s count-only); profiling it
needs `noinline` under `-flto`. Reverted.

### Text output: byte counts from the prime count where a chunk's numbers share a digit count (kept, 2026-10-09)

The counting pass doesn't need the primes, only their digits: where a chunk's first and last
numbers have the same digit count, its bytes are its prime count x (digits + 1), so it runs
with `NullSink` (a popcount per word); only the chunks across a power of 10 decode their primes
with `ByteCounter`. For both sinks to share the thread's `SegmentSieve` (and carry it across
chunks), the per-thread cache moved out of the `sieve_chunk<Writer>` template into
`sieve_slot`. Output byte-identical (1e9, 1e10, a range across 1e10, `--start` tails). Dev PC,
1e10 to tmpfs, 12 threads, mean of 5: counting pass **0.911 s -> 0.202 s** (count-only takes
0.18 s). The write pass (5.0 s on tmpfs) is kernel time and stays.

## Wheel and stepping tables

The bitmap uses a mod-30 wheel (`wheel.hpp`). Small and med64 primes step with `erat_small.hpp`'s
constant offsets, medium ones with `wheel210_big.hpp`'s packed mod-210 table (`PACK210`), sparse
ones with its mod-2310 `TABLE2310`; `wheel_delta_at` only builds the presieve tables.

### Wheel size: mod 6 vs. mod 30 vs. mod 210 (historical, pre-tiered-marking architecture)

Early version (one per-prime jump table, before the presieve and the tier split), i5-11400F,
count-only: mod 210 won at 1e11 (7.02s vs 8.01s mod 30) but lost badly at 1e12 (142.60s vs
89.54s) once its table (`phi(210)=48` entries per prime) outgrew L3, so mod 30 shipped. The
numbers are historical (today's 1e12 is ~3-4x faster); a re-run would need fresh measurement.

### `ONFLY_CORRECTION`/`GAP_K`: shared table replacing a per-prime `delta[]` (kept)

On-the-fly stepping `delta(p,j) = qp*GAP_K[j] + ONFLY_CORRECTION[pr][j]` from one shared,
L1-resident `WHEEL_SIZE` table instead of a per-prime `delta[]` that grows with the tier: ~3.25x
faster for primes forced through it at 1e11, dev PC (primesieve's `WheelElement` trick, re-derived
here). **Removed 2026-09-27**: no tier uses the mod-30 tables any more.

### `GAP_K210`/`ONFLY_CORRECTION210` table shape: chained-index vs. flat arrays

Two flat arrays indexed by `ri` (fixed per prime) and `w` (a loop counter). A chained "next" field
loaded from the previous lookup cut instructions:u 15.1% but cost cycles:u +2.3% at 1e12 (IPC 1.33
-> 1.10): each hit's load waits on the previous one, while `w` is register arithmetic. Reverted
before commit. Retried 2026-09-25 as one 8-byte `{gap,corr}` struct per (PR, w), `w` unchanged:
8-10% slower at 1e12 (33.55s/34.25s vs ~30.8-31.5s, wall only), cause not isolated. Reverted.

## Pre-sieve

`Presieve::fill()` starts every sub-block by OR-ing the periodic patterns of the primes up to 163
(16 groups, 4 tables per pass), from period-sized tables read in 4 KiB chunks with wraparound.

### Extending pre-sieve coverage past prime 163 (tried three ways, all reverted)

Each group costs one full-segment shift-and-OR pass however rarely its primes hit. Two badly sized
groups (167+173, 179+181) at 1e12: cycles +1.6-1.8%, wall flat. Four well-sized ones (each new prime
with a small partner, ~7-10 KB): cycles +4.3%, wall +3.7%: group count, not table size, is the
cost. One 17th group of all four (~935 MB table), i5-11400F: +1.4% wall at 1e12 (single rep),
streaming a table no cache holds. A win would need a per-group cost that scales with hits.

### `fill()`: skip the `self_k` correction loop when it can't possibly match (kept, 2026-09-26)

A ~30-35-entry loop fixing each pre-sieve prime's self-hit ran on every `fill()` call, though a
`self_k` (wheel index of a prime <= 163) can only match near the start of the range. Now skipped
when `k_low > max_self_k`. Dev PC, 1e12, 4 reps: instructions:u -0.0125% (deterministic), cycles:u
-0.055%, inside the ~0.3% noise. Kept anyway: provably correct, one field and one guard.

### Period-sized tables: fill in 4 KiB chunks with wraparound (kept, 2026-09-29)

Tables used to be unrolled `max_seg_k_width` bits past their period, each as big as the segment:
16 x ~512 KiB = ~8.3 MB on the i5-13500, 4 MB on the dev PC, streamed through L2 every segment and
evicting the segment and the med64/medium state (consistent with the server's LLC-loads at 14x
primesieve's). Now `fill()` works in `PRESIEVE_CHUNK_BYTES` = 4 KiB chunks, each table's offset
wrapping by its period between chunks: ~190 KB for all 16, as in primesieve. Dev PC, cycles:u: 1e13
10% tail -1.3%, 1e12 -0.8%; full 1e13 343.79s -> 336.33s (0.94x primesieve).

## Output: gap encoding and the .db format

`-o x.db` writes format 3: sieve threads take each prime's wheel index (`write_k`), encode gaps as
wheel-index deltas, compress 65536-prime blocks with zstd level 1 and `pwrite` them to a `.blk`
sidecar; SQLite (WAL, 4 KiB pages) keeps only the block index that `nth_prime` reads.

### Gap encoding: wheel-index deltas

**Kept, 2026-09-28 (format_version 2).** Version 1 stored `delta/2` in a byte, whose distribution
carries the mod-30 residue structure that zstd can't see. A gap counted in wheel indices is a
near-geometric, near-iid stream whose order-0 entropy is the iid-bitmap bound, and zstd's Huffman
stage gets within ~1% of it, so neither a compressed bitmap nor an arithmetic coder was pursued.
Dev PC, 1e12 end to end: 24.13 GB (5.13 bits/prime) -> 20.23 GB at zstd 1 (-16.2%, 4.30
bits/prime), time -31% (level 1 became the default); the same ~16% at 1e15 is ~3 TB of the planned
22 TB disk. Byte 1..255 is the gap, 0 escapes to a raw 4-byte delta (prev off the wheel, gaps over
255 wheel steps, none below 1e15). `nth_prime` refuses any other `format_version`.

### Write-pipeline knobs: `BATCH`, `--db-block-size`, `wal_autocheckpoint` (all measured, kept at their defaults)

With the single SQLite writer as critical path, interleaved A/B: `BATCH` 1000 beat 10000
(14.7-19.9s vs 24.2-28.1s); `--db-block-size` 65536 beat 16384 and 1048576 (16-21s vs 27-30s);
`wal_autocheckpoint=0` was more variable (18.8-30.1s vs 20.0-20.8s). Defaults kept: the limit was
the single writer itself, later removed by the `.blk` sidecar.

### Page size (kept)

`page_size` only applies to a new database, so a stale file at the target path is removed first: a
leftover 64 KB page wasted ~half a page per block in an early prototype. 4K re-confirmed 2026-10-02.

### `PRAGMA cache_size` increase (tried, reverted, 2026-09-25)

512 MB instead of SQLite's 2 MB, to stop re-fetching B-tree pages at large N: worse on the server
at 1e12 and 1e13. Reverted, cause not isolated.

### `--zstd-level` default: 1 (kept, 2026-09-28)

Level 3 was a guess; on v1 bytes level 1 was already 5-8% faster at the same size. With wheel-index
gaps it is faster and smaller (dev PC, 2 reps): 1e12 20.23 vs 20.94 GB, 218.1s vs 273.8s. Level
3's LZ search finds nothing in a near-iid stream and its block splitting costs ratio. Default 1.

### `.db` extraction in wheel indices: `GapBlockSink::write_k` (kept, 2026-10-01)

At 1e10 `-t 12`, 51% of cycles went to extraction + gap encoding, which built each value and then
divided it back into wheel indices to subtract them. `sieve_and_emit` now hands k to sinks with
`write_k`; the gap is `k - last_k_`, the value is rebuilt only for a block's first prime and
escapes; output byte-identical. Dev PC, 1e10 `-t 12`: instructions:u -26.7%, cycles:u -17.8%; wall
flat on the WSL disk (I/O-bound), on tmpfs -10% at `-t 12` and -20% at `-t 2`.

### `.db` encoder over the whole segment, its state in locals (kept, 2026-10-09)

The profile of a 1e11 `.db` on tmpfs (dev PC, 12 threads: 6.5 s against 2.56 s count-only) put
41% of cycles in the extraction + encoding loop and 15.6% in libzstd. Almost every hot
instruction of the loop was a stack access: `bits` (`blsr` on memory), `count_in_block_`,
`last_k_` and `last_on_wheel_` were loaded and stored on every prime. Each gap byte went out
through a `uint8_t` pointer, which may alias any member, so the sink's state could not stay in
registers. `GapBlockSink::write_segment` now takes the whole segment (the words, `k_low`, the
count) and walks its primes with the state in locals, written back once per segment or block;
the gaps go to a fixed buffer (`MAX_GAP_BYTES` per prime) through a pointer, no `push_back`.
`write_k` and `SegmentSieve::emit_indices` are gone. Same primes (1e9 `--slice` md5). Dev PC,
`benchmark_ab.sh` with `-o` on tmpfs, last 1e11 below 1e12, 6 interleaved pairs: cycles:u
**-12.8%**, instructions:u -12.0%, wall **-8.0%**, every B run below every A run.

Not adopted: one `ZSTD_CCtx` per sink (`ZSTD_compressCCtx`) instead of `ZSTD_compress`'s own per
block: 5.06 vs 5.11 s mean of 5, a tie. The zstd share is the compression itself.

### `journal_mode=OFF` for the bulk load (tried, reverted, 2026-10-01)

WAL writes every page twice. `journal_mode=OFF` + `synchronous=OFF` was 2-3x faster on the dev PC
at 1e11, but on the i5-13500 at 1e12 (ABBA, `sync` timed) it never won: +6.6% (WAL 59.85s, off
67.39s, off 87.89s, WAL 85.86s), plausibly because OFF writes pages in place, scattered by B-tree
splits. Reverted. Side finding: both modes got ~30% slower after ~40-60 GB written back to back
(SSD cache or writeback throttling); for 1e15 the sustained write speed is what counts.

### `blocks`/`block_data` table split (kept until format 3, 2026-10-02: the blocks left SQLite, see `.db` output: the `.blk` sidecar)

SQLite keeps a row in one B-tree cell, so the mutable metadata (`start_index`, fixed by
`fix_offsets`) lived apart from the blobs: an UPDATE over ~62K rows/2.6 GB of blobs took ~12-17s at
1e11. Superseded by format 3, where the blocks moved to the `.blk` sidecar.

### `.db` output: where the time goes (2026-10-02)

i5-13500, 1e12, 20 threads, NVMe RAID0: count 21.5s / 414s user, `.db` 77.4s / 825s user + 41s sys
(11.2 of 20 cores busy, 260 MB/s); dev PC 1e11: 4.4 of 12 cores busy. The single SQLite writer was
the limit through kernel time (WAL copies each page twice), not CPU (libsqlite3 2.45% of user
cycles); 36% of user CPU was extraction + gap encoding, ~30 cycles per prime. `page_size` 64K was
+78% size and slower, 16K +6% and no faster: 4K kept.

### `.db` output: the `.blk` sidecar, blocks written by the sieve threads (kept, format 3, 2026-10-02)

Compressed blocks go to a `.blk` file next to the `.db` (block_file.hpp): one `fetch_add` on a
shared offset places a block and the thread that compressed it `pwrite`s it (parallel, lock-free,
no gaps). The `.db` keeps one index row per block (start_index, count, start_prime, offset, len)
plus the sidecar's name and size, checked by `nth_prime`, which reads a block with one `pread`. Dev
PC, 1e11, 12 threads: tmpfs 6.1-7.2s -> 4.4-4.8s (-30%), WSL disk 18.9-26.3s -> 6.1-10.6s
(-60..-70%), ~10 of 12 cores busy. Open: ext4 caps a file at 16 TiB (1e15 is ~15.5 TiB), so the
sidecar needs sharding if that disk is ext4.

### `.db` format 4: ranges, and `nth_prime` as a query tool (kept, 2026-10-07)

`-o` works with `--start S`: a `.txt` of [S, N], or a `.db` whose positions count from the first
prime >= S (`meta.range_start`, format 4; format 3 reads as range_start 0) -- pi(S - 1) is unknown
to a partial sieve, so it isn't stored. `nth_prime` gained `--count X Y`, `--next X`, `--range X
Y`, `--slice I J` and `--info`. Lookup by value is a binary search over positions with the
`start_index` query (~20 probes), not an SQL index on `start_prime`, which SQLite orders as a
signed 64-bit integer (wrong above 2^63, where tails now go); counting a range decodes only its two
end blocks; blocks decode carrying the wheel index (`decode_block`, next to the encoder). Last 1e10
below 1e18 (241M primes, 148 MB `.blk`, dev PC): count the range 0.002 s vs 2.0 s to sieve it
again on 12 threads, `--next` 0.003 s, print it all 4.0 s on one thread vs 15.8 s for
`primesieve -p`. Not done: parallel block decode for `--range` / `--slice`.

## Compiler, build and PGO

The build is `-O3 -march=native -flto=auto`; the tuning comes from caches, threads and CPU flags,
never from a CPU model name. Since 2026-10-07 PGO (`make pgo`, the `eratostenes-pgo` Docker image) has been
removed from the build; the entries remain as the record of why.

### PGO overall: measured on the dev PC, not adopted on the production server

Dev PC, cycles:u, 1e10-1e13: ~2-4% fewer cycles. Production server (2026-09-25): 1e12 ~24.5-25s
vs 24.51s, 1e13 328.49s vs 330.38s (0.57%), inside the noise, with no perf there. Not adopted: an
unconfirmed gain against a longer build and a binary tied to its build machine.

### PGO training set: a natural 1e13 pass (tried, reverted, 2026-09-25, follow-up session)

To give the sparse tier real proportions: the instrumented build ran 45+ minutes on the dev PC
without finishing. Killed; not worth the build time.

### Compiler flags and PGO, re-asked with the A/B tooling (nothing, 2026-10-04 night)

`make benchmark-flags`, dev PC, 12 threads, 1e13: `-mprefer-vector-width=512` -0.3%,
`-funroll-loops` +0.6%, `-O2` +2.0%, `-fno-plt` -0.6%, `-march=x86-64-v3` -1.5%, all overlapping.
PGO: 1e15 tail -1.9% (4/4), full 1e12 23.27/23.34s vs 23.13/23.13s. Nothing to take.

### Cascade Lake: branches kept off 32-byte boundaries (JCC erratum), `-Wa,-mbranches-within-32B-boundaries` (measured, not adopted, 2026-10-07)

The Xeon @ 2.80GHz sandbox went from 1.00x to 1.06x at 1e13 on a tail where the changed
`process_big` never runs: its growth only shifted the hot dense kernels' alignment, and on a
JCC-erratum core a jump crossing or ending on a 32-byte boundary misses the uop cache. With the flag: -3.4% at
1e13, -2.2% at 1e14 (3/3, overlapping); the layout move cost ~2%, the rest was the host. **Not
adopted** (user): it needs a CPU-model rule; for a long Skylake-family run, `make variant
DEFS=-Wa,-mbranches-within-32B-boundaries`. On this core any change can move a hot kernel ~2-3%.

## Open threads

Where the time still goes, and what nothing measured so far has fixed:

- **med64 is 50-64% of the cycles at 1e13-1e14 on every machine**, bound by
  L1 misses, with no lever found
  ([i5-3470 profile](#i5-3470-profile-at-1e12-the-med64-tier-over-the-whole-l2-segment-is-59-of-the-cycles-open-2026-10-04)).
  Hypothesis to check with counters on the tower: it is L1<->L2 bandwidth
  (a fill plus a dirty victim, ~104 bytes per hit).
- **`process_big` at 12 threads is issue-bound**; the wrap mask is gone since,
  the spills are what is left
  ([entry](#sparse-tier-process_big-is-issue-bound-at-12-threads-the-rings-wrap-mask-and-the-spills-are-what-is-left-open-2026-10-05)).
- **Activation at the top of N on old cores**: why taking the independent
  work away from the 64-bit division costs Ivy Bridge ~35 cycles per prime;
  hence `ERA_ACT_IDX` only with BMI2
  ([entry](#activation-at-the-top-of-n-on-old-cores-83-ns-per-prime-on-nehalem-two-flags-to-split-it-open-2026-10-04)).
  Haswell..Skylake were never measured for that gate.
- **1e10-1e11 full counts on the 2-vCPU VMs** stay at 1.03-1.10x primesieve.
  `--tune small=1/2` helps Emerald Rapids by 3% and hurts the dev PC (Rocket
  Lake): per microarchitecture, so it stays a manual `--tune`.
- **Wall time has two power regimes** (burst and sustained) on every
  machine, which short A/B windows can confuse with a code effect
  ([entry](#two-power-regimes-on-every-machine-burst-and-sustained-open-2026-10-04)).
- **No AMD or ARM machine** has been measured.
