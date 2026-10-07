# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve), one section per machine, newest CPU first: the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), same thread count for both programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. Each time is the mean of REPS interleaved pairs (eratostenes, primesieve, eratostenes, ...).

## Intel Core i5-13500

Intel Core i5-13500 (Raptor Lake, 2023), 64 GB. 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.126s | 0.129s | 0.98x |
| 1e11 | 1.785s | 2.101s | 0.85x |
| 1e12 | 22.293s | 25.074s | 0.89x |
| 1e13 | 278.068s | 293.409s | 0.95x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.043s | 3.228s | 0.94x |
| 1e14 | 3.636s | 3.774s | 0.96x |
| 1e15 | 4.228s | 4.568s | 0.93x |
| 1e16 | 4.884s | 5.257s | 0.93x |
| 1e17 | 5.726s | 6.217s | 0.92x |
| 1e18 | 7.305s | 8.051s | 0.91x |

## Intel Core i5-1235U

Intel Core i5-1235U (Alder Lake, 2022), 8 GB. 12 threads (2 per core); L1d 288 KiB (6 instances); L2 7.5 MiB (6 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.31s | 0.33s | 0.94x |
| 1e11 | 5.11s | 5.81s | 0.88x |
| 1e12 | 72.36s | 80.14s | 0.90x |
| 1e13 | 914.60s | 973.76s | 0.94x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 10.18s | 10.559s | 0.96x |
| 1e14 | 12.25s | 12.632s | 0.97x |
| 1e15 | 14.13s | 14.599s | 0.97x |
| 1e16 | 16.21s | 16.732s | 0.97x |
| 1e17 | 18.74s | 19.173s | 0.98x |
| 1e18 | N/A | N/A | N/A |

## Intel Core i5-11400F

Intel Core i5-11400F (Rocket Lake, 2021), 32 GB. 12 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.17s | 0.197s | 0.86x |
| 1e11 | 1.95s | 2.366s | 0.82x |
| 1e12 | 23.46s | 27.114s | 0.87x |
| 1e13 | 310.42s | 361.227s | 0.86x |
| 1e14 | 4185.20s | 5804.52s | 0.72x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.77s | 4.81s | 0.78x |
| 1e14 | 4.84s | 6.95s | 0.70x |
| 1e15 | 6.75s | 9.66s | 0.70x |
| 1e16 | 9.14s | 12.45s | 0.73x |
| 1e17 | 12.21s | 15.79s | 0.77x |
| 1e18 | 17.34s | 21.47s | 0.81x |

## Intel Core i5-3470

Intel Core i5-3470 (Ivy Bridge, 2012), 8 GB. 4 threads (1 per core); L1d 128 KiB (4 instances); L2 1 MiB (4 instances); L3 6 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.436s | 0.439s | 0.99x |
| 1e11 | 5.501s | 5.525s | 1.00x |
| 1e12 | 69.330s | 70.163s | 0.99x |
| 1e13 | 871.653s | 909.435s | 0.96x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 9.629s | 10.350s | 0.93x |
| 1e14 | 12.467s | 14.367s | 0.87x |
| 1e15 | 14.819s | 18.404s | 0.81x |
| 1e16 | 17.198s | 21.258s | 0.81x |
| 1e17 | 20.093s | 25.823s | 0.78x |
| 1e18 | 21.590s | 27.158s | 0.79x |

## Intel Core i7-620M

Intel Core i7-620M (Arrandale, 2010), 8 GB. 4 threads (2 per core); L1d 64 KiB (2 instances); L2 512 KiB (2 instances); L3 4 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 3.12s | 3.969s | 0.79x |
| 1e11 | 42.25s | 47.880s | 0.88x |
| 1e12 | 481.29s | 556.640s | 0.86x |
| 1e13 | 6082.61s | 6432.604s | 0.95x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 65.92s | 69.739s | 0.95x |
| 1e14 | 77.45s | 77.720s | 1.00x |
| 1e15 | 90.24s | 87.702s | 1.03x |
| 1e16 | 101.33s | 96.647s | 1.05x |
| 1e17 | 121.30s | 109.183s | 1.11x |
| 1e18 | 134.05s | 122.093s | 1.10x |
