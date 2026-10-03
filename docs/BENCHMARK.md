# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve) on machines other than the i5-13500 server (its tables are in the [README](../README.md#benchmark)). One section per machine: a description, then the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), with the same thread count for the two programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster.

## Intel Core i5-13500 

Intel Core i5-13500 (Raptor Lake, 2023), [primesieve](https://github.com/kimwalisch/primesieve) alongside it for reference:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.13s | 0.130s | 1.00x |
| 1e11 | 1.64s | 1.648s | 1.00x |
| 1e12 | 22.79s | 25.055s | 0.91x |
| 1e13 | 283.45s | 292.251s | 0.97x |
| 1e14 | 3392.08s | 3349.907s | 1.01x |
| 1e15 | 40664.78s | - | - |

The last 1e11 numbers below N (`make benchmark-tails`):

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.11s | 3.370s | 0.92x |
| 1e14 | 3.82s | 4.013s | 0.95x |
| 1e15 | 4.41s | 4.541s | 0.97x |
| 1e16 | 5.10s | 5.162s | 0.99x |
| 1e17 | 6.11s | 6.097s | 1.00x |
| 1e18 | 7.68s | 7.991s | 0.96x |

## Intel Core i5-11400F

Intel Core i5-11400F (Rocket Lake, 2021), 6 cores / 12 threads, 48 KiB L1d and 512 KiB L2 per core, 12 MiB L3; WSL2 (Debian), primesieve 12.7, 12 threads.

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

## Intel Xeon Emerald Rapids (claude.ai sandbox)

Intel Xeon @ 2.10GHz (Emerald Rapids, family 6 model 207, 2023), 2-vCPU KVM, 1 thread per core, 48 KiB L1d and 2 MiB L2 per core, L3 reported as 260 MiB (the whole host's, shared with other tenants: the same binary's 1e18 tail ranged from 28 s to 41 s across three hosts of this CPU on one day, primesieve's from 28 s to 37 s, so only ratios from one host are comparable); Ubuntu 24.04, g++ 13.3, primesieve 12.0, 2 threads.

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

## Intel Xeon @ 2.80GHz (claude.ai sandbox)

Intel Xeon @ 2.80GHz (model hidden by the hypervisor; 32 KiB L1d and 1 MiB L2 per core point at Cascade Lake, 2019), 2-vCPU KVM, 1 thread per core, 32 KiB L1d and 1 MiB L2 per core, 33 MiB L3; primesieve 12.0, 2 threads.

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

## Intel Core i5-1235U (laptop)

Intel Core i5-1235U (Alder Lake, 2022), hybrid, 2 P-cores with HT + 8 E-cores, 12 threads, 8 GB RAM (the 1e18 tail needs ~0.4 GB per thread in each program and doesn't fit).

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

## Intel Core i7-620M (2010 MacBook Pro)

Intel Core i7 M620 @ 2.67GHz (Arrandale, 2010), 2 cores with HT, 4 threads, 32 KiB L1d and 256 KiB L2 per core, 4 MiB L3, 8 GB RAM; Ubuntu, primesieve 12.12, 4 threads. Tails: last 1e10 below N instead of the standard 1e11.

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

## Intel Core i5-3470 (2012 desktop)

Intel Core i5-3470 @ 3.20GHz (Ivy Bridge, 2012), 4 cores, 4 threads, 32 KiB L1d and 256 KiB L2 per core, 6 MiB L3, 8 GB RAM; Ubuntu (live USB), primesieve 12.12, 4 threads. Single runs of both programs in both tables (the live system drifts ~15% between runs; its A/Bs are interleaved).

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
