# Benchmarks

Timings of `eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve) on every machine it has run on, oldest data kept as history. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. The i5-13500 server table is also in the [README](../README.md#benchmark).

## How to measure

```sh
REPS=2 make benchmark         # pi(N) for N = 1e10..1e13, plus the .db I/O sweep
REPS=2 make benchmark-tails   # last 1e11 numbers below 1e14, 1e15, ..., 1e18 (NS=1e19 etc. up to 2^64 - 2^32*16)
```

Both print the machine's identity above the table (`scripts/machine_info.sh`: CPU, threads per core, caches, primesieve version, commit, date). Paste the output as is under the machine's section and add a row to the summary.

- **Same thread count** for both programs (`THREADS`, default `nproc`), and nothing else running on the machine.
- **Tails** (`benchmark-tails`): `eratostenes N --start N-1e11` vs `primesieve N-1e11 N -c`. Every window is 1e11 wide, so a run costs about the same at every height; what changes is how many base primes are active and in which tier. The two counts are checked against each other. A tail is not a full run: fixed costs (the base-prime sieve up to sqrt(N), activating every base prime once per thread and steal) weigh more in a 1e11 window than across a full range. Tails measured before the 2026-10-01 startup fix ([RESEARCH.md](RESEARCH.md#top-of-range-tails-startup-costs-that-grow-with-sqrtn-kept-2026-10-01)) lose time to them from 1e16 up: 1.76x -> 0.96x at 1e18 on the dev PC.
- **Cloud VMs change CPU between sessions**: the claude.ai sandbox below was an Emerald Rapids one day and a 32 KiB-L1d / 1 MiB-L2 Xeon the next. Only compare rows with the same CPU line.
- **primesieve versions differ** (12.7 on the dev PC, 12.0 from Ubuntu on the sandbox); the version is part of the header.

## Summary

Ratios only; see each machine's section for times, commits and caveats.

Full runs, pi(N):

| machine | threads | 1e10 | 1e11 | 1e12 | 1e13 | 1e14 |
|---|---:|---:|---:|---:|---:|---:|
| i5-13500 (server) | 20 | 0.94x | 0.90x | 0.89x | 0.96x | 1.01x |
| i5-11400F (dev PC) | 12 | 0.86x | 0.84x | 0.86x | 0.85x | 0.72x |
| i5-1235U (laptop) | 12 | 0.78x | 0.72x | 0.84x | ~0.67x | - |
| Xeon Emerald Rapids (sandbox, older binary) | 2 | 1.25x | 1.12x | 1.06x | 1.06x | - |
| Xeon, CPU not recorded (sandbox) | 2 | 1.03x | 1.05x | 0.97x | 0.93x | - |
| Xeon 32K L1d / 1M L2 (sandbox) | 2 | - | 1.02x | - | - | - |

Tails, last 1e11 below N:

| machine | threads | binary | 1e14 | 1e15 | 1e16 | 1e17 | 1e18 | 1e19 |
|---|---:|---|---:|---:|---:|---:|---:|---:|
| i5-13500 (server) | 20 | before startup fix | - | 0.95x | 1.17x | 1.49x | 1.92x | - |
| i5-13500 (server) | 20 | 0719d5b | 0.95x | 1.03x | 1.06x | 1.02x | 1.13x | - |
| i5-13500 (server) | 20 | 0719d5b, 2nd run | 0.99x | 1.08x | 1.09x | 1.09x | 1.07x | - |
| i5-11400F (dev PC) | 12 | before startup fix | - | 0.83x | 0.91x | 1.13x | 1.76x | - |
| i5-11400F (dev PC) | 12 | startup fix | 0.77x | 0.89x | 0.87x | 0.89x | 0.96x | - |
| i5-11400F (dev PC) | 2 | before startup fix | - | 1.21x | - | - | - | - |
| i5-11400F (dev PC) | 2 | 0719d5b | 1.19x | 1.25x | 1.26x | 1.23x | 1.27x | - |
| i5-11400F (dev PC) | 6 | runs + steals | - | - | - | - | - | 1.05x |
| Xeon 32K L1d / 1M L2 (sandbox) | 2 | fd9f8e5 | - | 1.06x | - | - | - | - |

## Intel Core i5-13500 (server)

Hybrid: 6 P-cores with HT (48 KiB L1d, 1.25 MiB L2 each) + 8 E-cores (32 KiB L1d, 2 MiB L2 per 4-core cluster), 24 MiB L3, 20 threads. Native Linux, run through Docker (`make docker-benchmark`).

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.128s | 0.136s | 0.94x |
| 1e11 | 1.48s | 1.641s | 0.90x |
| 1e12 | 21.03s | 23.625s | 0.89x |
| 1e13 | 280.81s | 292.554s | 0.96x |
| 1e14 | 3392.08s | 3349.907s | 1.01x |
| 1e15 | 40976.93s | - | - |

Tails, `make docker-benchmark-tails` with `NS="1e15 1e16 1e17 1e18"`, before the startup fix (Docker image, no `.git` inside: commit not recorded; primesieve 11.0 from Debian):

```
13th Gen Intel(R) Core(TM) i5-13500, 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB (1 instance)
primesieve 11.0; eratostenes ?; 2026-10-01; best of 1
```

| N | tail | primes | eratostenes | primesieve | ratio |
|---|---|---:|---:|---:|---:|
| 1e15 | last 1e11 (0.01%) | 2,895,324,362 | 3.87s | 4.078s | 0.95x |
| 1e16 | last 1e11 (0.001%) | 2,714,317,775 | 6.03s | 5.136s | 1.17x |
| 1e17 | last 1e11 (0.0001%) | 2,554,661,982 | 9.06s | 6.085s | 1.49x |
| 1e18 | last 1e11 (1e-05%) | 2,412,705,071 | 15.22s | 7.937s | 1.92x |

After the startup fix (0719d5b, same image setup):

```
13th Gen Intel(R) Core(TM) i5-13500, 20 threads (2 per core); L1d 544 KiB (14 instances); L2 11.5 MiB (8 instances); L3 24 MiB (1 instance)
primesieve 11.0; eratostenes ?; 2026-10-01; best of 1
```

| N | tail | primes | eratostenes | primesieve | ratio |
|---|---|---:|---:|---:|---:|
| 1e14 | last 1e11 (0.1%) | 3,102,093,076 | 3.34s | 3.508s | 0.95x |
| 1e15 | last 1e11 (0.01%) | 2,895,324,362 | 5.06s | 4.912s | 1.03x |
| 1e16 | last 1e11 (0.001%) | 2,714,317,775 | 6.24s | 5.884s | 1.06x |
| 1e17 | last 1e11 (0.0001%) | 2,554,661,982 | 7.43s | 7.294s | 1.02x |
| 1e18 | last 1e11 (1e-05%) | 2,412,705,071 | 10.78s | 9.548s | 1.13x |

Single runs; both programs ran 20-30% slower at 1e15 than in the table above. The fix leaves the server 5 chunks per thread below 1e15 (was 8) and 1 from 1e16 up, where the queue no longer balances P- and E-cores: `--debug-idle` shows 10.9% idle at the 1e18 tail and 13.5% at 1e16 (20 chunks, 20 threads). A second run of the same code (`make docker-benchmark-tails`, best of 1): 1e14 3.22s vs 3.259s (0.99x), 1e15 4.75s vs 4.409s (1.08x), 1e16 5.82s vs 5.334s (1.09x), 1e17 6.81s vs 6.269s (1.09x), 1e18 8.63s vs 8.037s (1.07x). The next change (contiguous runs per thread, the sieve carried across chunks, steals) is aimed at that idle; server numbers pending.

## Intel Core i5-11400F (dev PC)

6 cores / 12 threads, 48 KiB L1d and 512 KiB L2 per core, 12 MiB L3. WSL2 (Debian), primesieve 12.7.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.16s | 0.187s | 0.86x |
| 1e11 | 1.93s | 2.306s | 0.84x |
| 1e12 | 23.54s | 27.283s | 0.86x |
| 1e13 | 308.59s | 362.103s | 0.85x |
| 1e14 | 4185.20s | 5804.52s | 0.72x |

Tails before the startup fix (binary built from 81598ae's code; the header shows the working tree's commit, which already had the fix in source):

```
11th Gen Intel(R) Core(TM) i5-11400F @ 2.60GHz, 12 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB (1 instance)
primesieve 12.7; eratostenes 82fb442-dirty; 2026-10-01; best of 2
```

| N | tail | primes | eratostenes | primesieve | ratio |
|---|---|---:|---:|---:|---:|
| 1e15 | last 1e11 (0.01%) | 2,895,324,362 | 8.25s | 9.882s | 0.83x |
| 1e16 | last 1e11 (0.001%) | 2,714,317,775 | 11.64s | 12.796s | 0.91x |
| 1e17 | last 1e11 (0.0001%) | 2,554,661,982 | 18.48s | 16.416s | 1.13x |
| 1e18 | last 1e11 (1e-05%) | 2,412,705,071 | 37.01s | 21.021s | 1.76x |

After the startup fix (segmented base sieve, fewer chunks per thread at the top):

```
11th Gen Intel(R) Core(TM) i5-11400F @ 2.60GHz, 12 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB (1 instance)
primesieve 12.7; eratostenes 82fb442-dirty; 2026-10-01; best of 2
```

| N | tail | primes | eratostenes | primesieve | ratio |
|---|---|---:|---:|---:|---:|
| 1e14 | last 1e11 (0.1%) | 3,102,093,076 | 5.52s | 7.148s | 0.77x |
| 1e15 | last 1e11 (0.01%) | 2,895,324,362 | 8.95s | 10.012s | 0.89x |
| 1e16 | last 1e11 (0.001%) | 2,714,317,775 | 11.12s | 12.788s | 0.87x |
| 1e17 | last 1e11 (0.0001%) | 2,554,661,982 | 14.80s | 16.699s | 0.89x |
| 1e18 | last 1e11 (1e-05%) | 2,412,705,071 | 20.60s | 21.433s | 0.96x |

The 1e15 tail swings ~15% from run to run here with either binary: an interleaved A/B of the two (6 runs each) gives medians of 8.10s and 8.09s, so this table's 8.95s is one of those swings, not a regression.

The same tails with 2 threads (one per core, the regime of the 2-vCPU sandbox):

```
11th Gen Intel(R) Core(TM) i5-11400F @ 2.60GHz, 2 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB (1 instance)
primesieve 12.7; eratostenes 0719d5b; 2026-10-01; best of 2
```

| N | tail | primes | eratostenes | primesieve | ratio |
|---|---|---:|---:|---:|---:|
| 1e14 | last 1e11 (0.1%) | 3,102,093,076 | 15.15s | 12.716s | 1.19x |
| 1e15 | last 1e11 (0.01%) | 2,895,324,362 | 18.65s | 14.919s | 1.25x |
| 1e16 | last 1e11 (0.001%) | 2,714,317,775 | 21.90s | 17.363s | 1.26x |
| 1e17 | last 1e11 (0.0001%) | 2,554,661,982 | 26.84s | 21.796s | 1.23x |
| 1e18 | last 1e11 (1e-05%) | 2,412,705,071 | 33.67s | 26.473s | 1.27x |

Flat at ~1.25x from 1e14 to 1e18: with the startup fixed, what is left with one thread per core is sieving speed per thread, the same at every height.

1e19 (152M base primes, ~1.2 GB per thread in each program, so 6 threads to fit in WSL's 12 GB), with contiguous runs and steals (uncommitted at the time):

```
11th Gen Intel(R) Core(TM) i5-11400F @ 2.60GHz, 6 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB (1 instance)
primesieve 12.7; eratostenes 5b2e3c3-dirty; 2026-10-01; best of 1
```

| N | tail | primes | eratostenes | primesieve | ratio |
|---|---|---:|---:|---:|---:|
| 1e19 | last 1e11 (1e-06%) | 2,285,738,870 | 30.93s | 29.328s | 1.05x |

The 1e15 tail by thread count (2026-10-01, before the startup fix, which doesn't change this window's chunking; best of 2, run order era/ps/ps/era):

| threads | eratostenes | primesieve | ratio |
|---:|---:|---:|---:|
| 2 | 18.48s | 15.292s | 1.21x |
| 6 | 9.29s | 9.575s | 0.97x |
| 12 | 8.79s | 10.203s | 0.86x |

The lead only shows under contention: from 2 to 12 threads eratostenes speeds up 2.10x and primesieve 1.50x. With one thread per core and the L3 to themselves, primesieve is faster per thread. That is the regime of the 2-vCPU sandbox below.

## Intel Core i5-1235U (laptop)

Hybrid 2 P-cores with HT + 8 E-cores, 12 threads, 8 GB RAM, never tuned for. One `make benchmark` run on 2026-09-30, binary before 81f2918.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.36s | 0.46s | 0.78x |
| 1e11 | 4.32s | 6.00s | 0.72x |
| 1e12 | 80.98s | 96.60s | 0.84x |
| 1e13 | ~1000s | 1500s | ~0.67x |

1e13: eratostenes' time rounded by hand; primesieve ran first, with the laptop cooler.

## claude.ai sandbox (2-vCPU KVM, no SMT, primesieve 12.0)

The CPU behind it changes between sessions; each run is listed under the CPU its own log shows. All runs `-t 2`.

**Xeon Emerald Rapids** (family 6 model 207, 48 KiB L1d, 2 MiB L2, L3 reported 260 MB for the whole host), 2026-10-01, binary before 6846baf (half-L1d sub-block):

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | ~0.65s | ~0.52s | 1.25x |
| 1e11 | 7.7s | 6.9s | 1.12x |
| 1e12 | 88.0s | 83.0s | 1.06x |
| 1e13 | 1051.2s | 991.6s | 1.06x |

**CPU not recorded**, c0e63c8: 1e10 1.03x, 1e11 1.05x, 1e12 0.97x (interleaved runs, times not kept); full 1e13 1091.4s vs 1169.7s = **0.93x**, pi(1e13) correct. primesieve took 18% longer than in the Emerald Rapids session, so probably not the same CPU.

**Xeon, 32 KiB L1d / 1 MiB L2 / 33 MiB L3, 2.8 GHz** (Cascade Lake class, inferred from the startup log: whole-L1d sub-block of 32 KiB, 1 MiB segment, medium-NTA gate at 16.5 MiB of L3 per thread), fd9f8e5 (same code as c0e63c8):

| range | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e11 (mean of 3) | 8.90s | 8.73s | 1.02x |
| 1e13, last 1% (mean of 2) | 14.35s | 15.00s | 0.96x |
| 1e15, last 1e11 (best of 3) | 23.06s | 21.80s | 1.06x |

The 1e11 and 1e13 rows come from the same session as the 1e15 tail, after a restart; the CPU is assumed to be the same.
