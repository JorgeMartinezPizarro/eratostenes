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

## 2. Wheel factorization (`wheel.hpp`)

Every number the sieve ever looks at is a multiple of 2, 3, or 5, except for 8 out
of every 30 -- those 8 residues (1, 7, 11, 13, 17, 19, 23, 29 mod 30) are the only
ones that can possibly be prime past 5. Instead of representing all 30 numbers per
"row" and immediately crossing off the trivial 22, the sieve only ever represents
those 8: a **wheel index** `k` numbers them directly (`k=0` is the number 1, `k=1`
is 7, and so on), so the bit array is already 73% smaller before any prime-specific
marking starts. `wheel_number(k)` and `wheel_index(n)` convert between an index and
the actual integer it represents; `WHEEL_R`/`WHEEL_POS`/`WHEEL_GAP` are small
precomputed tables that make both conversions O(1).

The wheel's modulus (30, i.e. primes {2,3,5}) is a compile-time constant, not a
runtime option -- bigger wheels (210, 2310) exist in the code, commented out. The
tradeoff is real: each extra wheel prime removes more trivial candidates, but the
per-prime state needed for the two dense marking tiers (§6) only stays a handful of
compact, compile-time-constant lookup tables *because* one byte maps to exactly 30
consecutive integers. Push the wheel past mod 30 and that byte-alignment breaks, so
the fast tiers would need their own, larger derivation -- measured not to pay for
itself at the sizes this project targets (see `wheel.hpp`'s own comment).

The other thing this file provides is `ONFLY_CORRECTION`: a small, shared table
(indexed by a prime's residue class mod 30 and its current phase within the wheel's
8-position cycle) that lets the medium and sparse tiers (§6) step a prime forward to
its next hit with one multiply, one table lookup, and one add -- no runtime
division, and no per-prime table that would grow with how many primes use it. The
derivation (why that table has exactly this shape) is in `wheel.hpp`'s own comment,
next to `make_onfly_correction`.

## 3. Presieve (`presieve.hpp`)

The smallest base primes (7, 11, 13, ...) have a multiple in *every* segment --
their period is far shorter than a segment, so there's never a segment where they
can be skipped. Marking them one hit at a time, every segment, is real per-bit work
that no amount of scheduling cleverness removes. But because the wheel already
turned "which numbers are candidates" into a periodic pattern, and a small prime's
own hit pattern is *also* periodic, the product is periodic too: precompute it once,
and filling a new segment becomes a handful of bitwise ORs (one shifted copy per
precomputed table) instead of a marking loop per prime.

The catch is that one table covering many primes has a period equal to the
*product* of all of them, so it grows explosively -- a handful of primes already
means a multi-megabyte table. The fix (borrowed from primesieve, whose own grouping
this project reuses as-is) is several small, independent tables instead of one big
one, each covering just 2-3 primes chosen so every table's own period stays around
6,000-10,000 (roughly 1KB), combined with OR at fill time. `presieve.hpp`'s own
comment has the full grouping and the measured cost model behind it (group *count*
drives the per-segment cost more than table size does, up to a point) -- including
two dead ends (extending coverage past prime 163) that measured as regressions on
this project's dev hardware, kept there so they aren't retried blind.

## 4. Segmentation and parallel chunking (`main.cpp`)

The wheel-index range `[0, wheel_count_upto(N))` is far too big to hold as one bit
array for large N, so it's processed in small, fixed-size **segments**, reusing the
same array -- this is the classic segmented-sieve idea, and it's the reason base
primes only need O(1) state each between segments (§6) instead of O(range).

Above that, the whole range is split into many more **chunks** than there are
threads (16 per thread), pulled from a shared queue rather than assigned one
fixed chunk per thread. This matters because chunks are not equal work: a chunk
near the start of the range has far fewer *active* base primes per segment (most
base primes haven't reached their first multiple yet) than a chunk near the end.
Static one-chunk-per-thread assignment would leave early-finishing threads idle
while the last one grinds through the most expensive part of the range; dynamic,
fine-grained chunk stealing keeps every thread busy until the work genuinely runs
out. See `run_parallel_chunks` in `main.cpp`.

Text output needs two passes over this same structure (count bytes, then write) so
that `pwrite()` can have every thread's exact, disjoint file offset known before any
byte is written, letting all threads write in parallel with no locking and no merge
step; `.db` output only needs one pass, since blocks flow through an async queue to
a single writer thread instead of a fixed file offset (see §8 and
`main.cpp`'s own top-of-file comment for why that distinction exists).

## 5. Base prime activation

A base prime only starts contributing hits once its square falls inside the range
being processed -- below that, it can't have a multiple there that isn't already
covered by a smaller prime. `SegmentSieve::sieve_and_emit` tracks, per tier, how far
into each sorted prime list it has already "activated," so across a whole chunk's
worth of segments, every prime is examined for activation exactly once, not once per
segment.

## 6. Three-tier marking within a segment (`segment_sieve.hpp`, `erat_small.hpp`)

Once a base prime is active, how expensive it is to mark depends entirely on how
often it hits within one segment -- a prime much smaller than the segment hits it
dozens of times; a prime close to the segment's own width hits it once, if at all.
One marking strategy can't be good at both ends, so base primes are split into three
tiers by expected hit count (mirroring primesieve's own EratSmall/EratMedium/
EratBig split):

- **Small** (`p < L1d/2` bytes, roughly the smallest 80%+ of all marks): crossed off
  one L1-sized sub-block of the segment at a time, so the marks land in L1 instead
  of sweeping the whole (L2-sized) segment. This tier uses an **unrolled**,
  byte-addressed loop (`erat_small.hpp`) whose per-hit bit masks and byte offsets
  are *compile-time constants* -- a direct consequence of the mod-30 wheel meaning
  one byte is exactly 30 numbers (§2): a prime's residue class mod 30 fully
  determines which of the 8 bit positions in a byte it can ever hit and by how much
  the byte index advances each 8-hit cycle, so none of that needs computing at
  runtime. One list per residue class keeps that dispatch a compile-time template
  parameter rather than a per-prime branch.
- **Medium** (up to the segment width, a few hits per segment): a plain
  one-hit-per-iteration loop using `ONFLY_CORRECTION` (§2) to step to the next hit.
  The small tier's unrolled loop was measured *slower* here -- with only a few hits
  to amortize its entry/exit cost over, the unpredictable jump in and out of the
  unrolled cycle costs more than the unrolling saves. A later attempt to
  hand-interleave four medium primes' independent stepping chains (hoping to hide
  one prime's branch-misprediction latency behind the others' ready work) was tried
  and measured as a clear regression too -- see [RESEARCH.md](RESEARCH.md) for the
  numbers and why it didn't pay off.
- **Sparse** (at least the segment width, at most ~1 hit per segment): the only tier
  where a *bucket* actually earns its keep -- most segments have nothing to do for
  most of these primes, so each one is scheduled into whichever future segment its
  next hit actually falls in (a fixed-size ring of pooled block queues), and a given
  segment's processing only ever touches the few sparse primes genuinely due that
  segment. [RESEARCH.md](RESEARCH.md) has the history of several prior internal
  designs for this tier (an idx-indexed intrusive list, an AoS relayout, a couple of
  division-free stepping variants) and why the current one (fixed-size pooled
  blocks per ring slot, primesieve's own EratBig
  design) won.

The boundary between small and medium is itself tuned, not guessed: it's set so a
prime counts as "small" once it has roughly 16+ hits per L1 sub-block, the point
past which the unrolled loop's fixed entry/exit cost is reliably amortized on the
hardware this was measured on (see `main.cpp`'s comment where `small_limit` is
computed).

## 7. Cache auto-tuning (`arg_parser.hpp`)

Two sizes are auto-tuned from the machine's real, detected L1d/L2 cache size
(read from `/sys/devices/system/cpu/...` on Linux, with `--l1-bytes`/`--l2-bytes`
overrides for when detection can't be trusted, e.g. in a container) rather than a
fixed guess:

- **Segment width**: capped at half the detected L2, so the segment's own bit array
  stays cache-resident even while sharing L2 with a hyperthread sibling. Smaller
  than that cap is better (removing the cap and using the full L2 was measured
  *slower*, not faster -- see `arg_parser.hpp`'s comment), so the auto default is
  the smallest width that still keeps every base prime out of the costlier sparse
  tier, unless that would exceed the cap, in which case some primes fall into the
  sparse tier on purpose.
- **Small-tier sub-block**: the detected L1d size, since that tier's whole point
  (§6) is keeping its marks inside L1 rather than sweeping the L2-sized segment.

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
