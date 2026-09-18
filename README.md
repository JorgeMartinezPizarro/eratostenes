# eratostenes

A segmented, parallel, bit-packed Sieve of Eratosthenes for generating or
counting large prime lists.

## Build

Requires a C++20 compiler, POSIX `pwrite`/`ftruncate` (Linux or WSL; does
not build as-is with MSVC/native Windows), and the SQLite3 and zstd
development libraries (for `.db` output -- see below):

```
sudo apt-get install libsqlite3-dev libzstd-dev   # Debian/Ubuntu/WSL
```

```
make            # release build: -O3 -march=native -flto, plus nth_prime
make portable   # no -march=native, for a binary you'll copy to another machine
make debug      # ASan/UBSan, for debugging
make test       # checks pi(N) for N=1e8..1e11, plus known primes by
                # position in a real .db (N=1e10, via nth_prime)
make verify-db  # round-trips small N through .db output and checks it
                # against the text output, position by position
```

`make` also builds `nth_prime`, the reader for `.db` output (see below).
It's release-profile only (not a hot loop, so no portable/debug variants);
`make portable nth_prime` builds both explicitly if you need the portable
`eratostenes` alongside it.

If you're working on Windows with the project under `/mnt/c/...`, build and
run **inside WSL**, pointing output at a native Linux directory (e.g.
`~/...`), not `/mnt/c/...`: that mount is much slower for heavy I/O.

## Docker

```
make docker                                              # build the image
make run ARGS="--limit 100b -o /output/primes.txt -t 12" # run it
```

`make run` mounts `./output` (host) at `/output` (container); use
`-o /output/<file>` in `ARGS` so the result lands somewhere accessible
outside the container. With no `ARGS`, it runs `--help`.

## Usage

```
./eratostenes --limit N [options]

  -n, --limit N          Upper bound (inclusive). Accepts suffixes:
                          k=1e3  m=1e6  b=1e9 (short-scale billion)  t=1e12
                          e.g. 100b = 10^11
  -o, --output PATH      Output file (default: primes.txt). If PATH ends
                          in .db, writes SQLite instead of plain text --
                          see ".db output" below.
  -t, --threads N        Thread count (default: available cores)
  -s, --segment-width N  Numeric width per segment (default: 4194304)
  -c, --count-only       Only count primes, skip writing the file
      --db-block-size N  Primes per compressed block in .db mode (default: 65536)
      --zstd-level N     zstd compression level in .db mode (default: 3)
  -h, --help             Help
```

Which primes get skipped up front (the wheel) is fixed at compile time in
`src/wheel.hpp` (`WHEEL_PRIMES`) -- see "Wheel factorization" below.

Examples:

```
./eratostenes -n 1000000 -o primes_1M.txt
./eratostenes -n 100b -o ~/primes_100b.txt -t 12
./eratostenes -n 100b -t 12 --count-only
./eratostenes -n 100b -o ~/primes_100b.db -t 12
```

`--count-only` skips the write pass entirely: no file is created, nothing
is converted to text. Use it to get pi(N) or to measure raw sieve speed
without disk I/O -- essential from around 10^11 up, where the equivalent
text file already weighs tens of GB.

## .db output (compact, indexable)

Plain text costs ~9-13 bytes/prime and grows every decade (pi(10^12) as
text is already ~450GB). `-o out.db` writes a SQLite3 file instead,
directly from the sieve (the text intermediate is never created), using
gap encoding + zstd:

- Primes are stored as the **gaps** between consecutive primes, not their
  absolute values: 1 byte per gap in the common case (`byte = gap/2`,
  since every gap above 2 is even), with an escape byte + 4-byte value for
  the rare larger gaps and the one odd gap (2->3). See `src/gap_encoding.hpp`.
- Gaps are grouped into fixed-size blocks (`--db-block-size`, default
  65536 primes/block) and each block is zstd-compressed
  (`--zstd-level`, default 3 -- entropy coding, where zstd gets most of
  its ratio on this near-random byte stream, barely depends on level, so
  a low level keeps builds fast without giving up much size) and stored
  as one row in a `blocks` table, indexed by its starting position.
- Measured: ~0.57-0.59 bytes/prime, flat from 10^7 to 10^9 (prime gap
  entropy grows only as log(log N)) -- roughly **18-20x smaller** than
  the text output, with the same file remaining randomly indexable.

Read a `.db` file with the paired reader binary (also built by `make`):

```
./nth_prime out.db 1000000     # the 1,000,000th prime (1-indexed: N=1 -> 2)
./nth_prime out.db --count     # pi(limit): how many primes are stored
```

It looks up the one block containing the requested position (indexed by
`start_index`, not a table scan) and decodes just that block -- lookups
stay fast regardless of how large the `.db` file is.

`make verify-db` round-trips small N through both output modes and checks
every (or, at larger N, a random sample of) position between them.

## Tuning for your machine

The two knobs that matter -- which wheel (`WHEEL_PRIMES` in
`src/wheel.hpp`) and `-s` (segment width) -- both come down to fitting a
per-thread data structure into a specific cache level. Find your CPU's
cache sizes first:

```
lscpu                                              # Linux / WSL: L1d, L1i, L2, L3 sizes
cat /sys/devices/system/cpu/cpu0/cache/index*/size # per-level detail, same info
```

On native Windows, `Get-CimInstance Win32_CacheMemory` (PowerShell) or the
CPU's page on ark.intel.com / similar will have L2/L3; L1 rarely matters
for this tuning.

**Wheel vs L3.** Each base prime costs `8 + 4*phi(wheel)` bytes in the
shared jump table, and there are `pi(sqrt(N))` base primes, so the table
is:

```
table_bytes = pi(sqrt(N)) * (8 + 4 * phi(wheel))
```

Pick the largest of the prepared configs in `src/wheel.hpp` whose
`table_bytes` stays comfortably under your L3 (leave headroom -- other
things share that cache too), for the largest N you plan to run:

| N | pi(sqrt(N)) | `2,3` (mod 6) | `2,3,5` (mod 30) | `2,3,5,7` (mod 210) | `2,3,5,7,11` (mod 2310) |
|---|---:|---:|---:|---:|---:|
| 10^10 | 9,592 | 150 KiB | 375 KiB | 1.8 MiB | 17.6 MiB |
| 10^11 | 27,184 | 425 KiB | 1.0 MiB | 5.2 MiB | 50.0 MiB |
| 10^12 | 78,498 | 1.2 MiB | 3.0 MiB | 15.0 MiB | 144.3 MiB |
| 10^13 | ~224,000 | 3.4 MiB | 8.5 MiB | 42.7 MiB | 411.9 MiB |
| 10^14 | 620,160 | 9.5 MiB | 23.7 MiB | 118.3 MiB | 1140.4 MiB |
| 10^15 | ~1,955,000 | 29.8 MiB | 74.6 MiB | 372.9 MiB | 3593 MiB |

(`pi(sqrt(N))` for 10^13 and 10^15 is approximate -- sqrt(N) isn't a round
number there, so it's the standard `x/(ln(x)-1)` estimate rather than an
exact count; 10^10/10^11/10^12/10^14 are exact.)

**Segment width vs L2.** Each thread's per-segment bit array is:

```
array_bytes = segment_width * phi(wheel) / wheel_mod / 8
```

| `-s` | `2,3` (mod 6) | `2,3,5` (mod 30) | `2,3,5,7` (mod 210) | `2,3,5,7,11` (mod 2310) |
|---|---:|---:|---:|---:|
| 2^22 = 4,194,304 (default) | 171 KiB | 137 KiB | 117 KiB | 106 KiB |
| 2^23 = 8,388,608 | 341 KiB | 273 KiB | 234 KiB | 213 KiB |
| 2^24 = 16,777,216 | 683 KiB | 546 KiB | 468 KiB | 426 KiB |
| 2^25 = 33,554,432 | 1.33 MiB | 1.07 MiB | 936 KiB | 851 KiB |
| 2^26 = 67,108,864 | 2.67 MiB | 2.13 MiB | 1.83 MiB | 1.66 MiB |
| 2^27 = 134,217,728 | 5.33 MiB | 4.27 MiB | 3.66 MiB | 3.33 MiB |

Pick the largest `-s` that keeps this comfortably under your per-core L2
(on a chip with mixed core types, use the smallest L2-per-thread figure
across all core types). From there, sweep a few values around it -- the
optimum is usually a flat plateau, not a single sharp point, and it's cheap
to check directly with `--count-only` on a middling N.

## Design

**Wheel factorization.** `WHEEL_PRIMES` in `src/wheel.hpp` picks which
primes are skipped up front; everything else (modulus, residues, position
table) is derived from it at compile time via `constexpr`. Numbers that
survive the wheel are numbered with a continuous "wheel index" k=0,1,2,...,
and each segment is a bit array (`uint64_t` words) where bit i corresponds
to `wheel_number(k_low + i)`. The wheel's own primes are emitted directly
(special-cased on thread 0). Marking a base prime's multiples without
dividing in the hot loop needs a precomputed jump table per prime
(`compute_wheel_deltas`); every prime's table lives in one flat, contiguous
buffer (a `std::array` inside `WheelBasePrime`) stored as `uint32_t` to
keep its cache footprint small.

Bigger wheels remove more candidates per number checked, but the jump
table needed to do that grows faster than the benefit: adding prime p
multiplies the table by `(p-1)` but only cuts marking work by `(p-1)/p`.
Past a certain size that table stops fitting L3 and the bottleneck shifts
from CPU to memory bandwidth -- see [Benchmarks](#benchmarks) below for
measured timings across wheels and N. The wheel is a
compile-time choice (not a runtime flag) because the compiler can turn a
division by a compile-time-constant modulus into a cheap multiply-shift,
which it can't do for a runtime value.

**Bucket sieve.** Rather than checking every base prime against every
segment, each prime is scheduled into the "bucket" of the future segment
where its next multiple actually falls (a fixed-size ring, see
`SegmentSieve` in `src/segment_sieve.hpp`). Processing a segment means
looking at only its own bucket: mark, then reschedule each prime into
whichever future bucket comes next. Primes become relevant to the sieve in
order of size, so a single forward-only pointer over the (sorted)
base-prime list picks up newly relevant ones once per thread chunk.

**Segmented scan.** Base primes (<= sqrt(N)) are found first with a simple
in-memory sieve. The wheel-index range is then walked in segments (`-s`,
numeric width, default ~4.19M), marking each base prime's multiples within
the segment.

**Parallelization.** The full range splits into as many contiguous chunks
as threads, each sieving its own chunk independently (base primes are
read-only, shared without locks).

**Direct write, two passes, no merge.** Writing is split into a count pass
(each thread sieves its chunk and only counts output bytes, no I/O) and a
write pass (each thread re-sieves the same chunk and writes with
`pwrite()` directly into its own, disjoint region of the already-sized
output file, in parallel with the others). This means every output byte is
written exactly once, at the cost of sieving each chunk twice.

**`.db` output** (see [above](#db-output-compact-indexable) for the
format) reuses the same two-pass shape, but the count pass only needs
prime counts (`NullSink`, cheaper than the text path's byte counting), and
the write pass has no fixed byte offsets to land on: each thread's
`GapBlockSink` (`src/gap_block_sink.hpp`) gap-encodes and zstd-compresses
its own primes into fixed-size, self-describing blocks (each carries its
own starting position and count) and pushes them to a thread-safe queue.
A single dedicated writer thread (`SqlitePrimeStore`,
`src/sqlite_prime_store.hpp`) drains that queue into SQLite with batched
transactions -- this keeps SQLite's single-writer constraint off the
sieve/compress hot path, which stays fully parallel; only the
already-compressed insert step is serialized.

## Limitations

- Well past 10^12 (e.g. 10^14), even the `2,3,5` wheel's table may stop
  fitting L3 (it scales with `pi(sqrt(N))`). At that point a smaller wheel
  (`2,3`) or a different table layout would be needed.
- Per-thread write offsets use `pwrite`, which is POSIX-specific.
- Free disk space isn't checked up front: N=100b needs on the order of
  49GB free at the destination, N=1t on the order of 500GB (use
  `--count-only` if you only care about pi(N)).

## Verification

`make test` checks pi(N) against the known value for N=1e8..1e11, plus a
handful of known primes by position (the 1st, 1000th, 10000th, 200
millionth, and last) in a real `.db` built at N=1e10, read back with
`nth_prime`. Output has also been checked to be byte-for-byte identical
(same hash) across thread counts, segment widths, and wheels for the
same N.

`make verify-db` checks the `.db` output mode against that same text
output: matching pi(N), and every (or, past a few hundred thousand
primes, a random sample of) position agreeing between the two.

## Benchmarks

Measured on an Intel Core i5-11400F (6 cores / 12 threads, L3 = 12 MB) with
`--count-only` (isolates CPU/cache work from disk I/O), rebuilding between
runs with each wheel active. See [Tuning for your
machine](#tuning-for-your-machine) above for how the jump-table size scales
with wheel and N.

| N | mod 6 | mod 30 | mod 210 |
|---|---:|---:|---:|
| 1e9 | 0.50s | 0.50s | 0.50s |
| 1e10 | 1.00s | 1.00s | 0.50s |
| 1e11 | 8.00s | 5.51s | 5.51s |
| 1e12 | 91.04s | 73.55s | 137.63s |

`pi(N)` matched the known value at every N (455,052,511 / 4,118,054,813 /
37,607,912,018). The repo ships with mod 30 active, since 10^12 and up is
this project's main target range.

## Free memory of WSL desde windows

In the terminal, run:

```powershell
diskpart
```

and then write:

```powershell
select vdisk file="C:\ruta\completa\a\ext4.vhdx" 
compact vdisk
```


