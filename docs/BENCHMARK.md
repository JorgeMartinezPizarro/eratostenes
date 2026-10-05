# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve), one section per machine, newest CPU first: the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), same thread count for both programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. Each time is the mean of REPS interleaved pairs (eratostenes, primesieve, eratostenes, ...); the line under each machine says how many, with the commit measured.

## Intel Xeon Emerald Rapids (claude.ai sandbox)

Intel Xeon @ 2.10GHz (Emerald Rapids, 2023), 2 vCPU.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.76s | 0.751s | 1.01x |
| 1e11 | 9.05s | 9.172s | 0.99x |
| 1e12 | 108.78s | 112.085s | 0.97x |
| 1e13 | 1358.15s | 1428.372s | 0.95x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 11.82s | 12.632s | 0.94x |
| 1e14 | 14.30s | 15.028s | 0.95x |
| 1e15 | 16.63s | 17.237s | 0.96x |
| 1e16 | 19.55s | 21.624s | 0.90x |
| 1e17 | 21.70s | 21.963s | 0.99x |
| 1e18 | 25.58s | 26.598s | 0.96x |

## Intel Core i5-13500

Intel Core i5-13500 (Raptor Lake, 2023), 64 GB. 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB. primesieve 11.0 (Debian, inside the `dev` container); eratostenes 5f7d213; mean of 3, pairs interleaved.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.14s | 0.137s | 1.02x |
| 1e11 | 1.87s | 2.105s | 0.89x |
| 1e12 | 23.03s | 25.332s | 0.91x |
| 1e13 | 286.09s | 295.222s | 0.97x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.02s | 3.227s | 0.94x |
| 1e14 | 3.84s | 3.968s | 0.97x |
| 1e15 | 4.39s | 4.634s | 0.95x |
| 1e16 | 5.08s | 5.297s | 0.96x |
| 1e17 | 6.15s | 6.249s | 0.98x |
| 1e18 | 7.61s | 8.202s | 0.93x |

## Intel Core i5-1235U

Intel Core i5-1235U (Alder Lake, 2022), 8 GB.

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

Intel Core i5-11400F (Rocket Lake, 2021).

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

## Intel Xeon @ 2.80GHz (claude.ai sandbox)

Intel Xeon @ 2.80GHz (Cascade Lake, 2019), 2 vCPU, 8 GB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.80s | 0.869s | 0.92x |
| 1e11 | 9.32s | 9.739s | 0.96x |
| 1e12 | 114.25s | 119.365s | 0.96x |
| 1e13 | 1502.30s | 1478.500s | 1.02x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 14.19s | 13.911s | 1.02x |
| 1e14 | 17.69s | 16.266s | 1.09x |
| 1e15 | 20.80s | 18.940s | 1.10x |
| 1e16 | 24.07s | 21.798s | 1.10x |
| 1e17 | 28.56s | 27.080s | 1.05x |
| 1e18 | 33.57s | 32.590s | 1.03x |

## Intel Core i5-3470

Intel Core i5-3470 (Ivy Bridge, 2012), 8 GB. 4 threads (1 per core); L1d 128 KiB (4 instances); L2 1 MiB (4 instances); L3 6 MiB. primesieve 12.12 (Ubuntu live USB); eratostenes 5f7d213; mean of 3, pairs interleaved.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.45s | 0.439s | 1.03x |
| 1e11 | 5.64s | 5.520s | 1.02x |
| 1e12 | 71.95s | 70.185s | 1.03x |
| 1e13 | 907.72s | 909.366s | 1.00x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 10.01s | 10.310s | 0.97x |
| 1e14 | 12.71s | 14.037s | 0.91x |
| 1e15 | 15.08s | 18.268s | 0.83x |
| 1e16 | 16.16s | 21.642s | 0.75x |
| 1e17 | 18.05s | 23.191s | 0.78x |
| 1e18 | 21.01s | 25.293s | 0.83x |

## Intel Core i7-620M

Intel Core i7-620M (Arrandale, 2010), 8 GB, running at 1.2 GHz (no battery: the SMC caps the clock; at its nominal 2.66 GHz it overheats).

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
