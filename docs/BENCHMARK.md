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
| 1e10 | 0.13s | 0.146s | 0.89x |
| 1e11 | 1.88s | 1.962s | 0.96x |
| 1e12 | 22.83s | 25.307s | 0.90x |
| 1e13 | 284.67s | 293.805s | 0.97x |
| 1e14 | 3392.08s | 3349.907s | 1.01x |
| 1e15 | 40381.23s | - | - |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 2.56s | 2.860s | 0.90x |
| 1e14 | 3.75s | 3.880s | 0.97x |
| 1e15 | 4.47s | 4.518s | 0.99x |
| 1e16 | 5.18s | 5.210s | 0.99x |
| 1e17 | 6.10s | 6.103s | 1.00x |
| 1e18 | 7.81s | 7.894s | 0.99x |

## Intel Core i5-1235U

Intel Core i5-1235U (Alder Lake, 2022), 8 GB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.41s | 0.452s | 0.91x |
| 1e11 | 5.66s | 6.502s | 0.87x |
| 1e12 | 71.92s | 80.164s | 0.90x |
| 1e13 | 916.24s | 973.433s | 0.94x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 10.09s | 10.643s | 0.95x |
| 1e14 | 12.22s | 12.510s | 0.98x |
| 1e15 | 14.11s | 14.555s | 0.97x |
| 1e16 | 16.17s | 16.763s | 0.96x |
| 1e17 | 18.61s | 19.170s | 0.97x |
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
| 1e10 | 0.45s | 0.438s | 1.03x |
| 1e11 | 5.64s | 5.547s | 1.02x |
| 1e12 | 71.73s | 70.597s | 1.02x |
| 1e13 | 922.09s | 913.416s | 1.01x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 10.40s | 10.376s | 1.00x |
| 1e14 | 13.39s | 13.960s | 0.96x |
| 1e15 | 15.97s | 18.213s | 0.88x |
| 1e16 | 18.52s | 21.634s | 0.86x |
| 1e17 | 20.41s | 25.583s | 0.80x |
| 1e18 | 23.32s | 28.842s | 0.81x |

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
