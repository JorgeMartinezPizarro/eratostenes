# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve), one section per machine, newest CPU first: the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), same thread count for both programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. Each time is the mean of REPS interleaved pairs (eratostenes, primesieve, eratostenes, ...).

## Intel Core i5-13500

Intel Core i5-13500 (Raptor Lake, 2023), 64 GB. 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.123s | 0.127s | 0.97x |
| 1e11 | 1.773s | 2.101s | 0.84x |
| 1e12 | 22.360s | 25.227s | 0.89x |
| 1e13 | 279.024s | 296.144s | 0.94x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.066s | 3.175s | 0.97x |
| 1e14 | 3.637s | 3.791s | 0.96x |
| 1e15 | 4.223s | 4.548s | 0.93x |
| 1e16 | 4.875s | 5.266s | 0.93x |
| 1e17 | 5.698s | 6.186s | 0.92x |
| 1e18 | 7.337s | 7.954s | 0.92x |

## Intel Core i5-1235U

Intel Core i5-1235U (Alder Lake, 2022), 8 GB. 12 threads (2 per core); L1d 352 KiB (10 instances); L2 6.5 MiB (4 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.394s | 0.436s | 0.90x |
| 1e11 | 5.506s | 6.269s | 0.88x |
| 1e12 | 69.461s | 77.631s | 0.89x |
| 1e13 | 878.047s | 944.379s | 0.93x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 9.962s | 10.432s | 0.95x |
| 1e14 | 11.899s | 12.371s | 0.96x |
| 1e15 | 13.871s | 14.215s | 0.98x |
| 1e16 | 15.656s | 16.270s | 0.96x |
| 1e17 | 17.895s | 19.175s | 0.93x |
| 1e18 | 21.370s | 23.080s | 0.93x |

## Intel Core i5-11400F

Intel Core i5-11400F (Rocket Lake, 2021), 32 GB. 12 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.163s | 0.197s | 0.83x |
| 1e11 | 1.928s | 2.306s | 0.84x |
| 1e12 | 24.172s | 27.492s | 0.88x |
| 1e13 | 302.353s | 321.951s | 0.94x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.497s | 3.731s | 0.94x |
| 1e14 | 4.474s | 4.517s | 0.99x |
| 1e15 | 5.255s | 5.962s | 0.88x |
| 1e16 | 7.042s | 7.714s | 0.91x |
| 1e17 | 7.423s | 9.172s | 0.81x |
| 1e18 | 10.084s | 11.949s | 0.84x |

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
