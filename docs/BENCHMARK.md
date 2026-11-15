# Benchmarks

`eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve), one section per machine, newest CPU first: the count table (`make benchmark`, pi(N)) and the tails table (`make benchmark-tails`, the last 1e11 numbers below each N), same thread count for both programs. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. Each time is the mean of REPS interleaved pairs (eratostenes, primesieve, eratostenes, ...).

## Intel Core i5-13500

Intel Core i5-13500 (Raptor Lake, 2023), 64 GB. 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.151s | 0.160s | 0.94x |
| 1e11 | 1.840s | 2.087s | 0.88x |
| 1e12 | 22.378s | 25.067s | 0.89x |
| 1e13 | 278.806s | 293.146s | 0.95x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.053s | 3.191s | 0.96x |
| 1e14 | 3.671s | 3.742s | 0.98x |
| 1e15 | 4.238s | 4.571s | 0.93x |
| 1e16 | 4.863s | 5.215s | 0.93x |
| 1e17 | 5.705s | 6.162s | 0.93x |
| 1e18 | 7.107s | 8.009s | 0.89x |

## Intel Core i5-1235U

Intel Core i5-1235U (Alder Lake, 2022), 8 GB. 12 threads (2 per core); L1d 352 KiB (10 instances); L2 6.5 MiB (4 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.296s | 0.315s | 0.94x |
| 1e11 | 3.958s | 4.491s | 0.88x |
| 1e12 | 49.906s | 54.546s | 0.91x |
| 1e13 | 626.376s | 664.202s | 0.94x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 6.903s | 7.276s | 0.95x |
| 1e14 | 8.238s | 8.801s | 0.94x |
| 1e15 | 9.634s | 10.378s | 0.93x |
| 1e16 | 11.273s | 12.540s | 0.90x |
| 1e17 | 13.163s | 15.420s | 0.85x |
| 1e18 | 16.154s | 19.784s | 0.82x |

## Intel Core i5-11400F

Intel Core i5-11400F (Rocket Lake, 2021), 32 GB. 12 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.161s | 0.187s | 0.86x |
| 1e11 | 1.878s | 2.294s | 0.82x |
| 1e12 | 23.283s | 26.957s | 0.86x |
| 1e13 | 297.647s | 322.027s | 0.92x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 3.306s | 3.539s | 0.93x |
| 1e14 | 4.065s | 4.395s | 0.92x |
| 1e15 | 4.917s | 5.459s | 0.90x |
| 1e16 | 5.926s | 6.733s | 0.88x |
| 1e17 | 7.205s | 8.469s | 0.85x |
| 1e18 | 9.346s | 11.083s | 0.84x |

## Intel Core i7-4770HQ

Intel Core i7-4770HQ (Haswell, 2014), 16 GB. 8 threads (2 per core); L1d 128 KiB (4 instances); L2 1 MiB (4 instances); L3 6 MiB.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.550s | 0.804s | 0.68x |
| 1e11 | 6.699s | 9.689s | 0.69x |
| 1e12 | 82.498s | 110.858s | 0.74x |
| 1e13 | 985.416s | 1288.734s | 0.76x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 10.703s | 13.695s | 0.78x |
| 1e14 | 12.455s | 15.489s | 0.80x |
| 1e15 | 14.324s | 17.055s | 0.84x |
| 1e16 | 16.249s | 17.968s | 0.90x |
| 1e17 | 18.508s | 20.039s | 0.92x |
| 1e18 | 21.978s | 23.599s | 0.93x |

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
| 1e10 | 1.217s | 1.552s | 0.78x |
| 1e11 | 14.809s | 18.793s | 0.79x |
| 1e12 | 188.604s | 219.338s | 0.86x |
| 1e13 | 2408.479s | 2584.321s | 0.93x |

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e13 | 27.225s | 28.718s | 0.95x |
| 1e14 | 31.917s | 33.379s | 0.96x |
| 1e15 | 36.322s | 37.120s | 0.98x |
| 1e16 | 41.048s | 41.001s | 1.00x |
| 1e17 | 46.160s | 46.543s | 0.99x |
| 1e18 | 53.416s | 53.802s | 0.99x |
