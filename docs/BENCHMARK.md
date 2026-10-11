# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve), one section per machine, newest CPU first: the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), same thread count for both programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. Each time is the mean of REPS interleaved pairs (eratostenes, primesieve, eratostenes, ...).

## Intel Core i5-13500

Intel Core i5-13500 (Raptor Lake, 2023), 64 GB. 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.151s | 0.157s | 0.96x |
| 1e11 | 1.819s | 2.086s | 0.87x |
| 1e12 | 22.437s | 25.133s | 0.89x |
| 1e13 | 278.587s | 292.453s | 0.95x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.054s | 3.170s | 0.96x |
| 1e14 | 3.608s | 3.799s | 0.95x |
| 1e15 | 4.233s | 4.598s | 0.92x |
| 1e16 | 4.921s | 5.247s | 0.94x |
| 1e17 | 5.746s | 6.095s | 0.94x |
| 1e18 | 7.150s | 7.761s | 0.92x |

## Intel Core i5-1235U

Intel Core i5-1235U (Alder Lake, 2022), 8 GB. 12 threads (2 per core); L1d 352 KiB (10 instances); L2 6.5 MiB (4 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.424s | 0.439s | 0.97x |
| 1e11 | 5.490s | 6.238s | 0.88x |
| 1e12 | 69.387s | 77.155s | 0.90x |
| 1e13 | 881.015s | 940.546s | 0.94x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 9.753s | 10.182s | 0.96x |
| 1e14 | 11.567s | 12.345s | 0.94x |
| 1e15 | 13.552s | 14.264s | 0.95x |
| 1e16 | 15.563s | 16.360s | 0.95x |
| 1e17 | 17.822s | 18.963s | 0.94x |
| 1e18 | 21.429s | 23.486s | 0.91x |

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
| 1e10 | 0.438s | 0.437s | 1.00x |
| 1e11 | 5.506s | 5.524s | 1.00x |
| 1e12 | 69.078s | 70.178s | 0.98x |
| 1e13 | 868.821s | 910.464s | 0.95x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 9.656s | 10.473s | 0.92x |
| 1e14 | 12.341s | 14.575s | 0.85x |
| 1e15 | 14.808s | 18.530s | 0.80x |
| 1e16 | 17.175s | 21.861s | 0.79x |
| 1e17 | 19.207s | 25.711s | 0.75x |
| 1e18 | 21.161s | 26.772s | 0.79x |

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
