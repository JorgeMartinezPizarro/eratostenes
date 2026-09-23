# Eratostenes

A segmented, parallel, wheel-based Sieve of Eratosthenes (C++20) for generating or counting primes up to very large N (10^12+), with a compact, randomly-indexable `.db` output format for storing dense prime tables at scale. [primesieve](https://github.com/kimwalisch/primesieve) is this project's reference, both for performance (see [Benchmarks](#benchmarks)) and for technique -- several of the ideas below come directly from reading its source. See [docs/ALGORITHM.md](docs/ALGORITHM.md) for how the pieces fit together.

## Build

Requires a C++20 compiler, POSIX `pwrite`/`ftruncate` (Linux or WSL; does not build as-is with MSVC/native Windows), and the SQLite3 and zstd development libraries (for `.db` output):

```sh
sudo apt-get install libsqlite3-dev libzstd-dev   # Debian/Ubuntu/WSL
```

`make test` additionally needs [primecount](https://github.com/kimwalisch/primecount) on `PATH` -- it's the source of truth for every expected pi(N)/nth-prime value the test checks against (no hardcoded constants):

```sh
sudo apt-get install primecount-bin   # Debian/Ubuntu/WSL
```

```sh
make            # release build: -O3 -march=native -flto, plus nth_prime
make portable   # no -march=native, for a binary you'll copy to another machine
make debug      # ASan/UBSan, for debugging
make test       # checks output against primecount across N and across
                # several parameter combinations, plus .db vs text output
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

What this project is built from, one term each — follow the link for the concept itself. See [docs/ALGORITHM.md](docs/ALGORITHM.md) for how they combine.

- [Segmented sieve](https://en.wikipedia.org/wiki/Sieve_of_Eratosthenes#Segmented_sieve)
- [Wheel factorization](https://en.wikipedia.org/wiki/Wheel_factorization)
- [Bucket sieve](https://en.wikipedia.org/wiki/Bucket_queue)
- [Memory pool](https://en.wikipedia.org/wiki/Memory_pool)
- [CPU cache](https://en.wikipedia.org/wiki/CPU_cache)
- [Load balancing (computing)](https://en.wikipedia.org/wiki/Load_balancing_(computing))
- [Random access](https://en.wikipedia.org/wiki/Random_access) 
- [Delta encoding](https://en.wikipedia.org/wiki/Delta_encoding)

## Benchmarks

`./eratostenenes N -c` on an Intel Core i5-13500, primesieve alongside it for reference:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.18s | 0.143s | 1.3x |
| 1e11 | 2.14s | 1.665s | 1.3x |
| 1e12 | 31.93s | 24.885s | 1.3x |
| 1e13 | 377.75s | 291.243s | 1.3x |

Below the same for i5-11400F:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.22s | 0.187s | 1.2x |
| 1e11 | 2.63s | 2.342s | 1.1x |
| 1e12 | 32.88s | 27.944s | 1.2x |
| 1e13 | 457.88s | 411.433s | 1.1x |

Below the results for `./eratostenes limit -o base.db`:

| limit  |          pi(N) | db size    | bit/prime |   MB/s | total(s) |
|--------|---------------:|------------|----------:|-------:|---------:|
|1E8    |      5,761,455 | 3.30 MiB   |      4.81 |   34.7 |     0.11 |
|1E9    |     50,847,534 | 28.75 MiB  |      4.74 |  143.6 |     0.28 |
|1E10   |    455,052,511 | 269.30 MiB |      4.96 |  320.9 |     1.08 |
|1E11   |  4,118,054,813 | 2.42 GiB   |      5.04 |  336.9 |    10.31 |
|1E12   | 37,607,912,018 | 22.46 GiB  |      5.13 |  312.0 |   110.13 |

## Verification

`make test` checks pi(N) and primes by position against [primecount](https://github.com/kimwalisch/primecount) across several N and parameter combinations (threads, segment width, cache-size overrides, `.db` block size, zstd level), and checks `.db` output against plain text output position by position.

## WSL disk reclaim

WSL2's virtual disk doesn't shrink back automatically after deleting large files inside it. From PowerShell:

```powershell
Get-ChildItem "$env:LOCALAPPDATA\Packages" -Filter *.vhdx -Recurse | Select-Object FullName, Length
wsl --shutdown
```
Then open ```diskpart``` and add 

```powershell
select vdisk file="C:\ruta\completa\a\ext4.vhdx"
compact vdisk
```
