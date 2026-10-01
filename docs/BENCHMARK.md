# Benchmarks

Timings of `eratostenes` against [primesieve](https://github.com/kimwalisch/primesieve) on every machine it has run on, oldest data kept as history. Ratio = eratostenes / primesieve: below 1 means eratostenes is faster. The i5-13500 server table is also in the [README](../README.md#benchmark).

## How to measure

```sh
REPS=2 make benchmark         # pi(N) for N = 1e10..1e13, plus the .db I/O sweep
REPS=2 make benchmark-tails   # last 1e11 numbers below 1e15, 1e16 and 1e17
```

Both print the machine's identity above the table (`scripts/machine_info.sh`: CPU, threads per core, caches, primesieve version, commit, date). Paste the output as is under the machine's section and add a row to the summary.

- **Same thread count** for both programs (`THREADS`, default `nproc`), and nothing else running on the machine.
- **Tails** (`benchmark-tails`): `eratostenes N --start N-1e11` vs `primesieve N-1e11 N -c`. Every window is 1e11 wide, so a run costs about the same at every height; what changes is how many base primes are active and in which tier. The two counts are checked against each other. A tail is not a full run: fixed costs (the single-threaded base-prime sieve up to sqrt(N), activating every base prime once per chunk) weigh more in a 1e11 window than across a full range, and more so at 1e17 (16.8M sparse primes).
- **Cloud VMs change CPU between sessions**: the claude.ai sandbox below was an Emerald Rapids one day and a 32 KiB-L1d / 1 MiB-L2 Xeon the next. Only compare rows with the same CPU line.
- **primesieve versions differ** (12.7 on the dev PC, 12.0 from Ubuntu on the sandbox); the version is part of the header.

## Summary

Ratios only; see each machine's section for times, commits and caveats.

| machine | threads | 1e10 | 1e11 | 1e12 | 1e13 | 1e14 | tail 1e15 | tail 1e16 | tail 1e17 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| i5-13500 (server) | 20 | 0.94x | 0.90x | 0.89x | 0.96x | 1.01x | - | - | - |
| i5-11400F (dev PC) | 12 | 0.86x | 0.84x | 0.86x | 0.85x | 0.72x | 0.82x | 0.93x | 1.20x |
| i5-11400F (dev PC) | 2 | - | - | - | - | - | 1.21x | - | - |
| i5-1235U (laptop) | 12 | 0.78x | 0.72x | 0.84x | ~0.67x | - | - | - | - |
| Xeon Emerald Rapids (sandbox, older binary) | 2 | 1.25x | 1.12x | 1.06x | 1.06x | - | - | - | - |
| Xeon, CPU not recorded (sandbox) | 2 | 1.03x | 1.05x | 0.97x | 0.93x | - | - | - | - |
| Xeon 32K L1d / 1M L2 (sandbox) | 2 | - | 1.02x | - | - | - | 1.06x | - | - |

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

Tails: pending (`make docker-benchmark-tails`).

## Intel Core i5-11400F (dev PC)

6 cores / 12 threads, 48 KiB L1d and 512 KiB L2 per core, 12 MiB L3. WSL2 (Debian), primesieve 12.7.

| N | eratostenes | primesieve | ratio |
|---|---:|---:|---:|
| 1e10 | 0.16s | 0.187s | 0.86x |
| 1e11 | 1.93s | 2.306s | 0.84x |
| 1e12 | 23.54s | 27.283s | 0.86x |
| 1e13 | 308.59s | 362.103s | 0.85x |
| 1e14 | 4185.20s | 5804.52s | 0.72x |

```
11th Gen Intel(R) Core(TM) i5-11400F @ 2.60GHz, 12 threads (2 per core); L1d 288 KiB (6 instances); L2 3 MiB (6 instances); L3 12 MiB (1 instance)
primesieve 12.7; eratostenes c892d86-dirty; 2026-10-01; best of 2
```

| N | tail | primes | eratostenes | primesieve | ratio |
|---|---|---:|---:|---:|---:|
| 1e15 | last 1e11 (0.01%) | 2,895,324,362 | 8.07s | 9.848s | 0.82x |
| 1e16 | last 1e11 (0.001%) | 2,714,317,775 | 11.92s | 12.832s | 0.93x |
| 1e17 | last 1e11 (0.0001%) | 2,554,661,982 | 19.13s | 15.985s | 1.20x |

The 1e15 tail by thread count (2026-10-01, same binary, best of 2, run order era/ps/ps/era):

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
