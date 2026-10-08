# eratostenes

<img src="assets/eratostenes.jpg" alt="Eratosthenes" width="80" align="right">

A segmented, parallel, wheel-based Sieve of Eratosthenes written in C++. It is designed to count and generate primes up to very large N (10^15+), with a compact, randomly-indexable `.db` output format for storing dense prime tables at scale.

## Build

Requires a C++20 compiler, POSIX `pwrite`/`ftruncate` (Linux or WSL; does not build as-is with MSVC/native Windows), and the SQLite3 and zstd development libraries (for `.db` output):

```sh
sudo apt-get install build-essential libsqlite3-dev libzstd-dev   # Debian/Ubuntu/WSL
```

`make test` additionally needs [primecount](https://github.com/kimwalisch/primecount) on `PATH` -- it's the source of truth:

`make benchmark` requires [primesieve](https://github.com/kimwalisch/primesieve) on `PATH` to compare times.

```sh
sudo apt-get install primecount-bin primesieve  # Debian/Ubuntu/WSL
```

```sh
make                  # release build: -O3 -march=native -flto, plus nth_prime
make portable         # no -march=native, for a binary you'll copy to another machine
make debug            # ASan/UBSan, for debugging
make test             # checks output against primecount across N and across
                      # several parameter combinations, plus .db vs text output
make benchmark        # Count-only times against primesieve, N = 1e10..1e13
make benchmark-io     # .db size and write throughput, N = 1e8..1e13
make benchmark-tails  # Count-only, the last 1e11 numbers below 1e13..1e18
```

## Docker

```sh
make docker                             # build the image
make run ARGS="100b -o primes.txt"      # run it
```

`make run` mounts `./output` (host) at `/output` (container); use
`-o /output/<file>` in `ARGS` so the result lands somewhere accessible
outside the container.

## Usage

```sh
./eratostenes N [options]

  N                       Upper bound (inclusive), positional (no flag --
                          primesieve-style). Accepts suffixes:
                          k=1e3  m=1e6  b=g=1e9 (short-scale billion)  t=1e12
  -o, --output PATH       Output file. Without it, only counts primes --
                          no file is written. .db suffix switches to the
                          compact SQLite format (see below).
  -t, --threads N         Thread count (default: available cores)
  -s, --segment-width N   Numeric width per segment
                          (default: auto, sized from N and the caches)
      --db-block-size N   Primes per compressed block in .db mode (default: 65536)
      --zstd-level N      zstd compression level in .db mode (default: 1)
      --start N0          Only the range [N0, N]: count it, or with -o
                          write just its primes
      --max-mem N         Memory the run may take, e.g. 8g (default: 90% of
                          the available RAM; 0 = no limit): fewer threads
                          run when they wouldn't fit (each keeps ~8 bytes
                          per prime up to sqrt(N): ~1.5 GiB near 2^64)
  -h, --help              Help
```

```sh
./eratostenes 1m -o primes_1M.txt        # Write to text
./eratostenes 10b -t 12                   # Count using 12 threads
./eratostenes 1t -o ~/primes_1t.db        # Write to db
./eratostenes 1e18 --start 999999e12 -o tail.db   # Write only a range (a tail)
```

## Database

`-o out.db` writes two files that travel together: `out.db`, a small SQLite index (one row per block of 65536 primes: position, count, first prime, and where the block sits in the sidecar), and `out.blk`, the gap-encoded, zstd-compressed blocks themselves, written in parallel by every sieve thread. The index stays in the MBs; the `.blk` is ~0.55 bytes per prime (~17 TB at 1e15; on ext4 a single file tops out at 16 TiB, XFS has no such limit). To query for primes you can use the `nth_prime` companion:

```sh
./nth_prime out.db 1000000           # the 1,000,000th stored prime
./nth_prime out.db --count           # how many primes are stored (pi(limit) for a full run)
./nth_prime out.db --count X Y       # how many lie in [X, Y]
./nth_prime out.db --next X          # the smallest stored prime >= X, and its position
./nth_prime out.db --range X Y       # print the primes in [X, Y]
./nth_prime out.db --slice I J       # print the primes at positions I..J
./nth_prime out.db --info            # range, count, first/last prime, sizes

# with Docker only (the .db must be in ./output)
make nth-prime ARGS="/output/out.db 1000000"
```

A `.db` written with `--start N0` holds only the primes of [N0, N], and its positions are relative to that range: position 1 is the first prime >= N0 (its absolute index, pi(N0 - 1), can't be known without sieving [0, N0)). X and Y must lie inside the stored range.

`nth_prime` looks up the one block containing a position in the index (by `start_index`, not a table scan), reads just those bytes from the `.blk` with one `pread` and decodes that block; a value is found by a binary search over positions (~20 such lookups). Queries take a few milliseconds whatever the file's size, and printing streams the blocks in order. On the last 1e10 numbers below 1e18 (241M primes, a 148 MB `.blk`, i5-11400F): `--count` over the whole range 0.002 s against 2.0 s to sieve it again on 12 threads, `--next` 0.003 s, and printing all of it 4.0 s on one thread against 15.8 s for `primesieve -p`. It checks that the `.blk` next to the `.db` is the one it was written with (name and size).

Below the results for `./eratostenes limit -o base.db`:

| limit  | db size    | bit/prime |   MB/s | total(s) |
|--------|------------|----------:|-------:|---------:|
|1E8    | 2.47 MiB   |      3.59 |   43.1 |     0.06 |
|1E9    | 22.74 MiB  |      3.75 |  132.5 |     0.18 |
|1E10   | 212.07 MiB |      3.91 |  505.4 |     0.44 |
|1E11   | 1.95 GiB   |      4.06 |  678.9 |     3.08 |
|1E12   | 18.43 GiB  |      4.21 |  509.3 |    38.85 |
|1E13   | 174.89 GiB |      4.34 |  424.8 |   442.04 |

## Algorithms

What this project is built from, one term each — follow the link for the concept itself. See [docs/ALGORITHM.md](docs/ALGORITHM.md) for how they combine, and [docs/RESEARCH.md](docs/RESEARCH.md) for the log of tried, measured, and reverted optimization attempts behind the current design.

- [Segmented sieve](https://en.wikipedia.org/wiki/Sieve_of_Eratosthenes#Segmented_sieve)
- [Wheel factorization](https://en.wikipedia.org/wiki/Wheel_factorization)
- [Bucket sieve](https://en.wikipedia.org/wiki/Bucket_queue)
- [Memory pool](https://en.wikipedia.org/wiki/Memory_pool)
- [CPU cache](https://en.wikipedia.org/wiki/CPU_cache)
- [Load balancing](https://en.wikipedia.org/wiki/Load_balancing_(computing))
- [Random access](https://en.wikipedia.org/wiki/Random_access) 
- [Delta encoding](https://en.wikipedia.org/wiki/Delta_encoding)

## Benchmark

`./eratostenes N` (counts only) on an Intel Core i5-13500 (Raptor Lake, 2023), [primesieve](https://github.com/kimwalisch/primesieve) alongside it for reference:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.125s | 0.131s | 0.95x |
| 1e11 | 1.759s | 2.112s | 0.83x |
| 1e12 | 22.395s | 25.182s | 0.89x |
| 1e13 | 279.230s | 294.566s | 0.95x |
| 1e14 | 3386.88s | 3349.907s | 1.01x |
| 1e15 | 39517.53s | 39053.581s | 1.01x |

For more machines and results, see: [docs/BENCHMARK.md](docs/BENCHMARK.md).

## Validation

`make test` checks pi(N) and primes by position against [primecount](https://github.com/kimwalisch/primecount) across several N and parameter combinations (threads, segment width, cache-size overrides, `.db` block size, zstd level), and checks `.db` output against plain text output position by position.

## Issues

See [docs/ISSUES.md](docs/ISSUES.md) for known issues.