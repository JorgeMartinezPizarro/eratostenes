# Benchmark

Measured on an Intel Core i5-11400F (6 cores / 12 logical threads, L3 = 12
MB), using `--count-only` (no disk I/O) to isolate CPU/cache work:

```
./eratostenes -n 10000000000 -t 12 --count-only    # 1e10
./eratostenes -n 100000000000 -t 12 --count-only   # 1e11
./eratostenes -n 1t -t 12 --count-only             # 1e12
```

`pi(N)` matched the known value in every run below: pi(10^10) =
455,052,511, pi(10^11) = 4,118,054,813, pi(10^12) = 37,607,912,018.

## Why wheel size matters

Each base prime needs a jump table of `phi(wheel)` entries (`uint32_t`, 4
bytes each). That table, times the number of base primes (`pi(sqrt(N))`),
has to fit the L3 shared by all threads for the sieve to stay CPU-bound
instead of memory-bandwidth-bound:

| N | pi(sqrt(N)) | mod 6 table | mod 30 table | mod 210 table |
|---|---:|---:|---:|---:|
| 10^10 | 9,592 | 154KB | 384KB | 1.9MB |
| 10^11 | 27,184 | 435KB | 1.1MB | 5.4MB |
| 10^12 | 78,498 | 1.3MB | 3.1MB | 15.7MB |

## Results

`WHEEL_PRIMES` in `src/wheel.hpp` set to each wheel in turn, rebuilding
(`make clean && make`) between runs:

| N | mod 6 | mod 30 | mod 210 | winner |
|---|---:|---:|---:|---|
| 10^10 | 1.00s | 1.01s | 1.01s | tie |
| 10^11 | 10.51s | 8.01s | **7.02s** | mod 210 |
| 10^12 | 123.58s | **89.54s** | 142.60s | mod 30 |

`WHEEL_PRIMES` is set to `{2,3,5}` (mod 30) in the repository, since 10^12
and up is this project's main target range.
