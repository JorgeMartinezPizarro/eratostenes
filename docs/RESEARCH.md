# Research log: tried, measured, reverted

This collects every optimization attempt this project has tried, benchmarked, and
either kept or reverted -- pulled out of the source files' own comments so those
files stay readable, while the actual measurements and reasoning behind each
decision stay somewhere. Organized by source file, in the order the ideas appear
there. Design rationale for the code as it stands today still lives next to the
code (see [ALGORITHM.md](ALGORITHM.md)); this file is the "what else was tried and
why it didn't stick" record.

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
  - [`cross_off_medium`: EratMedium-style 64-list restructuring](#cross_off_medium-eratmedium-style-64-list-restructuring)
  - [med64: mod-210 stepping, two variants (tried, both reverted, 2026-09-26, external review, Opus 5.5)](#med64-mod-210-stepping-two-variants-tried-both-reverted-2026-09-26-external-review-opus-55)
  - [Small tier: mod-210 stepping as 7 unrolled mod-30 copies (tried, reverted, 2026-09-27)](#small-tier-mod-210-stepping-as-7-unrolled-mod-30-copies-tried-reverted-2026-09-27)
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
- [sqlite_prime_store.hpp](#sqlite_prime_storehpp)
  - [Write-pipeline knobs: `BATCH`, `--db-block-size`, `wal_autocheckpoint` (all measured, kept at their defaults)](#write-pipeline-knobs-batch---db-block-size-wal_autocheckpoint-all-measured-kept-at-their-defaults)
  - [`PRAGMA cache_size` increase (tried, reverted, 2026-09-25)](#pragma-cache_size-increase-tried-reverted-2026-09-25)
  - [`blocks`/`block_data` table split (kept)](#blocksblock_data-table-split-kept)
  - [Page size (kept)](#page-size-kept)
- [main.cpp](#maincpp)
  - [`SUB_BLOCK_BYTES`: per-thread vs. machine-wide sizing (kept, uniform-with-margin wins)](#sub_block_bytes-per-thread-vs-machine-wide-sizing-kept-uniform-with-margin-wins)
  - [`sieve_chunk`: one `SegmentSieve` per worker instead of per chunk (tried, reverted -- neutral on cycles:u, 2026-09-26)](#sieve_chunk-one-segmentsieve-per-worker-instead-of-per-chunk-tried-reverted----neutral-on-cyclesu-2026-09-26)
  - [`run_parallel_chunks`: chunk-granularity idle-time investigation (2026-09-25, external review, Opus 5.5)](#run_parallel_chunks-chunk-granularity-idle-time-investigation-2026-09-25-external-review-opus-55)
  - [Chunk-width floor: at least 4 segments per chunk (kept, 2026-09-27)](#chunk-width-floor-at-least-4-segments-per-chunk-kept-2026-09-27)
  - [`small_limit` cutoff tuning](#small_limit-cutoff-tuning)
  - [Cache-topology sizing: per-CPU-minimum step (kept)](#cache-topology-sizing-per-cpu-minimum-step-kept)
  - [EratBig-style sparse tier: forcing a power-of-2 segment width, and `sparse_limit = seg_k_width/4` (all attempts reverted)](#eratbig-style-sparse-tier-forcing-a-power-of-2-segment-width-and-sparse_limit--seg_k_width4-all-attempts-reverted)
- [arg_parser.hpp](#arg_parserhpp)
  - [`--zstd-level` default: 1 measured faster than 3, not yet made the default (open)](#--zstd-level-default-1-measured-faster-than-3-not-yet-made-the-default-open)
  - [Sub-block size: half the L1d, not all of it (kept, 2026-09-27)](#sub-block-size-half-the-l1d-not-all-of-it-kept-2026-09-27)
  - [Auto segment width: dropping the `isqrt(limit)` cap (kept)](#auto-segment-width-dropping-the-isqrtlimit-cap-kept)
  - [`seg_k_width_from_l2_bytes`'s extra /2 margin, applied on top of an already-per-thread L2 share (kept, counterintuitive)](#seg_k_width_from_l2_bytess-extra-2-margin-applied-on-top-of-an-already-per-thread-l2-share-kept-counterintuitive)
- [presieve.hpp](#presievehpp)
  - [`fill()`: skip the `self_k` correction loop when it can't possibly match (kept, 2026-09-26)](#fill-skip-the-self_k-correction-loop-when-it-cant-possibly-match-kept-2026-09-26)
  - [Extending pre-sieve coverage past prime 163 (tried three ways, all reverted)](#extending-pre-sieve-coverage-past-prime-163-tried-three-ways-all-reverted)
- [Makefile](#makefile)
  - [PGO training set: a natural 1e13 pass (tried, reverted, 2026-09-25, follow-up session)](#pgo-training-set-a-natural-1e13-pass-tried-reverted-2026-09-25-follow-up-session)
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

**Verdict: both reverted, code removed** (not kept behind a flag -- this
tier's mod-210 stepping is a genuine dead end at the N this project targets,
not a pending tune). If revisited, the setup-cost-vs-few-hits mismatch is
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

## sqlite_prime_store.hpp

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

### `blocks`/`block_data` table split (kept)

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

## arg_parser.hpp

### `--zstd-level` default: 1 measured faster than 3, not yet made the default (open)

The default (3) was picked on general zstd knowledge -- entropy coding, which is
most of the achievable ratio on this near-random gap-byte stream, barely depends
on compression level, so higher levels mostly buy slower builds, not smaller
files. That reasoning was later checked directly: `--zstd-level 1` vs. the
default 3, interleaved x3 on the dev PC, measured ~5-8% faster (16.7-19.6s vs.
17.8-20.9s) with essentially identical output size (0.629 vs. 0.630
bytes/prime). A small, consistent, low-risk win -- **not yet made the default**,
just measured; worth doing since there's no real tradeoff at this ratio.

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

### PGO training set: a natural 1e13 pass (tried, reverted, 2026-09-25, follow-up session)

Considered adding a natural `1e13` training pass (no `-s` override) to also give
the sparse tier real-proportion data instead of just the forced-tiny-width passes
already in the training set. Doesn't scale: an instrumented (unoptimized,
atomic-counter) build running a full 1e13 count-only pass took 45+ minutes on the
dev PC with no sign of finishing, vs seconds for the forced-small-width runs --
the instrumented binary is far slower than release, and at 1e13 that cost becomes
prohibitive per `make pgo` invocation. Killed before completion; not worth the
build-time cost for one training pass among several.

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
