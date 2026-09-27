# How eratostenes works

This walks through the pipeline in the order data actually flows through it: base
primes → wheel numbering → parallel chunking → per-segment tiered marking → output.
Each section explains what the code does and why, at a level meant to make the
design decisions legible without duplicating the exact formulas and derivations --
those live as comments next to the code they justify (linked from each section
below), since that's where they need to stay correct. The measured numbers behind
every tried-and-reverted optimization attempt live separately, in
[RESEARCH.md](RESEARCH.md), so the code comments stay focused on the design as it
stands today.

## 1. Base primes (`base_sieve.hpp`)

Everything downstream needs the primes up to `sqrt(N)` -- these are the only ones
whose multiples can possibly need marking. `sqrt(N)` is always small even for huge
N (`sqrt(1e15) ≈ 31.6M`), so this step is a plain, non-segmented, odds-only bit
sieve: no wheel, no parallelism, nothing fancy. It runs once, in milliseconds, and
its output (a `std::vector<uint64_t>`) feeds every other step.

## 2. Wheel factorization (`wheel.hpp`, `wheel210_big.hpp`)

Every number the sieve ever looks at is a multiple of 2, 3, or 5, except for 8 out
of every 30 -- those 8 residues (1, 7, 11, 13, 17, 19, 23, 29 mod 30) are the only
ones that can possibly be prime past 5. Instead of representing all 30 numbers per
"row" and immediately crossing off the trivial 22, the sieve only ever represents
those 8: a **wheel index** `k` numbers them directly (`k=0` is the number 1, `k=1`
is 7, and so on), so the bit array is already 73% smaller before any prime-specific
marking starts. `wheel_number(k)` and `wheel_index(n)` convert between an index and
the actual integer it represents; `WHEEL_R`/`WHEEL_POS` are small precomputed
tables that make both conversions O(1).

The mod-30 layout means **one byte is exactly 30 consecutive integers** (bit j of
byte q is the number `30q + WHEEL_R[j]`). Everything fast in §6 is built on that:
a prime's residue class mod 30 fully determines which bit it hits in each byte and
by how many bytes it advances, so the marking masks are compile-time constants.
`wheel.hpp` still carries commented-out mod-6/210/2310 configurations from before
the tiered design, but they no longer compile: `erat_small.hpp` `static_assert`s
mod 30, since its constant masks only exist for that layout (`wheel.hpp`'s own
comment has the historical measurements).

The wheel that numbers the *bits* (mod 30) is separate from the wheel that steps a
prime's *multipliers*. Every marking prime is > 163, and the presieve (§3) always
covers 7, so a multiple `p*m` with `7 | m` is already marked. The medium and sparse
tiers therefore step `m` only through the 48 residues coprime to 210 instead of the
8 coprime to 30, skipping ~14% of hits. `wheel210_big.hpp` holds the tables for
that: `big::TABLE` (sparse tier: mask, byte step and next phase per (class,
phase)) and the same data packed one word per (class, phase) for the medium tier
(`PACK210[pr][w] = mask | dm << 8 | corr << 16`: hit = `s[pos] |= mask`, next hit
`pos += qp*dm + corr`, no division).
The small and med64 tiers stay on mod-30 multipliers: their unrolled loops depend
on the 8-hits-per-p-bytes cycle, and both mod-210 versions tried so far lost (see
[RESEARCH.md](RESEARCH.md#erat_smallhpp)).

## 3. Presieve (`presieve.hpp`)

The smallest base primes (7, 11, 13, ... up to 163) have many multiples in *every*
segment, so marking them one hit at a time is per-bit work no scheduling removes.
But because the wheel already turned "which numbers are candidates" into a
periodic pattern, and a small prime's own hit pattern is *also* periodic, the
product is periodic too: precompute it once, and filling a new segment becomes a
handful of bitwise ORs instead of a marking loop per prime. This fill *replaces*
zeroing the segment -- every tier after it ORs its marks on top.

One table covering many primes would have a period equal to the *product* of all of
them, so the 35 primes 7..163 are split into 16 small groups (primesieve's own
grouping, reused as-is: `{7,23,37}`, `{11,19,31}`, `{13,17,29}`, then pairs like
`{41,163}` ... `{97,101}`), each with a period of 8 × product ≈ 48K-80K bits. Each
table is stored unrolled one full segment width past its period, so the window for
any segment is one contiguous read. `fill()`:

- reads each table at `k_low mod period` -- always byte-aligned (segment starts are
  multiples of 64, periods multiples of 8), so each output word is a single
  unaligned 8-byte load, no shift-and-combine;
- combines tables 4 at a time, so `dst` is written 4 times, not 16 (the first
  group assigns, the rest OR) -- GCC vectorizes this;
- un-marks the presieve primes themselves (a table marks p as a multiple of p),
  only for the few segments at the very start of the range (`max_self_k` guard).

It runs per small-tier sub-block (§6), right before that sub-block is crossed off,
so the fill lands in L1. `presieve.hpp`'s own comment has the cost model (group
*count* drives the cost) and three measured dead ends extending coverage past 163.

## 4. Segmentation and parallel chunking (`main.cpp`)

The wheel-index range `[0, wheel_count_upto(N))` is far too big to hold as one bit
array for large N, so it's processed in small, fixed-size **segments**, reusing the
same array -- this is the classic segmented-sieve idea, and it's the reason base
primes only need O(1) state each between segments (§6) instead of O(range).

Above that, the whole range is split into many more **chunks** than there are
threads (`CHUNKS_PER_THREAD = 150`, `main.cpp`), pulled from a shared queue
rather than assigned one fixed chunk per thread. This matters because chunks are
not equal work: a chunk near the start of the range has far fewer *active* base
primes per segment (most base primes haven't reached their first multiple yet)
than a chunk near the end. Static one-chunk-per-thread assignment would leave
early-finishing threads idle while the last one grinds through the most
expensive part of the range; dynamic, fine-grained chunk stealing keeps every
thread busy until the work genuinely runs out. 150 (not the more obvious-looking
16) came out of an idle-time investigation on real target hardware -- see
[RESEARCH.md](RESEARCH.md#run_parallel_chunks-chunk-granularity-idle-time-investigation-2026-09-25-external-review-opus-55).
At small N the chunk count is capped so every chunk still spans at least 4
segments (`ERATOSTENES_MIN_SEGS_PER_CHUNK`): each chunk re-creates its
`SegmentSieve` and re-activates every base prime, and that setup would otherwise
dominate. See `run_parallel_chunks` and `split_ranges` in `main.cpp`.

Text output needs two passes over this same structure (count bytes, then write) so
that `pwrite()` can have every thread's exact, disjoint file offset known before any
byte is written, letting all threads write in parallel with no locking and no merge
step; `.db` output only needs one pass, since blocks flow through an async queue to
a single writer thread instead of a fixed file offset (see §8 and
`main.cpp`'s own top-of-file comment for why that distinction exists). Count-only
runs (no `-o`) are one pass and just `popcount` each finished word.

## 5. Base prime activation

A base prime only starts contributing hits once its square falls inside the range
being processed -- below that, it can't have a multiple there that isn't already
covered by a smaller prime. `SegmentSieve::sieve_and_emit` tracks, per tier, how far
into each sorted prime list it has already "activated," so across a whole chunk's
worth of segments, every prime is examined for activation exactly once, not once per
segment. Activation finds the first multiplier `m` with `p*m >= max(p², segment
start)` that is coprime to 30 (small, med64) or to 210 (medium, sparse), and packs
the prime's state into 8 bytes (`erat::DenseState`: `p/30`, residue class and
multiplier phase in one word, the pending hit's position in the other).

## 6. Four-tier marking within a segment (`segment_sieve.hpp`, `erat_small.hpp`)

Once a base prime is active, how expensive it is to mark depends entirely on how
often it hits within one segment -- a prime much smaller than the segment hits it
thousands of times; a prime close to the segment's own width hits it once, if at
all. One marking strategy can't be good at both ends, so base primes are split into
tiers by value (mirroring primesieve's own EratSmall/EratMedium/EratBig split, with
one extra tier -- med64 -- of this project's own). Per segment they run in this
order: small (with the presieve fill, sub-block by sub-block), med64, medium,
sparse, then extraction.

- **Small** (`p < small_limit`): the segment is processed one **sub-block** (half
  the L1d) at a time -- presieve fill, then every small prime crossed off inside
  that sub-block -- so the marks land in L1 instead of sweeping the whole
  (L2-sized) segment. The loop (`erat_small.hpp::cross_off<PR>`) is **unrolled**
  8 hits per cycle over the mod-30 byte layout: for a prime `p = 30*qp + R[PR]`,
  one multiplier cycle covers exactly `p` bytes and its 8 hits sit at fixed
  offsets `qp*(R[j]-1) + C[PR][j]` with fixed bit masks `M[PR][j]`, all
  compile-time constants once the residue class `PR` is a template parameter.
  That's under 2 instructions per hit (mostly `or byte [base+offset], mask`). A switch
  enters the cycle at the pending hit's phase and a checked chain leaves it at
  `end`. One list per residue class (`small_[8]`) keeps `PR` a template parameter
  instead of a per-prime branch. Pending hits are rebased once, at the segment's
  last sub-block.
- **med64** (`small_limit <= p < med64_limit`): the same byte-marking
  `cross_off<PR>`, but over the whole segment in one pass (these primes have
  too few hits per sub-block to amortize a call per sub-block). Primes are kept in
  64 lists keyed by (residue class, entry phase), double-buffered (`m64_cur_`/
  `m64_nxt_`): each segment reads one set and files every prime into the other by
  its new phase, so every call in one inner loop enters the unrolled cycle at the
  same phase and that entry switch is predictable. Applying the 64-list idea to
  the *whole* medium tier was tried twice and reverted both times -- the medium
  population keeps growing with N and the footprint of 64 lists eventually costs
  more than it saves; a band next to `small_limit` saturates early and doesn't.
  See [RESEARCH.md](RESEARCH.md#segment_sievehpp).
- **Medium** (`med64_limit <= p < seg_k_width`, a few hits per segment): a plain
  one-hit-per-iteration loop (`cross_off_medium<PR>`) on byte positions, stepping
  with the mod-210 table (§2), one table load per hit. The table holds two
  48-phase cycles so the phase `w` doesn't need wrapping on every hit (a medium
  prime has fewer than 48 hits per segment; `w` is folded back once per call).
  One list per residue class (`medium_[8]`) makes the class a template
  parameter, like the small tier. The unrolled loop, a 4-way interleave,
  prefetching and the 64-list layout were all measured slower here -- see
  [RESEARCH.md](RESEARCH.md#erat_smallhpp).
- **Sparse** (`p >= seg_k_width`, at most ~1 hit per segment): the only tier where
  a *bucket* earns its keep -- most segments have nothing to do for most of these
  primes, so each one is filed into the ring slot of the segment its next hit
  falls in, and a segment only touches the primes due in it (`process_big`,
  primesieve's EratBig design). Each ring slot is a linked list of 1 KiB,
  1 KiB-aligned blocks from a pool (a tail pointer landing on a block boundary
  means "full", so there's no count field); hits are byte marks stepped with
  `big::TABLE` (mod-210, §2); the slot is `byte position >> log2(segment bytes)`,
  so whenever any prime is sparse `main.cpp` floors the segment to a power of 2
  bytes. The next block of a chain is prefetched once per block, since pooled
  blocks are scattered in memory. [RESEARCH.md](RESEARCH.md#segment_sievehpp)
  has the history of the earlier designs.

Finally, **extraction**: invert each word (bit = 0 means prime) and either
`popcount` it (count-only) or walk its set bits with `ctz` to emit values.

The two cutoffs are tuned jointly (the lower bound of med64 *is* `small_limit`):
`small_limit = sub-block / 4` (L1d/8: 6144 on a 48 KiB L1d) and `med64_limit =
seg_k_width / 12`, re-confirmed after the sub-block moved to half the L1d. Both are
overridable via `ERATOSTENES_SMALL_NUM`/`_DEN` and `ERATOSTENES_MED64_NUM`/`_DEN`
for sweeps without recompiling; `ERATOSTENES_MED64_NUM=0` disables med64, giving
the three-tier layout back. See [RESEARCH.md](RESEARCH.md) for the sweeps.

### Where the time goes

Share of `cycles:u` per tier on the dev PC (i5-11400F, 12 threads, 256 KiB
segment, 24 KiB sub-block), from `perf record` on a `-g` build of the same code
(source-line attribution; LTO inlines most tiers into one function otherwise).
Measured 2026-09-27, just *before* the medium tier moved to byte positions (which
cut its cost by roughly 7-8%):

| N | small | med64 | medium | sparse | presieve | extraction |
|---|---:|---:|---:|---:|---:|---:|
| 1e11 | 39.6% | 46.1% | 6.0% | -- | 5.4% | 2.1% |
| 1e12 | 30.9% | 35.3% | 27.5% | -- | 4.4% | 1.6% |
| 1e13 | 21.6% | 25.4% | 45.4% | 2.9% | 3.0% | 1.2% |
| 1e14 (after) | 13.7% | 19.8% | 38.8% | 24.2% | 2.3% | 0.9% |

(The 1e14 row is with the byte-position medium tier. At that N medium's IPC drops
to 0.89 from 1.27 at 1e12 and it takes 45% of all L2 misses and 58% of branch
misses: the sparse tier's ~4 MiB per thread of in-flight bucket entries pushes the
segment and medium's state out of L2.)

What bounds each tier, as far as measured (IPC per hyperthread at 1e12: med64
0.80, small 0.91, medium 1.41 before the byte change, extraction 2.45): the small
tier's unrolled loop runs from the Loop Stream Detector at ~1 store/cycle, the L1
commit limit -- fewer stores alone doesn't help unless the loop stays that small.
med64 scatters its hits across the whole L2-sized segment, so most of its stores
miss L1 and hit L2 (it alone is ~77% of all L1 load misses at 1e11). Medium does
too, but with far fewer hits per prime it was the one tier bound by instruction
count, which is what the byte-position change went after. The medium tier's
population is capped by π(seg_k_width); past N ≈ seg_k_width² (~4.4e12 here) the
rest of the base primes go sparse.

## 7. Cache auto-tuning (`arg_parser.hpp`, `main.cpp`)

Two sizes are auto-tuned from the machine's real, detected cache sizes (read from
`/sys/devices/system/cpu/...` on Linux, with `--l1-bytes`/`--l2-bytes` overrides
for when detection can't be trusted) rather than a fixed guess:

- **Segment width**: half the detected L2, so the segment's own bit array stays
  cache-resident alongside a hyperthread sibling and the tiers' state. Past that
  width, some base primes fall into the sparse tier on purpose.
- **Small-tier sub-block**: half the detected L1d -- that tier's whole point (§6)
  is keeping its marks inside L1, and the other half leaves room for the tier's
  own per-prime state and the presieve reads alongside the sub-block.

On a hybrid CPU (P-cores and E-cores with different caches) both are still
applied **uniformly to every thread** -- threads migrate between core types at
runtime, so sizing per thread by where it started isn't reliable. The segment uses
the *smallest* per-CPU L2 share (L2 size / CPUs sharing it); the sub-block uses the
*largest* L1d. Both choices were measured on the i5-13500 (the P-core L1d wins by
2%, the P-core L2 loses by 1.5%). See
[RESEARCH.md](RESEARCH.md#cache-topology-sizing-per-cpu-minimum-step-kept) and the
entries on each half-size margin.

## 8. Output: text vs `.db`

Text output is one prime per line, written directly. `.db` output is a SQLite file
where primes are grouped into fixed-size blocks, each block **delta-encoded**
(storing gaps between consecutive primes instead of the primes themselves -- gaps
are small and fit in 1 byte almost always, with a rare 5-byte escape for larger
ones) and then zstd-compressed. Each block is a row carrying its own starting
position and prime count, indexed by position (`idx_blocks_start`) so `nth_prime`
can find and decompress just the one block a query needs, rather than scanning the
file -- lookups stay fast (milliseconds) regardless of how large the file gets.

Since SQLite only allows one writer at a time, all the CPU-heavy work (sieving,
delta-encoding, compressing) stays fully parallel across threads, and only the
already-compressed block hand-off to a single dedicated writer thread is serialized.
That writer thread inserts each block with a position that's initially only correct
*relative to its own chunk* (chunks finish out of order, since they're pulled from
the shared queue in §4) -- once every chunk's real prime count is known (a free
byproduct of the same sieve pass, no second pass needed), a handful of cheap
`UPDATE`s correct every block's position to its true, file-wide value. Block
metadata (position, count) and the compressed payload are two separate tables for
this specific reason: SQLite stores a whole row together, so correcting a small
integer column would otherwise force rewriting the compressed payload too. See
`sqlite_prime_store.hpp`'s own comment for the measured cost of getting that wrong.

## 9. Verification

`make test` (`scripts/test.sh`) checks pi(N) and primes by position against
[primecount](https://github.com/kimwalisch/primecount), an independent reference
implementation -- not hardcoded constants -- across several N and several
parameter combinations (threads, segment width, cache-size overrides, `.db` block
size, zstd level), plus checks `.db` output against plain text output position by
position. Raw sieve performance is checked separately against
[primesieve](https://github.com/kimwalisch/primesieve) (see the main
[README](../README.md#benchmarks)).
