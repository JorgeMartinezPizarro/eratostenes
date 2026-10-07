# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve), one section per machine, newest CPU first: the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), same thread count for both programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. Each time is the mean of REPS interleaved pairs (eratostenes, primesieve, eratostenes, ...).

## Intel Xeon Emerald Rapids

Intel Xeon @ 2.10GHz (Emerald Rapids, 2023, family 6 model 207), 2 vCPU KVM (1 per core), 7.8 GiB. L1d 48 KiB and L2 2 MiB per core; L3 260 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.71s | 0.715s | 0.99x |
| 1e11 | 8.70s | 8.556s | 1.02x |
| 1e12 | 104.31s | 107.809s | 0.97x |
| 1e13 | 1210.94s | 1341.085s | 0.90x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 12.60s | 12.603s | 1.00x |
| 1e14 | 13.78s | 15.439s | 0.89x |
| 1e15 | 15.89s | 18.023s | 0.88x |
| 1e16 | 18.72s | 22.062s | 0.85x |
| 1e17 | 21.73s | 25.446s | 0.85x |
| 1e18 | 24.30s | 30.074s | 0.81x |

## Intel Core i5-13500

13th Gen Intel(R) Core(TM) i5-13500, 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB (1 instance)

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

12th Gen Intel(R) Core(TM) i5-1235U, 12 threads (2 per core); L1d 288 KiB (6 instances); L2 7.5 MiB (6 instances); L3 12 MiB (1 instance)

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

## Intel Xeon @ 2.80GHz

Intel Xeon @ 2.80GHz (Cascade Lake, 2019; the hypervisor hides the exact model), 2 vCPU KVM (1 per core), 7 GB. L1d 32 KiB and L2 1 MiB per core; L3 33 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.70s | 0.751s | 0.93x |
| 1e11 | 8.09s | 8.155s | 0.99x |
| 1e12 | 102.23s | 101.929s | 1.00x |
| 1e13 | 1245.53s | 1281.035s | 0.97x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 15.01s | 14.209s | 1.06x |
| 1e14 | 17.06s | 16.835s | 1.01x |
| 1e15 | 19.31s | 21.109s | 0.91x |
| 1e16 | 22.08s | 22.235s | 0.99x |
| 1e17 | 24.69s | 26.471s | 0.93x |
| 1e18 | 28.30s | 33.352s | 0.85x |

## Intel Core i5-3470

Intel(R) Core(TM) i5-3470 CPU @ 3.20GHz, 4 threads (1 per core); L1d 128 KiB (4 instances); L2 1 MiB (4 instances); L3 6 MiB (1 instance)

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

Intel Core i7-620M (Arrandale, 2010), 8 GB.

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
