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
whose multiples can possibly need marking. `sqrt(N)` is small next to N but not
tiny at the top of the range (`sqrt(1e18) = 1e9`, 50.8M primes), so this step is a
segmented, odds-only byte sieve in 32 KiB windows, split over the threads (each
takes a contiguous part of the range). Its output is a **bitmap on the wheel**
(`BasePrimes`): bit k set when `wheel_number(k)` (§2) is prime -- 33 MB at 1e18,
where a `std::vector<uint64_t>` of the same primes took 406 MB and every thread
read it all when activating (§5). A small rank index gives `pi(x)` for any x in a
few popcounts. The dense tiers (§6) take their few hundred thousand primes out of
the bitmap as lists; the sparse tier is just a run of the bitmap, walked bit by
bit. 1e9 takes 0.12 s with 12 threads (dev PC).

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
The layout is fixed at mod 30 (`wheel.hpp` `static_assert`s it); bigger wheels
for the bits were measured before the tiered design and lost (RESEARCH.md).

The wheel that numbers the *bits* (mod 30) is separate from the wheel that steps a
prime's *multipliers*. Every marking prime is > 163, and the presieve (§3) always
covers 7, so a multiple `p*m` with `7 | m` is already marked. The med64, medium
and sparse tiers therefore step `m` only through the 48 residues coprime to 210
instead of the 8 coprime to 30, skipping ~14% of hits. `wheel210_big.hpp` holds
the tables for that: `big::TABLE` (mask, byte step and next phase per (class,
phase); med64 reads it as compile-time constants, the sparse tier as one packed
word per row) and the same data packed one word per (class, phase) for the medium
tier (`PACK210[pr][w] = mask | dm << 8 | corr << 16`: hit = `s[pos] |= mask`, next
hit `pos += qp*dm + corr`, no division).
The sparse tier goes one step further, to the 480 residues coprime to 2310: the
presieve covers 11 as well, so multipliers divisible by 11 are redundant too
(~9.1% fewer sparse hits). `big::TABLE2310` holds its rows as one 32-bit word
(`mask | dm << 8 | corr << 16 | next << 20`, 15 KiB).
The small tier stays on mod-30 multipliers: its unrolled loop depends on the
8-hits-per-p-bytes cycle, and the mod-210 version tried lost (see
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
table is stored one 4 KiB fill chunk past its period (~190 KB for all 16), and
`fill()` covers `dst` chunk by chunk, advancing each table's offset and wrapping it
by its period in between -- primesieve's period-sized buffers, rather than tables
unrolled a whole segment past their period (~8 MB at a 512 KiB segment, see
docs/RESEARCH.md). `fill()`:

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
threads (`CHUNKS_PER_THREAD = 150`, `main.cpp`; 32 per thread on a `--start`
tail), each a whole number of segments. Chunks are not equal work: a chunk near
the start of the range has far fewer *active* base primes per segment (most base
primes haven't reached their first multiple yet) than a chunk near the end, and
on a hybrid CPU the cores aren't equal either. So each thread starts on its own
**contiguous run** of chunks and walks it in order, carrying its `SegmentSieve`
from one chunk into the next: the pending hit of every active prime is already
known, so no base prime is activated again (§5). A thread whose run is empty
**steals** the back of another run, in whole chunks, and pays one activation for
the stolen piece. The steal is priced with what the run itself has measured --
each thread's sieving rate and the time one activation takes -- so the thief
takes the piece that has it and the victim finish together, and nothing when a
piece wouldn't pay its activation; `--debug-idle` prints those measurements and
how far apart the threads finished. 150 chunks per thread came out of an
idle-time investigation on real target hardware (with the earlier shared-queue
design) -- see
[RESEARCH.md](RESEARCH.md#run_parallel_chunks-chunk-granularity-idle-time-investigation-2026-09-25-external-review-opus-55)
and
[RESEARCH.md](RESEARCH.md#run_parallel_chunks-contiguous-runs-the-sieve-carried-across-chunks-steals-kept-2026-10-01).
At small N the chunk count is capped so every chunk still spans at least 4
segments (`MIN_SEGS_PER_CHUNK`). See `run_parallel_chunks`, `sieve_chunk` and
`split_ranges` in `main.cpp`.

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
covered by a smaller prime. `SegmentSieve::activate` tracks, per tier, how far into
each sorted prime list (or, for the sparse tier, the base-prime bitmap) it has
already "activated," so across a thread's whole run of chunks (§4), every prime is
examined for activation exactly once, not once per segment. Activation finds the
first multiplier `m` with `p*m >= max(p², segment start)` that is coprime to 30
(small, med64), 210 (medium) or 2310 (sparse), and packs the prime's state into 8
bytes (`erat::DenseState`: `p/30`, residue class and multiplier phase in one word,
the pending hit's position in the other). Starting a run (or a stolen piece) at
the top of 1e18 activates all 50M base primes at once, ~1 s per thread, most of
it the kernel faulting in the bucket pool the entries go to (§6, sparse); that
fixed cost is what the contiguous runs in §4 avoid paying per chunk.

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
- **med64** (`small_limit <= p < med64_limit`): byte marking over the whole
  segment in one pass (these primes have too few hits per sub-block to amortize
  a call per sub-block), with primesieve EratMedium's loop shape
  (`cross_off_checked210<PR>`): one running index and one bounds check per hit,
  so each call leaves at a single loop exit instead of the small tier's
  unrolled-cycle exit plus tail exit. It steps on the mod-210 multiplier wheel
  (§2): the step from phase w to w+1 is `qp*dm + corr` with dm in {2,4,6,8,10},
  so five multiples of `qp` in registers plus per-case compile-time constants
  cover all 48 phases, with no per-call table (what sank the earlier mod-210
  attempts). Primes are kept in 384 lists keyed by (residue class, entry phase),
  double-buffered (`m64_cur_`/`m64_nxt_`): each segment reads one set and files
  every prime into the other by its new phase, so every call in one inner loop
  enters the cycle at the same phase and that entry switch is predictable.
  (The mod-30 version, `cross_off_checked<PR>` with 64 lists, was the default
  until 2026-09-30 and has been removed.) Applying the 64-list idea to
  the *whole* medium tier was tried twice and reverted both times -- the medium
  population keeps growing with N and the footprint of 64 lists eventually costs
  more than it saves; a band next to `small_limit` saturates early and doesn't.
  See [RESEARCH.md](RESEARCH.md#segment_sievehpp).
- **Medium** (`med64_limit <= p < seg_k_width`, a few hits per segment): a plain
  one-hit-per-iteration loop (`cross_off_medium<PR>`) on byte positions, stepping
  with the mod-210 table (§2), one table load per hit. The table holds two
  48-phase cycles so the phase `w` doesn't need wrapping on every hit (a medium
  prime has fewer than 48 hits per segment; `w` is folded back once per call).
  One list per residue class makes the class a template parameter, like the
  small tier. The state is two parallel arrays per class (struct of arrays):
  `(pos << 6) | w`, rewritten every segment, and `qp`, read-only -- so only half
  of it is ever dirty and written back. `qp` is stored as a 1-byte delta from
  the previous prime of the same class (the list is sorted by p; the largest
  gap below sqrt(1e15) is 52), so each prime streams 5 bytes per segment, not 8. Once that state outgrows the per-thread
  L3 share, it is also read with `prefetchnta` (once per prime), keeping it out
  of L2 so the segment stays there. The unrolled loop, a 4-way interleave,
  per-hit prefetching and the 64-list layout were all measured slower here --
  see [RESEARCH.md](RESEARCH.md#erat_smallhpp).
- **Sparse** (`p >= seg_k_width`, at most ~1 hit per segment; once the tier
  exists anyway the cutoff drops to `seg_k_width / 2` from 512 KiB of L2 per
  thread and to `seg_k_width / 4` from 1 MiB, or from 4 MiB of L3 per
  *active* thread (the L3 divided by the threads that run; 1/2 from 1.5 MiB),
  the 1/4 also below the sparse regime when an octave of base primes would
  land in the tier -- the bucket ring beats the medium tier's per-call cost for primes
  with fewer than ~4 hits per segment whenever the active threads have the
  bandwidth for its traffic: -10% at 1 and 2 threads on a 12 MiB L3, +13% at
  12; `--tune sparse=a/b` overrides, the startup log prints the choice): the
  only tier where
  a *bucket* earns its keep -- most segments have nothing to do for most of these
  primes, so each one is filed into the ring slot of the segment its next hit
  falls in, and a segment only touches the primes due in it (`process_big`,
  primesieve's EratBig design). Each ring slot is a linked list of 4 KiB,
  4 KiB-aligned blocks from a pool (a tail pointer landing on a block boundary
  means "full", so there's no count field; 4 KiB rather than the earlier 1 KiB
  is -4..-8% on the 1e15-1e18 tails, see RESEARCH.md); hits are byte marks stepped with
  `big::TABLE2310` (mod-2310, §2; each entry one 64-bit word, class/phase index |
  byte position << 12 | qp << 36); the slot is the current one plus
  `byte position >> log2(segment bytes)`, so whenever any prime is sparse
  `tuning.hpp` floors the segment to a power of 2 bytes. The ring holds twice
  the slots it needs and the cursor is reset by shifting the upper half down
  once it reaches the midpoint (`wrap_ring`), so the hot loop has no wrap mask.
  Pooled blocks are scattered in memory, so while a block is processed the
  next block of its chain is prefetched a line at a time, spread over the
  current block's groups of 4 entries; the segment byte each entry marks is
  prefetched 16 entries ahead (`ERA_BIG_PF`), and entries go two per
  iteration. [RESEARCH.md](RESEARCH.md#segment_sievehpp) has the history of
  the earlier designs.

Finally, **extraction**: invert each word (bit = 0 means prime) and either
`popcount` it (count-only) or walk its set bits with `ctz` to emit values.

The two cutoffs are tuned jointly (the lower bound of med64 *is* `small_limit`):
`small_limit = sub-block / 4` (L1d/8: 6144 on a 48 KiB L1d) and `med64_limit =
seg_k_width / 6` (1/12 until 2026-10-04: the wider band is -3.8% on a 256 KiB
segment and neutral on 512 KiB and 1 MiB ones). Both are
overridable via `--tune small=a/b` and `--tune med64=a/b` for sweeps without
recompiling; `--tune med64=0` disables med64, giving the three-tier layout back. See [RESEARCH.md](RESEARCH.md) for the sweeps.

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

## 7. Cache auto-tuning (`cpu_cache.hpp`, `tuning.hpp`)

Two sizes are auto-tuned from the machine's real, detected cache sizes (read from
`/sys/devices/system/cpu/...` on Linux by `cpu_cache.hpp`, with `--l1-bytes`/`--l2-bytes`
overrides for when detection can't be trusted) rather than a fixed guess:

- **Segment width**: half the detected L2, so the segment's own bit array stays
  cache-resident alongside a hyperthread sibling and the tiers' state. Past that
  width, some base primes fall into the sparse tier on purpose. Once any would
  (`isqrt(N) >= seg_k_width`), the width is doubled to the whole L2 share: from
  there on, the medium tier's fixed cost per prime per segment outweighs the
  extra cache pressure (-2.5% at 1e13, -10..-16% at 1e14 on the dev PC; +6.5% at
  1e12, where there's no sparse tier, which is why it's conditional). Even
  then, chunks entirely below narrow² (narrow = the width before doubling)
  can't have a sparse prime active yet, so they keep the narrow segment and
  the tier split that goes with it (-1.8% at 1e13). In the sparse regime the
  automatic width has a **ceiling**: the L2 per thread, but never under 16 x
  L1d (primesieve's own cap) nor over 32 x L1d, rounded down to a power of 2
  (the bucket tier's requirement). On a CPU with a large L2 per thread (2 MiB
  on a 2-vCPU Xeon) the doubling otherwise fills the whole L2, 4-9% slower on
  the top-of-range tails, and the ceiling lands on 1 MiB; on a 1 MiB-L2 Xeon
  the ceiling is the whole L2, 2-5% faster than half of it; the dev PC and
  the i5-13500 (256 KiB per thread under HT) are below it and unchanged. Whatever
  sysfs claims, the automatic base width never exceeds 32 x L1d: Docker
  Desktop's VM reports the host's L3 as a private L2 per vCPU and chose a
  2 MiB segment on a 256 KiB-L2 core, 2.14x primesieve. Below the
  sparse regime, when each thread has a core to itself (no SMT, or `-t` at
  or below the core count), the base segment is the **whole** L2 share (within
  32 x L1d) rather than half: the half is the hyperthread sibling's share, and
  with every base prime dense the medium tier's per-segment cost dominates
  (-8..-13% at 1e13 on a 2-vCPU Xeon with 1 MiB L2 per vCPU). The startup log
  says when either rule applies.
- **Small-tier sub-block**: half the detected L1d -- that tier's whole point (§6)
  is keeping its marks inside L1, and the other half is the hyperthread sibling's
  share. When no more threads run than physical cores with the largest L1d
  (VMs without SMT, `-t` below the core count), each thread has the L1d to
  itself and the sub-block takes all of it; the small/med64 cutoff stays at the
  half-L1d value either way.

On a hybrid CPU (P-cores and E-cores with different caches) both are still
applied **uniformly to every thread** -- threads migrate between core types at
runtime, so sizing per thread by where it started isn't reliable. The segment uses
the *smallest* per-CPU L2 share (L2 size / CPUs sharing it); the sub-block uses the
*largest* L1d. Both choices were measured on the i5-13500 (the P-core L1d wins by
2%, the P-core L2 loses by 1.5%). See
[RESEARCH.md](RESEARCH.md#cache-topology-sizing-per-cpu-minimum-step-kept) and the
entries on each half-size margin.

## 8. Output: text vs `.db`

Text output is one prime per line, written directly. `.db` output is two files:
`out.blk`, the primes grouped into fixed-size blocks, each block **delta-encoded**
(storing gaps between consecutive primes instead of the primes themselves) and then
zstd-compressed, one block after another; and `out.db`, a SQLite index with one
small row per block (its starting position and prime count, its first prime, and
its offset and length in the `.blk`). The gap is counted in **wheel indices** (§2),
not integers: one byte = how many mod-30 candidates the next prime is ahead, with a
rare 5-byte escape for the primes off the wheel (2, 3, 5) and gaps over 255
candidates. Counted that way the gaps are close to independent and geometric, which
zstd's Huffman stage codes within ~1% of their entropy; counted in integers (format
1) they carry the residue-class structure zstd can't see, ~16-18% more bits per
prime (see `gap_encoding.hpp`). The index is keyed by position
(`idx_blocks_start`), so `nth_prime` finds the one block a query needs, `pread`s
just its bytes from the `.blk` and decompresses that block -- a few index pages,
one contiguous read, one zstd frame, whatever the file's size.

The blocks never pass through SQLite: each sieve thread writes its finished block
into the `.blk` itself, at an offset handed out by one atomic counter
(`block_file.hpp`) -- any number of threads append at once, no lock, no gaps, no
merge step; blocks land in completion order and the index says where each one is.
Only the ~40-byte index rows go through a queue to a single writer thread, since
SQLite allows one writer at a time. Until format 3 the compressed blocks were BLOBs
in the `.db`, and that single writer -- in the kernel, copying every page twice
through the WAL -- capped `.db` output at ~260 MB/s on an NVMe RAID0 while most
cores waited (see RESEARCH.md). The writer inserts each row with a position that's
initially only correct *relative to its own chunk* (chunks finish out of order:
every thread works on its own run of them at once, §4) -- once every chunk's real
prime count is known (a free byproduct of the same sieve pass), a handful of cheap
`UPDATE`s correct every block's position to its true, file-wide value. The `.db`
records the `.blk`'s name and size, which `nth_prime` checks before reading.

## 9. Verification

`make test` (`scripts/test.sh`) checks pi(N) and primes by position against
[primecount](https://github.com/kimwalisch/primecount), an independent reference
implementation -- not hardcoded constants -- across several N and several
parameter combinations (threads, segment width, cache-size overrides, `.db` block
size, zstd level), plus checks `.db` output against plain text output position by
position. Raw sieve performance is checked separately against
[primesieve](https://github.com/kimwalisch/primesieve) (see
[BENCHMARK.md](BENCHMARK.md)).
