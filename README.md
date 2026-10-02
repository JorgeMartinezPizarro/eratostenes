# eratostenes

<img src="assets/eratostenes.jpg" alt="Eratosthenes" width="80" align="right">

A segmented, parallel, wheel-based Sieve of Eratosthenes writen in C++. It is designed to count and generate primes up to very large N (10^15+), with a compact, randomly-indexable `.db` output format for storing dense prime tables at scale.

## Build

Requires a C++20 compiler, POSIX `pwrite`/`ftruncate` (Linux or WSL; does not build as-is with MSVC/native Windows), and the SQLite3 and zstd development libraries (for `.db` output):

```sh
sudo apt-get install libsqlite3-dev libzstd-dev   # Debian/Ubuntu/WSL
```

`make test` additionally needs [primecount](https://github.com/kimwalisch/primecount) on `PATH` -- it's the source of truth:

`make benchmark` requires [primesieve](https://github.com/kimwalisch/primesieve) on `PATH` to compare times.

```sh
sudo apt-get install primecount-bin primesieve  # Debian/Ubuntu/WSL
```

```sh
make            # release build: -O3 -march=native -flto, plus nth_prime
make portable   # no -march=native, for a binary you'll copy to another machine
make pgo		# Build with a performance optimizations training.
make debug      # ASan/UBSan, for debugging
make test       # checks output against primecount across N and across
                # several parameter combinations, plus .db vs text output
make benchmark        # Count-only times against primesieve, N = 1e10..1e13
make benchmark-io     # .db size and write throughput, N = 1e8..1e12
make benchmark-tails  # Count-only, the last 1e11 numbers below 1e13..1e18
```

## Docker

```sh
make docker                                      # build the image
make run ARGS="100b -o /output/primes.txt -t 12" # run it
```

`make run` mounts `./output` (host) at `/output` (container); use
`-o /output/<file>` in `ARGS` so the result lands somewhere accessible
outside the container.

## Usage

```sh
./eratostenes N [options]

  N                       Upper bound (inclusive), positional (no flag --
                          primesieve-style). Accepts suffixes:
                          k=1e3  m=1e6  b=1e9 (short-scale billion)  t=1e12
  -o, --output PATH       Output file. Without it, only counts primes --
                          no file is written. .db suffix switches to the
                          compact SQLite format (see below).
  -t, --threads N         Thread count (default: available cores)
  -s, --segment-width N   Numeric width per segment
                          (default: auto, sized from N)
      --db-block-size N   Primes per compressed block in .db mode (default: 65536)
      --zstd-level N      zstd compression level in .db mode (default: 1)
  -h, --help              Help
```

```sh
./eratostenes 10000 -o primes_1M.txt      # Write to text        
./eratostenes 10b -t 12                   # Count using 12 threads
./eratostenes 1t -o ~/primes_100b.db      # Write to db
```

## Database

The `.db` format is a indexed sqlite file (max 256TB size), so it is suitable up to `e16`, around `160TB`. To query for primes you can use the `nth_prime` companion:

```sh
./nth_prime out.db 1000000     # the 1,000,000th prime
./nth_prime out.db --count     # pi(limit)

# with Docker only (the .db must be in ./output)
make nth-prime ARGS="/output/out.db 1000000"
```

`nth_prime` looks up the one block containing the requested position (indexed by `start_index`, not a table scan) and decodes just that block — lookups stay fast regardless of file size.

Below the results for `./eratostenes limit -o base.db`:

| limit  | db size    | bit/prime |   MB/s | total(s) |
|--------|------------|----------:|-------:|---------:|
|1E8    | 2.53 MiB   |      3.68 |   26.5 |     0.10 |
|1E9    | 23.65 MiB  |      3.90 |  112.7 |     0.22 |
|1E10   | 217.36 MiB |      4.01 |  296.0 |     0.77 |
|1E11   | 1.96 GiB   |      4.10 |  395.0 |     5.34 |
|1E12   | 18.84 GiB  |      4.30 |  317.5 |    63.70 |
|1E13   | 181.04 GiB  |      4.49 |  226.5 |     818.27 |

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

`./eratostenes N` (counts only) on an Intel Core i5-13500, [primesieve](https://github.com/kimwalisch/primesieve) alongside it for reference:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.15s | 0.137s | 1.09x |
| 1e11 | 1.49s | 1.656s | 0.91x |
| 1e12 | 20.93s | 23.724s | 0.92x |
| 1e13 | 285.32s | 292.763s | 0.97x |
| 1e14 | 3392.08s | 3349.907s | 1.01x |
| 1e15 | 40976.93s | - | - |

For more machines and results, see: [docs/BENCHMARK.md](docs/BENCHMARK.md).

## Validation

`make test` checks pi(N) and primes by position against [primecount](https://github.com/kimwalisch/primecount) across several N and parameter combinations (threads, segment width, cache-size overrides, `.db` block size, zstd level), and checks `.db` output against plain text output position by position.

## Issues

See [docs/ISSUES.md](docs/ISSUES.md) for known issues.