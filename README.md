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
./eratostenes 1000000 -o primes_1M.txt
./eratostenes 100b -o ~/primes_100b.txt -t 12
./eratostenes 100b -t 12 --count-only        # pi(N) or raw speed, no I/O
./eratostenes 100b -o ~/primes_100b.db -t 12 # compact indexable output
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

## Techniques

What this project is built from, one term each — follow the link for the
concept itself rather than this project's specific spin on it:

- [Segmented sieve](https://en.wikipedia.org/wiki/Sieve_of_Eratosthenes#Segmented_sieve)
- [Wheel factorization](https://en.wikipedia.org/wiki/Wheel_factorization)
- [Lookup table](https://en.wikipedia.org/wiki/Lookup_table) (precomputed bit patterns for the smallest primes, combined by bitwise OR at fill time instead of marking each one's multiples every segment -- `src/presieve.hpp`)
- [Bucket sieve](https://en.wikipedia.org/wiki/Bucket_queue)
- [Memory pool](https://en.wikipedia.org/wiki/Memory_pool) (the bucket sieve's ring: an intrusive linked list over a preallocated flat array, no per-segment heap allocation)
- [Bit array](https://en.wikipedia.org/wiki/Bit_array)
- [Hamming weight / popcount](https://en.wikipedia.org/wiki/Hamming_weight)
- [CPU cache](https://en.wikipedia.org/wiki/CPU_cache) (segment width and the table/on-the-fly prime-tier cutoff are both auto-tuned from the machine's real, detected L2/L3 size, not a fixed guess -- see `--l2-bytes`/`--l3-bytes` for when detection itself can't be trusted, e.g. inside a container)
- [Load balancing (computing)](https://en.wikipedia.org/wiki/Load_balancing_(computing)) (many more chunks than threads, pulled from a shared queue, since work per chunk isn't uniform across the range -- see `run_parallel_chunks`)
- [Random access](https://en.wikipedia.org/wiki/Random_access) (`pwrite()` into disjoint, precomputed regions of a pre-sized file lets every thread write its own share of the output in parallel with no locking and no merge step)
- [Delta encoding](https://en.wikipedia.org/wiki/Delta_encoding) (gaps between consecutive primes, for `.db`)
- [Zstandard](https://en.wikipedia.org/wiki/Zstd) (compresses the encoded gaps)
- [Database index](https://en.wikipedia.org/wiki/Database_index) (`.db` blocks are indexed by starting position, so `nth_prime` decodes the one block it needs instead of scanning)
- [SQLite](https://en.wikipedia.org/wiki/SQLite) (the `.db` container format)

## Benchmarks

`--count-only` (isolates CPU/cache work from disk I/O), mod 30, auto `-s`,
on an Intel Core i5-11400F (6 cores/12 threads, L1d 48KiB/core, L2
512KiB/core, L3 12MiB), primesieve alongside it for reference, same
machine, same thread count. Best of 3 reps (2 for 1e13), fresh
`wsl --shutdown` before the run:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.50s | 0.188s | 2.7x |
| 1e11 | 4.01s | 2.321s | 1.7x |
| 1e12 | 49.04s | 27.579s | 1.8x |
| 1e13 | 910.31s | 362.276s | 2.5x |

The 1e13 ratio jump comes from the auto segment width getting L2-capped
below sqrt(1e13), pushing ~32% of base primes into the costlier
`sparse_primes` tier instead of the cheap table tier (see
`segment_sieve.hpp`'s tier comments and `arg_parser.hpp`'s auto `-s`
logic for the full mechanics, and `presieve.hpp`/`segment_sieve.hpp` for
what's been tried against it and why).

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
Get-ChildItem "$env:LOCALAPPDATA\Packages" -Filter *.vhdx -Recurse | Select-Object FullName, Length
wsl --shutdown
```
Then open ```diskpart``` and add 

```powershell
select vdisk file="C:\ruta\completa\a\ext4.vhdx"
compact vdisk
```
