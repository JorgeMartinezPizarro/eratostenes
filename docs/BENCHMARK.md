# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve), one section per machine, newest CPU first: the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), same thread count for both programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. Each time is the mean of REPS interleaved pairs (eratostenes, primesieve, eratostenes, ...).

## Intel Core i5-13500

Intel Core i5-13500 (Raptor Lake, 2023), 64 GB. 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.123s | 0.139s | 0.88x |
| 1e11 | 1.541s | 1.846s | 0.83x |
| 1e12 | 18.920s | 21.877s | 0.86x |
| 1e13 | 236.621s | 253.611s | 0.93x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 2.579s | 2.760s | 0.93x |
| 1e14 | 3.067s | 3.220s | 0.95x |
| 1e15 | 3.560s | 3.877s | 0.92x |
| 1e16 | 4.168s | 4.556s | 0.91x |
| 1e17 | 4.933s | 5.506s | 0.90x |
| 1e18 | 6.228s | 7.103s | 0.88x |

## Intel Core i5-1235U

Intel Core i5-1235U (Alder Lake, 2022), 8 GB. 12 threads (2 per core); L1d 352 KiB (10 instances); L2 6.5 MiB (4 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.223s | 0.316s | 0.71x |
| 1e11 | 3.298s | 4.441s | 0.74x |
| 1e12 | 43.662s | 54.813s | 0.80x |
| 1e13 | 559.606s | 660.002s | 0.85x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 6.195s | 7.149s | 0.87x |
| 1e14 | 7.645s | 8.663s | 0.88x |
| 1e15 | 9.159s | 10.456s | 0.88x |
| 1e16 | 10.739s | 12.448s | 0.86x |
| 1e17 | 12.722s | 15.254s | 0.83x |
| 1e18 | 15.889s | 19.895s | 0.80x |

## Intel Core i5-11400F

Intel Core i5-11400F (Rocket Lake, 2021), 32 GB. 12 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.151s | 0.186s | 0.81x |
| 1e11 | 1.849s | 2.304s | 0.80x |
| 1e12 | 22.896s | 27.290s | 0.84x |
| 1e13 | 297.804s | 328.347s | 0.91x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.377s | 3.625s | 0.93x |
| 1e14 | 4.115s | 4.515s | 0.91x |
| 1e15 | 5.121s | 5.899s | 0.87x |
| 1e16 | 6.062s | 7.086s | 0.86x |
| 1e17 | 7.263s | 8.587s | 0.85x |
| 1e18 | 9.827s | 11.241s | 0.87x |

## Intel Core i7-4770HQ

Intel Core i7-4770HQ (Haswell, 2014), 16 GB. 8 threads (2 per core); L1d 128 KiB (4 instances); L2 1 MiB (4 instances); L3 6 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.510s | 0.809s | 0.63x |
| 1e11 | 6.289s | 9.692s | 0.65x |
| 1e12 | 78.131s | 111.033s | 0.70x |
| 1e13 | 946.658s | 1294.678s | 0.73x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 10.363s | 13.826s | 0.75x |
| 1e14 | 12.148s | 15.612s | 0.78x |
| 1e15 | 13.948s | 17.092s | 0.82x |
| 1e16 | 15.949s | 17.892s | 0.89x |
| 1e17 | 18.202s | 20.033s | 0.91x |
| 1e18 | 21.661s | 23.633s | 0.92x |

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
| 1e10 | 1.069s | 1.560s | 0.69x |
| 1e11 | 13.283s | 18.671s | 0.71x |
| 1e12 | 174.205s | 218.731s | 0.80x |
| 1e13 | 2291.869s | 2585.214s | 0.89x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 25.759s | 28.595s | 0.90x |
| 1e14 | 30.155s | 33.103s | 0.91x |
| 1e15 | 35.166s | 37.035s | 0.95x |
| 1e16 | 39.896s | 41.098s | 0.97x |
| 1e17 | 45.086s | 46.744s | 0.96x |
| 1e18 | 52.242s | 54.118s | 0.97x |
