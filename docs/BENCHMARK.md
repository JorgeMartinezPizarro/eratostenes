# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve), one section per machine, newest CPU first: the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), same thread count for both programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster.

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

Intel Core i5-13500 (Raptor Lake, 2023), 64 GB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.13s | 0.140s | 0.93x |
| 1e11 | 1.51s | 1.670s | 0.90x |
| 1e12 | 22.96s | 25.278s | 0.91x |
| 1e13 | 286.12s | 295.232s | 0.97x |
| 1e14 | 3392.08s | 3349.907s | 1.01x |
| 1e15 | 40664.78s | - | - |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.11s | 3.370s | 0.92x |
| 1e14 | 3.82s | 4.013s | 0.95x |
| 1e15 | 4.41s | 4.541s | 0.97x |
| 1e16 | 5.10s | 5.162s | 0.99x |
| 1e17 | 6.11s | 6.097s | 1.00x |
| 1e18 | 7.68s | 7.991s | 0.96x |

## Intel Core i5-1235U

Intel Core i5-1235U (Alder Lake, 2022), 8 GB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.34s | 0.438s | 0.78x |
| 1e11 | 5.72s | 6.616s | 0.86x |
| 1e12 | 73.01s | 91.081s | 0.80x |
| 1e13 | 980.21s | 1206.428s | 0.81x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 6.58s | 8.465s | 0.78x |
| 1e14 | 9.01s | 12.282s | 0.73x |
| 1e15 | 12.05s | 14.213s | 0.85x |
| 1e16 | 14.76s | 16.571s | 0.89x |
| 1e17 | 17.88s | 21.703s | 0.82x |
| 1e18 | N/A | N/A | N/A |

## Intel Core i5-11400F

Intel Core i5-11400F (Rocket Lake, 2021).

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.16s | 0.188s | 0.85x |
| 1e11 | 1.87s | 2.290s | 0.82x |
| 1e12 | 23.31s | 26.920s | 0.87x |
| 1e13 | 302.73s | 368.547s | 0.82x |
| 1e14 | 4185.20s | 5804.52s | 0.72x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.37s | 4.165s | 0.81x |
| 1e14 | 4.88s | 6.808s | 0.72x |
| 1e15 | 6.88s | 9.368s | 0.73x |
| 1e16 | 9.42s | 12.248s | 0.77x |
| 1e17 | 12.03s | 15.431s | 0.78x |
| 1e18 | 16.05s | 20.078s | 0.80x |

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

Intel Core i5-3470 (Ivy Bridge, 2012), 8 GB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.49s | 0.439s | 1.12x |
| 1e11 | 6.36s | 5.575s | 1.14x |
| 1e12 | 78.26s | 71.616s | 1.09x |
| 1e13 | 1013.19s | 943.535s | 1.07x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 10.95s | 10.732s | 1.02x |
| 1e14 | 14.87s | 14.208s | 1.05x |
| 1e15 | 16.84s | 16.925s | 0.99x |
| 1e16 | 18.95s | 18.930s | 1.00x |
| 1e17 | 21.47s | 23.550s | 0.91x |
| 1e18 | 25.76s | 26.688s | 0.97x |

## Intel Core i7-620M

Intel Core i7-620M (Arrandale, 2010), 8 GB. Tails: last 1e10 below N instead of the standard 1e11.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 3.25s | 4.046s | 0.80x |
| 1e11 | 39.49s | 48.221s | 0.82x |
| 1e12 | 496.56s | 561.887s | 0.88x |
| 1e13 | N/A | N/A | N/A |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 6.67s | 7.017s | 0.95x |
| 1e14 | 7.98s | 8.129s | 0.98x |
| 1e15 | 9.46s | 9.315s | 1.02x |
| 1e16 | 11.22s | 10.639s | 1.05x |
| 1e17 | 14.19s | 13.477s | 1.05x |
| 1e18 | 21.33s | 19.979s | 1.07x |
