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
- [Bucket sieve](https://en.wikipedia.org/wiki/Bucket_queue) (used only for the rare primes with at most ~1 hit/segment — see `src/segment_sieve.hpp`)
- [Bit array](https://en.wikipedia.org/wiki/Bit_array)
- [Hamming weight / popcount](https://en.wikipedia.org/wiki/Hamming_weight)
- [Delta encoding](https://en.wikipedia.org/wiki/Delta_encoding) (gaps between consecutive primes, for `.db`)
- [Zstandard](https://en.wikipedia.org/wiki/Zstd) (compresses the encoded gaps)
- [SQLite](https://en.wikipedia.org/wiki/SQLite) (the `.db` container format)

## Benchmarks

`--count-only` (isolates CPU/cache work from disk I/O), mod 30, auto `-s`,
on an Intel Core i5-11400F (6 cores/12 threads, L1d 48KiB/core, L2
512KiB/core, L3 12MiB), primesieve alongside it for reference, same
machine, same thread count:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.50s | 0.193s | 2.6x |
| 1e11 | 4.01s | 2.32s | 1.7x |
| 1e12 | 48.03s | 27.51s | 1.7x |

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
