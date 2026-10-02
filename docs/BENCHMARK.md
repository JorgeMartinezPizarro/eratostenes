# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve) on machines other than the i5-13500 server (its tables are in the [README](../README.md#benchmark)). One section per machine: a description, then the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), with the same thread count for the two programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster.

## Intel Core i5-13500 

Intel Core i5-13500, [primesieve](https://github.com/kimwalisch/primesieve) alongside it for reference:

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.15s | 0.137s | 1.09x |
| 1e11 | 1.49s | 1.656s | 0.91x |
| 1e12 | 20.93s | 23.724s | 0.92x |
| 1e13 | 285.32s | 292.763s | 0.97x |
| 1e14 | 3392.08s | 3349.907s | 1.01x |
| 1e15 | 40976.93s | - | - |

The last 1e11 numbers below N (`make benchmark-tails`):

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 2.57s | 2.827s | 0.91x |
| 1e14 | 3.81s | 3.872s | 0.98x |
| 1e15 | 4.58s | 4.529s | 1.01x |
| 1e16 | 5.52s | 5.234s | 1.05x |
| 1e17 | 6.49s | 6.272s | 1.03x |
| 1e18 | 8.38s | 7.969s | 1.05x |

## Intel Core i5-11400F

6 cores / 12 threads, 48 KiB L1d and 512 KiB L2 per core, 12 MiB L3; WSL2 (Debian), primesieve 12.7, 12 threads.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.16s | 0.187s | 0.86x |
| 1e11 | 1.89s | 2.303s | 0.82x |
| 1e12 | 23.65s | 27.224s | 0.87x |
| 1e13 | 308.81s | 363.157s | 0.85x |
| 1e14 | 4185.20s | 5804.52s | 0.72x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.44s | 4.318s | 0.80x |
| 1e14 | 5.05s | 6.937s | 0.73x |
| 1e15 | 7.09s | 9.580s | 0.74x |
| 1e16 | 9.65s | 12.565s | 0.77x |
| 1e17 | 12.84s | 15.819s | 0.81x |
| 1e18 | 18.28s | 21.089s | 0.87x |

## Intel Xeon Emerald Rapids (claude.ai sandbox)

Intel Xeon @ 2.10GHz (family 6 model 207), 2-vCPU KVM, 1 thread per core, 48 KiB L1d and 2 MiB L2 per core, L3 reported as 260 MiB (the whole host's, shared with other tenants: the same binary's 1e18 tail ranged from 28 s to 41 s across three hosts of this CPU on one day, primesieve's from 28 s to 37 s, so only ratios from one host are comparable); Ubuntu 24.04, g++ 13.3, primesieve 12.0, 2 threads.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.84s | 0.774s | 1.09x |
| 1e11 | 8.71s | 8.515s | 1.02x |
| 1e12 | 102.45s | 106.910s | 0.96x |
| 1e13 | 1264.79s | 1324.144s | 0.96x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 12.69s | 13.596s | 0.93x |
| 1e14 | 15.69s | 15.830s | 0.99x |
| 1e15 | 18.61s | 19.924s | 0.93x |
| 1e16 | 21.73s | 20.948s | 1.04x |
| 1e17 | 24.86s | 24.139s | 1.03x |
| 1e18 | 28.31s | 27.915s | 1.01x |

## Intel Xeon @ 2.80GHz (claude.ai sandbox)

Intel Xeon @ 2.80GHz (model hidden by the hypervisor), 2-vCPU KVM, 1 thread per core, 32 KiB L1d and 1 MiB L2 per core, 33 MiB L3; primesieve 12.0, 2 threads.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.80s | 0.869s | 0.92x |
| 1e11 | 9.32s | 9.739s | 0.96x |
| 1e12 | 114.25s | 119.365s | 0.96x |
| 1e13 | 1502.30s | 1478.500s | 1.02x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 17.50s | 16.139s | 1.08x |
| 1e14 | 22.13s | 19.954s | 1.11x |
| 1e15 | 28.03s | 22.108s | 1.27x |
| 1e16 | 33.23s | 26.022s | 1.28x |
| 1e17 | 40.58s | 32.133s | 1.26x |
| 1e18 | 49.04s | 41.830s | 1.17x |

## Intel Core i5-1235U (laptop)

Hybrid, 2 P-cores with HT + 8 E-cores, 12 threads, 8 GB RAM (the 1e18 tail needs ~0.4 GB per thread in each program and doesn't fit).

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