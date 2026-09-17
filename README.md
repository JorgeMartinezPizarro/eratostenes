# eratostenes

A segmented, parallel, bit-packed Sieve of Eratosthenes for generating (or
just counting) very large prime lists efficiently. Tested up to 10^12;
counting-only mode has been pushed further.

Two ideas do most of the work:

- **Wheel factorization**, fixed at compile time (`WHEEL_PRIMES` in
  `src/wheel.hpp`), skips multiples of a small set of primes before the
  sieve ever looks at them.
- **Bucket sieve** marks each base prime's multiples only in the segments
  where it actually has one, instead of checking every active prime on
  every segment.

Both choices -- and why they're not more "obviously" tuned (bigger wheel,
runtime-configurable wheel, etc.) -- are explained below and measured in
detail in [BENCHMARK.md](BENCHMARK.md).

## Build

Requires a C++20 compiler and POSIX `pwrite`/`ftruncate` (Linux or WSL;
does not build as-is with MSVC/native Windows).

```
make            # release build: -O3 -march=native -flto
make portable   # no -march=native, for a binary you'll copy to another machine
make debug      # ASan/UBSan, for debugging
make test       # runs test.sh: checks pi(N) against known values for N=1e8..1e11
```

If you're working on Windows with the project under `/mnt/c/...`, build and
run **inside WSL**, pointing output at a native Linux directory (e.g.
`~/...`), not `/mnt/c/...`: that mount goes through 9p and is much slower
for heavy I/O. In testing, writing to `/mnt/c` was the actual bottleneck,
not the CPU.

## Docker

```
make docker                                              # build the image
make run ARGS="--limit 100b -o /output/primes.txt -t 12" # run it
```

`make run` mounts `./output` (host) at `/output` (container) and creates
the directory if needed; always use `-o /output/<file>` in `ARGS` so the
result lands somewhere accessible outside the container. With no `ARGS`,
it runs `--help`.

## Usage

```
./eratostenes --limit N [options]

  -n, --limit N          Upper bound (inclusive). Accepts suffixes:
                          k=1e3  m=1e6  b=1e9 (short-scale billion)  t=1e12
                          e.g. 100b = 10^11
  -o, --output PATH      Output file (default: primes.txt)
  -t, --threads N        Thread count (default: available cores)
  -s, --segment-width N  Numeric width per segment (default: 4194304)
  -c, --count-only       Only count primes, skip writing the file
  -h, --help             Help
```

Which primes get skipped up front (the wheel) is fixed at compile time in
`src/wheel.hpp` (`WHEEL_PRIMES`) -- see "Wheel factorization" below before
touching it: more primes isn't always faster, it depends on N and your
CPU's L3 size.

Examples:

```
./eratostenes -n 1000000 -o primes_1M.txt
./eratostenes -n 100b -o ~/primes_100b.txt -t 12
./eratostenes -n 100b -t 12 --count-only
```

`--count-only` skips the write pass entirely (no file is created or
resized, nothing is converted to text): it only runs the count pass, using
a no-op sink instead of the byte-counter, so it doesn't even pay for
formatting each prime as text. Use it to get pi(N) or to measure the raw
sieve's speed without disk I/O in the way -- essential from around 10^11
up, where the equivalent text file already weighs tens of GB.

## Design

**Wheel factorization, fixed at compile time.** `WHEEL_PRIMES` in
`src/wheel.hpp` picks which primes are skipped up front (edit that line and
`make` to change it); the rest of the wheel (modulus, residues, position
table) is derived from it, also at compile time via `constexpr`. Numbers
that survive the wheel are numbered with a continuous "wheel index"
k=0,1,2,..., and each segment is a bit array (`uint64_t` words) where bit i
corresponds to `wheel_number(k_low + i)`. The wheel's own primes fall
outside that numbering and are emitted directly (special-cased on thread
0). Marking a base prime's multiples without dividing in the hot loop needs
a precomputed jump table per prime (`compute_wheel_deltas`); every prime's
table lives in **one flat, contiguous buffer** (a `std::array` inside
`WheelBasePrime`, not a `vector` per prime) to keep the access pattern
sequential, stored as `uint32_t` (not `uint64_t`) to halve its cache
footprint.

Bigger wheels remove more candidates, but the cost and benefit scale very
differently: adding prime p multiplies that jump table by `(p-1)` but only
cuts the marking work by `(p-1)/p` -- a bad trade for large p once the
table stops fitting L3 and the bottleneck shifts from CPU to memory
bandwidth. There's a real interior optimum, not a monotonic tradeoff, and
it depends on both N and the machine's cache size. A `--wheel` CLI flag
(runtime-configurable wheel) was tried and reverted: without a
compile-time-constant divisor, the compiler can't turn a division by the
wheel's modulus into a cheap multiply-shift, and that cost turned out to
grow faster than N. See [BENCHMARK.md](BENCHMARK.md) for the numbers behind
both of these.

**Bucket sieve.** A naive segmented sieve loops over every active base
prime on every segment, checking whether it has a multiple to mark there.
For a large prime (step comparable to or bigger than the segment) most of
those visits find nothing, and the check still isn't free -- that overhead
grows faster than N. The bucket sieve schedules each prime into the
"bucket" of the future segment where its next multiple actually falls (a
fixed-size ring, see `SegmentSieve` in `src/segment_sieve.hpp`); processing
a segment means looking at *only* its own bucket, marking, and
rescheduling each prime into whichever future bucket comes next. Newly
relevant primes (whose `p*p` just entered range) are picked up by a single
pointer that only moves forward through the (sorted) base-prime list, once
per prime per thread chunk -- not once per segment. Measured at N=10^12:
1.10x-1.62x faster depending on the wheel (more for wheels whose table
didn't fit cache); see BENCHMARK.md for why it doesn't change which wheel
wins.

**Segmented scan.** Base primes (<= sqrt(N)) are found first with a simple
in-memory sieve (always small: sqrt(10^12) = 10^6). The wheel-index range
is then walked in segments (`-s`, numeric width, default ~4.19M), marking
each base prime's multiples within the segment. With the bucket sieve, the
right segment width is a tradeoff between amortizing the roughly-fixed
per-segment overhead (wider is better) and keeping each thread's bit array
inside its cache (wider is worse past a point) -- tuning this from the
inherited default gave the single biggest speedup measured in this
project, see BENCHMARK.md.

**Parallelization.** The full range splits into as many contiguous chunks
as threads, each sieving its own chunk independently (base primes are
read-only, shared without locks). Work per number is roughly uniform, so a
static equal split already balances load well.

**Direct write, no merge pass.** An early version of this project wrote one
temp file per thread and concatenated them, which doubles disk I/O (every
output byte written once to the temp file, once again while merging). At
tens of GB of output that second copy was the real bottleneck. Instead:

1. **Count pass**: each thread sieves its chunk and only counts how many
   text bytes its primes will need -- no I/O.
2. Prefix sums over those counts give the exact offset where each thread
   must start writing, and the output file is resized to its final size up
   front (cheap even for tens of GB).
3. **Write pass**: each thread re-sieves the same chunk and writes with
   `pwrite()` directly into its own (disjoint) region of the final file, in
   parallel with the others.

The cost is repeating the (cheap, CPU/cache-bound) marking phase twice; the
payoff is never repeating the (expensive, I/O-bound) disk write.

## Results

See [BENCHMARK.md](BENCHMARK.md) for the full data: wheel-vs-N tradeoffs,
the bucket sieve's measured effect per wheel, the segment-width sweep, and
an external reference point (this project vs. a JS implementation on the
same hardware). Headline number: N=10^12 on an Intel i5-11400F (12
threads) went from 970.67s (first working version) to 89.54s across the
optimizations above -- a 10.84x cumulative speedup, same hardware, same
correct result throughout.

## Limitations

- Well past 10^12 (e.g. 10^14), even the `2,3,5` wheel's table may stop
  fitting L3 (it scales with `pi(sqrt(N))`; roughly 25MB at 10^14). At that
  point you'd want a smaller wheel (`2,3`) or a different table layout
  (e.g. shared across primes with the same residue mod the wheel, instead
  of one full row per prime).
- Per-thread write offsets use `pwrite`, which is POSIX-specific. A native
  Windows build would need `WriteFile` with `OVERLAPPED` (explicit offset)
  or a return to the temp-files-plus-merge scheme.
- Free disk space isn't checked up front: N=100b needs on the order of 49GB
  free at the destination, N=1t on the order of 500GB (use `--count-only`
  if you only care about pi(N)).

## Verification

Prime counts match the known value of pi(N) for N = 10, 100, 1000, 10^6,
2\*10^7, 10^8, 5\*10^7, 10^9, 10^10, 10^11 and 10^12, including the
boundaries around the wheel's own primes (11, 12, 13, 14). Output is
byte-for-byte identical (same hash) across thread counts (1, 3, 7, 12),
between `--count-only` and a normal write, across segment widths (`-s`
from 512 to several million -- which also stresses the bucket sieve's ring
sizing), and across wheels (`2,3` / `2,3,5` / `2,3,5,7` produce the same
file for the same N, rebuilding between each) -- ruling out both
chunk-boundary bugs and bugs specific to one wheel or ring size.
