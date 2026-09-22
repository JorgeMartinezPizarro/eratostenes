# Eratostenes

A segmented, parallel, wheel-based Sieve of Eratosthenes (C++20) for
generating or counting primes up to very large N (10^12+), with a compact,
randomly-indexable `.db` output format for storing dense prime tables at
scale. [primesieve](https://github.com/kimwalisch/primesieve) is this
project's reference, both for performance (see [Benchmarks](#benchmarks))
and for technique -- several of the ideas below come directly from reading
its source.

## Build

Requires a C++20 compiler, POSIX `pwrite`/`ftruncate` (Linux or WSL; does
not build as-is with MSVC/native Windows), and the SQLite3 and zstd
development libraries (for `.db` output):

```sh
sudo apt-get install libsqlite3-dev libzstd-dev   # Debian/Ubuntu/WSL
```

```sh
make            # release build: -O3 -march=native -flto, plus nth_prime
make portable   # no -march=native, for a binary you'll copy to another machine
make debug      # ASan/UBSan, for debugging
make test       # checks pi(N) for N=1e8..1e11, known primes by position
                # in a real .db (N=1e10, via nth_prime), and round-trips
                # smaller N through .db output checked against text output,
                # position by position
```

## Docker

```sh
make docker                                              # build the image
make run ARGS="100b -o /output/primes.txt -t 12"          # run it
```

`make run` mounts `./output` (host) at `/output` (container); use
`-o /output/<file>` in `ARGS` so the result lands somewhere accessible
outside the container.

## Usage

```sh
./eratostenes N [options]

  N                      Upper bound (inclusive), positional (no flag --
                          primesieve-style). Accepts suffixes:
                          k=1e3  m=1e6  b=1e9 (short-scale billion)  t=1e12
  -o, --output PATH      Output file (default: primes.txt). .db suffix
                          switches to the compact SQLite format (see below).
  -t, --threads N        Thread count (default: available cores)
  -s, --segment-width N  Numeric width per segment
                          (default: auto, sized from N)
  -c, --count-only       Only count primes, skip writing the file
      --db-block-size N  Primes per compressed block in .db mode (default: 65536)
      --zstd-level N     zstd compression level in .db mode (default: 3)
  -h, --help             Help
```

```sh
./eratostenes 10000 -o primes_1M.txt
./eratostenes 100m -o ~/primes_100b.txt -t 12
./eratostenes 10b -t 12 --count-only
./eratostenes 1t -o ~/primes_100b.db
```

## DB compression

The `.db` format is a indexed sqlite file (max 256TB size), so it is suitable up to `e16`, around `200TB`. To query for primes you can use the `nth_prime` companion:

```sh
./nth_prime out.db 1000000     # the 1,000,000th prime (1-indexed: N=1 -> 2)
./nth_prime out.db --count     # pi(limit)
```

`nth_prime` looks up the one block containing the requested position (indexed by `start_index`, not a table scan) and decodes just that block — lookups stay fast regardless of file size.

## Techniques

What this project is built from, one term each — follow the link for the concept itself:

- [Segmented sieve](https://en.wikipedia.org/wiki/Sieve_of_Eratosthenes#Segmented_sieve)
- [Wheel factorization](https://en.wikipedia.org/wiki/Wheel_factorization)
- [Bucket sieve](https://en.wikipedia.org/wiki/Bucket_queue)
- [Memory pool](https://en.wikipedia.org/wiki/Memory_pool) (the bucket sieve's ring: an intrusive linked list over a preallocated flat array, no per-segment heap allocation)
- [CPU cache](https://en.wikipedia.org/wiki/CPU_cache) (segment width and the table/on-the-fly prime-tier cutoff are both auto-tuned from the machine's real, detected L2/L3 size, not a fixed guess -- see `--l2-bytes`/`--l3-bytes` for when detection itself can't be trusted, e.g. inside a container)
- [Load balancing (computing)](https://en.wikipedia.org/wiki/Load_balancing_(computing)) (many more chunks than threads, pulled from a shared queue, since work per chunk isn't uniform across the range -- see `run_parallel_chunks`)
- [Random access](https://en.wikipedia.org/wiki/Random_access) (`pwrite()` into disjoint, precomputed regions of a pre-sized file lets every thread write its own share of the output in parallel with no locking and no merge step)
- [Delta encoding](https://en.wikipedia.org/wiki/Delta_encoding) (gaps between consecutive primes, for `.db`)

## Benchmarks

`-c` on an Intel Core i5-13500, primesieve alongside it for reference, same machine:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.50s | 0.198s | 2.5x |
| 1e11 | 3.00s | 1.731s | 1.7x |
| 1e12 | 41.01s | 25.051s | 1.6x |
| 1e13 | 473.61s | 290.832s | 1.6x |

Under an i5-11400F (dev PC, two generations older) the best-of-3 times
achieved for 1e11-1e13 are 4.01s, 47.03s, 878.64s -- kept as a baseline
for this weaker machine, not a fair ratio comparison (no primesieve run
alongside it here).

Writting primes to a `.db` file results in the following ratios per prime:

| limit  |          pi(N) | db size    | bit/prime |   MB/s | total(s) |
|--------|---------------:|------------|----------:|-------:|---------:|
|1E8    |      5,761,455 | 3.29 MiB   |      4.79 |    6.0 |     1.09 |
|1E9    |     50,847,534 | 28.72 MiB  |      4.74 |   48.6 |     1.13 |
|1E10   |    455,052,511 | 269.30 MiB |      4.96 |  263.9 |     1.58 |
|1E11   |  4,118,054,813 | 2.42 GiB   |      5.04 |  317.1 |    11.19 |
|1E12   | 37,607,912,018 | 22.46 GiB  |      5.13 |  279.0 |   127.46 |

## Verification

`make test` checks pi(N) against the known value for N=1e8..1e11, plus a
handful of known primes by position in a real `.db` built at N=1e10, read
back with `nth_prime`. It also checks the `.db` output mode against plain
text output: matching pi(N), and every (or, past a few hundred thousand
primes, a random sample of) position agreeing between the two. Output has
also been checked to be byte-for-byte identical across thread counts,
segment widths, and wheels for the same N.

## WSL disk reclaim

WSL2's virtual disk doesn't shrink back automatically after deleting large
files inside it. From PowerShell:

```powershell
Get-ChildItem "$env:LOCALAPPDATA\Packages" -Filter *.vhdx -Recurse | Select-Object FullName, Length
wsl --shutdown
```
Then open ```diskpart``` and add 

```powershell
select vdisk file="C:\ruta\completa\a\ext4.vhdx"
compact vdisk
```
