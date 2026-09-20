# Eratostenes

A segmented, parallel, wheel-based Sieve of Eratosthenes (C++20) for
generating or counting primes up to very large N (10^12+), with a compact,
randomly-indexable `.db` output format for storing dense prime tables at
scale.

## Build

Requires a C++20 compiler, POSIX `pwrite`/`ftruncate` (Linux or WSL; does
not build as-is with MSVC/native Windows), and the SQLite3 and zstd
development libraries (for `.db` output):

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

## Docker

```
make docker                                              # build the image
make run ARGS="--limit 100b -o /output/primes.txt -t 12" # run it
```

`make run` mounts `./output` (host) at `/output` (container); use
`-o /output/<file>` in `ARGS` so the result lands somewhere accessible
outside the container.

## Usage

```sh
./eratostenes --limit N [options]

  -n, --limit N          Upper bound (inclusive). Accepts suffixes:
                          k=1e3  m=1e6  b=1e9 (short-scale billion)  t=1e12
  -o, --output PATH      Output file (default: primes.txt). .db suffix
                          switches to the compact SQLite format (see below).
  -t, --threads N        Thread count (default: available cores)
  -s, --segment-width N  Numeric width per segment (default: 4194304)
  -c, --count-only       Only count primes, skip writing the file
      --db-block-size N  Primes per compressed block in .db mode (default: 65536)
      --zstd-level N     zstd compression level in .db mode (default: 3)
  -h, --help             Help
```

```sh
./eratostenes -n 1000000 -o primes_1M.txt
./eratostenes -n 100b -o ~/primes_100b.txt -t 12
./eratostenes -n 100b -t 12 --count-only        # pi(N) or raw speed, no I/O
./eratostenes -n 100b -o ~/primes_100b.db -t 12 # compact indexable output
```

## .db output (compact, indexable)

Plain text costs ~9-13 bytes/prime (pi(10^12) as text is ~450GB). `-o
out.db` writes primes as **gaps** between consecutive primes instead
(1 byte for the common case, escape byte + 4-byte value for rare larger
gaps — see `src/gap_encoding.hpp`), grouped into fixed-size blocks
(`--db-block-size`, default 65536 primes) and zstd-compressed
(`--zstd-level`) into a SQLite `blocks` table indexed by starting
position. Measured ~0.57-0.6 bytes/prime, ~18-20x smaller than text, while
staying randomly indexable.

```
./nth_prime out.db 1000000     # the 1,000,000th prime (1-indexed: N=1 -> 2)
./nth_prime out.db --count     # pi(limit)
```

`nth_prime` looks up the one block containing the requested position
(indexed by `start_index`, not a table scan) and decodes just that block —
lookups stay fast regardless of file size.

## How it sieves

- **Wheel factorization** (`src/wheel.hpp`) — skip multiples of a small
  fixed prime set (`WHEEL_PRIMES`, mod 30 by default) before sieving even
  starts; everything else (modulus, residues, per-prime jump tables) is
  derived at compile time via `constexpr`, so the compiler turns the wheel's
  modulus into a cheap multiply-shift instead of a runtime division.
- **Segmented scan** — base primes (<= sqrt(N)) are found once with a plain
  sieve, then the range is walked in fixed-width segments (`-s`) sized to
  fit cache, marking each base prime's multiples per segment.
- **Bucket sieve** (Tomás Oliveira e Silva's scheme, `src/segment_sieve.hpp`)
  — each base prime is scheduled into the bucket of the future segment
  where its next multiple actually falls, so processing a segment only
  touches the primes due that segment instead of checking all of them.
- **Pre-sieve** (`src/presieve.hpp`) — the smallest base primes hit every
  segment regardless of bucketing, so their periodic marking pattern is
  precomputed once and each segment is filled with a bulk shifted-word copy
  instead of a per-prime marking loop.
- **Dense/sparse split** — base primes below the segment width keep their
  full per-phase jump table (repeated hits per segment justify the table);
  primes at or above it get at most one hit per segment, so they're stored
  as just `p` and their next hit is recomputed on demand.
- **Parallel, direct, two-pass write** — the range splits into one
  contiguous chunk per thread (base primes are read-only, no locking
  needed). A count pass sizes the output file exactly; a write pass
  re-sieves and writes straight into each thread's disjoint region with
  `pwrite()` — every output byte written exactly once, no merge step.

Bigger wheels/deeper pre-sieves remove more candidates per number checked,
but their tables grow faster than the benefit — past a certain size the
table stops fitting L3 and the bottleneck shifts from CPU to memory
bandwidth. See [Tuning](#tuning-for-your-machine) below.

## Tuning for your machine

The two knobs that matter — wheel (`WHEEL_PRIMES` in `src/wheel.hpp`) and
`-s` (segment width) — both come down to fitting a per-thread structure
into a specific cache level. Get your cache sizes with `lscpu` (Linux/WSL)
or `Get-CimInstance Win32_CacheMemory` (native Windows PowerShell).

**Wheel vs L3**: `table_bytes = pi(sqrt(N)) * (8 + 4 * phi(wheel))`, shared
read-only across all threads. Pick the largest wheel whose table stays
comfortably under your L3 for the largest N you plan to run:

| N | pi(sqrt(N)) | `2,3` (mod 6) | `2,3,5` (mod 30) | `2,3,5,7` (mod 210) | `2,3,5,7,11` (mod 2310) |
|---|---:|---:|---:|---:|---:|
| 10^11 | 27,184 | 425 KiB | 1.0 MiB | 5.2 MiB | 50.0 MiB |
| 10^12 | 78,498 | 1.2 MiB | 3.0 MiB | 15.0 MiB | 144.3 MiB |
| 10^13 | ~224,000 | 3.4 MiB | 8.5 MiB | 42.7 MiB | 411.9 MiB |
| 10^14 | 620,160 | 9.5 MiB | 23.7 MiB | 118.3 MiB | 1140.4 MiB |
| 10^15 | ~1,955,000 | 29.8 MiB | 74.6 MiB | 372.9 MiB | 3593 MiB |

**Segment width vs L2**: `array_bytes = segment_width * phi(wheel) / wheel_mod / 8`,
replicated per thread:

| `-s` | `2,3` (mod 6) | `2,3,5` (mod 30) | `2,3,5,7` (mod 210) | `2,3,5,7,11` (mod 2310) |
|---|---:|---:|---:|---:|
| 2^22 = 4,194,304 (default) | 171 KiB | 137 KiB | 117 KiB | 106 KiB |
| 2^23 = 8,388,608 | 341 KiB | 273 KiB | 234 KiB | 213 KiB |
| 2^24 = 16,777,216 | 683 KiB | 546 KiB | 468 KiB | 426 KiB |

## Benchmarks

Measured on an Intel Core i5-11400F (6 cores/12 threads, L1d 48KiB/core, L2
512KiB/core, L3 12MiB), `--count-only` (isolates CPU/cache work from disk
I/O), mod 30 (the shipped default). [primesieve](https://github.com/kimwalisch/primesieve)
alongside it for reference, same machine, same thread count:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.53s | 0.192s | 2.8x |
| 1e11 | 5.54s | 4.599s | 1.2x |
| 1e12 | 76.59s | 27.422s | 2.8x |
| 1e13 | 1387.66s | 391.311s | 3.6x |

## Verification

`make test` checks pi(N) against the known value for N=1e8..1e11, plus a
handful of known primes by position in a real `.db` built at N=1e10, read
back with `nth_prime`. `make verify-db` checks the `.db` output mode
against plain text output: matching pi(N), and every (or, past a few
hundred thousand primes, a random sample of) position agreeing between the
two. Output has also been checked to be byte-for-byte identical across
thread counts, segment widths, and wheels for the same N.

## WSL disk reclaim

WSL2's virtual disk doesn't shrink back automatically after deleting large
files inside it. From PowerShell:

```powershell
diskpart
select vdisk file="C:\ruta\completa\a\ext4.vhdx"
compact vdisk
```
