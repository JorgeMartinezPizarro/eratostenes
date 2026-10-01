# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve) on machines other than the i5-13500 server (its tables are in the [README](../README.md#benchmark)). One section per machine: a description, then the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), with the same thread count for the two programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster.

## Intel Core i5-11400F (dev PC)

6 cores / 12 threads, 48 KiB L1d and 512 KiB L2 per core, 12 MiB L3; WSL2 (Debian), primesieve 12.7, 12 threads. Tails: eratostenes a141dd1, 2026-10-02, best of 2.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.16s | 0.187s | 0.86x |
| 1e11 | 1.93s | 2.306s | 0.84x |
| 1e12 | 23.54s | 27.283s | 0.86x |
| 1e13 | 308.59s | 362.103s | 0.85x |
| 1e14 | 4185.20s | 5804.52s | 0.72x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.64s | 4.514s | 0.81x |
| 1e14 | 5.24s | 7.133s | 0.73x |
| 1e15 | 7.59s | 9.609s | 0.79x |
| 1e16 | 10.80s | 12.714s | 0.85x |
| 1e17 | 14.13s | 15.893s | 0.89x |
| 1e18 | 19.21s | 21.175s | 0.91x |

## Intel Xeon Emerald Rapids (claude.ai sandbox)

Intel Xeon @ 2.10GHz (family 6 model 207), 2-vCPU KVM, 1 thread per core, 48 KiB L1d and 2 MiB L2 per core, L3 reported as 260 MiB (the whole host's). primesieve 12.0, 2 threads, 2026-10-02. Count: 3, 3, 2 and 1 runs at 1e10, 1e11, 1e12 and 1e13. Tails: eratostenes 55e2808, best of 2.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.836s | 0.731s | 1.14x |
| 1e11 | 8.84s | 8.61s | 1.03x |
| 1e12 | 101.92s | 107.72s | 0.95x |
| 1e13 | 1250.8s | 1337.3s | 0.94x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 14.14s | 14.382s | 0.98x |
| 1e14 | 17.09s | 16.873s | 1.01x |
| 1e15 | 22.02s | 19.131s | 1.15x |
| 1e16 | 26.43s | 21.534s | 1.23x |
| 1e17 | 30.50s | 24.982s | 1.22x |
| 1e18 | 37.21s | 31.329s | 1.19x |

## Intel Xeon @ 2.80GHz (claude.ai sandbox)

Intel Xeon @ 2.80GHz, 2-vCPU KVM, 1 thread per core, 32 KiB L1d and 1 MiB L2 per core, 33 MiB L3. primesieve 12.0, eratostenes 55e2808, 2 threads, 2026-10-02. Count: best of 1 (primesieve: one run). Tails: best of 3.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.73s | 0.747s | 0.98x |
| 1e11 | 8.49s | 8.828s | 0.96x |
| 1e12 | 100.65s | 104.099s | 0.97x |
| 1e13 | 1381.85s | 1301.040s | 1.06x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 16.00s | 14.098s | 1.13x |
| 1e14 | 18.67s | 17.153s | 1.09x |
| 1e15 | 23.04s | 20.192s | 1.14x |
| 1e16 | 26.54s | 21.232s | 1.25x |
| 1e17 | 31.12s | 25.662s | 1.21x |
| 1e18 | 37.05s | 31.956s | 1.16x |

## Intel Core i5-1235U (laptop)

Hybrid, 2 P-cores with HT + 8 E-cores, 12 threads, 8 GB RAM. eratostenes before 81f2918, 2026-09-30, one run; no tails yet.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.36s | 0.46s | 0.78x |
| 1e11 | 4.32s | 6.00s | 0.72x |
| 1e12 | 80.98s | 96.60s | 0.84x |
| 1e13 | ~1000s | 1500s | ~0.67x |
