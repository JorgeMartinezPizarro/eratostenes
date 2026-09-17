# Benchmark notes: finding the optimal wheel and segment width

Reference measurements for **Intel Core i5-11400F** (6 cores / 12 logical
threads, L3 = 12 MB), measuring `pi(N)` with `--count-only` (no disk I/O,
to isolate CPU/cache work from writing). This file is the lab notebook:
the full exploration, including hypotheses that turned out wrong. For the
short version, see the main [README](README.md).

## Wheel size vs L3: results by wheel, before bucket sieve

Methodology: for each N, the four candidate wheels (`2,3` / `2,3,5` /
`2,3,5,7` / `2,3,5,7,11`) were tested by changing `WHEEL_PRIMES` in
`src/wheel.hpp` and rebuilding (`make clean && make`) between each one.
`pi(N)` matched the known reference value in every case. These numbers
predate the bucket sieve (see further down); the engine is faster today for
all four wheels, but the relative ordering between wheels -- the
interesting part of this table -- doesn't change except where noted.

| N | mod 6 | mod 30 | mod 210 | mod 2310 | winner |
|---|---:|---:|---:|---:|---|
| 10^10 | 1.51s | 1.50s | **1.01s** | 1.52s | mod 210 |
| 10^11 | 21.01s | 18.01s | **16.51s** | 35.56s | mod 210 |
| 10^12 | 352.10s | **303.10s** | 571.78s | 970.67s | mod 30 |

pi(10^10) = 455,052,511, pi(10^11) = 4,118,054,813, pi(10^12) =
37,607,912,018 -- all three matched the known reference value for every
wheel at every N (confirms that changing the wheel never changes the
result, only the speed).

### Why the winner changes: table size vs L3

Each base prime needs a jump table of `phi(wheel)` entries (`uint32_t`, 4
bytes each) to mark its multiples without dividing in the hot loop. That
table, times the number of base primes (`pi(sqrt(N))`), is what needs to
fit in the L3 shared by all 12 threads for the bottleneck to stay CPU-bound
instead of memory-bandwidth-bound:

| N | pi(sqrt(N)) | mod 6 | mod 30 | mod 210 | mod 2310 |
|---|---:|---:|---:|---:|---:|
| 10^10 | 9,592 | 154KB | 384KB | 1.9MB | 18.5MB |
| 10^11 | 27,184 | 435KB | 1.1MB | 5.4MB | 52.4MB |
| 10^12 | 78,498 | 1.3MB | 3.1MB | 15.7MB | 151MB |

The crossover between "mod 210 wins" and "mod 30 wins" falls right between
10^11 and 10^12 -- exactly where mod 210's table goes from 5.4MB (fits with
room to spare) to 15.7MB (31% over the L3 budget).

## An abandoned approach: per-prime persistent cursor

Before the bucket sieve, a naive segmented sieve re-derives, for every
(base prime, segment) pair, "where is this prime's first multiple in this
segment" via a division. A persistent cursor (carrying each prime's wheel
position forward from one segment to the next instead of re-deriving it)
was tried to remove that division. Measured at N=10^12 with the mod-2310
wheel, it came out *slower* (1102s vs 970.67s): the real bottleneck at that
scale was memory bandwidth for reading the jump table, not the divisions,
and the cursor added its own per-thread memory traffic without addressing
that. The lesson (profile before optimizing what you *assume* is the
bottleneck) led directly to the bucket sieve below.

## Bucket sieve: does it favor bigger wheels?

The pre-bucket engine walks **every** active base prime on **every**
segment, whether it has a multiple to mark there or not -- for a large
prime (step comparable to or bigger than the segment), most of those visits
mark nothing, and the check still costs something. The bucket sieve
schedules each prime into the "bucket" of the future segment where its next
multiple falls, so a segment only processes the primes that actually have
work there.

Hypothesis before measuring: since this removes the cost of "visiting with
nothing to mark" -- which penalized big wheels more, since they have bigger
jump-table rows to fetch per visit -- maybe big wheels (mod 2310) would
become competitive again at N where they used to lose badly. Measured at
N=10^12 (same hardware, 12 threads):

| wheel | without bucket sieve | with bucket sieve | speedup |
|---|---:|---:|---:|
| mod 30 | 303.10s | **275.27s** | 1.10x |
| mod 2310 | 970.67s | 599.99s | 1.62x |

**The hypothesis was only partly right.** mod 2310 improves much more in
relative terms (1.62x vs 1.10x) -- confirming the bucket sieve helps bigger
wheels more, as expected. But mod 30 still wins clearly in absolute terms:
mod 2310 goes from 3.20x slower to 2.18x slower -- it closes the gap, it
doesn't flip the outcome.

Why: the bucket sieve removes the cost of visiting with no work, but it
doesn't shrink the row you have to fetch when a visit *does* have work --
that row is always `phi(wheel)` entries (480 for mod 2310, ~30 cache lines;
8 for mod 30, a single cache line), regardless of whether that particular
visit marks 1 bit or 10. The original asymmetry (adding a prime to the
wheel multiplies the table by `(p-1)` but only cuts the work by `(p-1)/p`)
is still there: the bucket sieve removes a cost that was roughly
wheel-independent (empty visits), not the one that scales with wheel size
(row size per useful visit). **A small wheel is still better**, with or
without the bucket sieve.

The effect also depends on whether the wheel was cache-bound to begin with.
For `2,3` and `2,3,5` (tables that always fit in L3, with or without the
bucket sieve) the improvement is modest and doesn't grow clearly with N:

| N | mod 6, no bucket | mod 6, bucket | mod 30, no bucket | mod 30, bucket |
|---|---:|---:|---:|---:|
| 10^10 | 1.51s | 1.50s | 1.50s | **1.01s** |
| 10^11 | 21.01s | 19.02s | 18.01s | **17.53s** |
| 10^12 | 352.10s | 306.90s | 303.10s | **275.27s** |

Nothing monotonic there (1.49x, 1.03x, 1.10x speedup for mod 30 at 10^10,
10^11, 10^12 respectively). The "improves with N" effect that *does* show
up clearly is specific to cache-bound wheels (mod 2310 above), where the
problem the bucket sieve fixes gets worse with N; for mod 6 and mod 30,
whose table always fit, that problem never existed at this scale.

## Segment width: the biggest win, and the simplest

After the bucket sieve, `--segment-width` (`-s`) was still at the value
inherited from the original version (`262144`), never re-evaluated for the
new engine. It should have been. Every call to `sieve_and_emit` pays a
roughly fixed cost per segment (look up the bucket, check the activation
pointer, set up extraction) independent of how wide that segment is -- so a
wider segment spreads that fixed cost over more useful work, as long as
each thread's bit array (proportional to `-s`) still fits its cache. Full
sweep at N=10^11 (mod 30, bucket sieve, 12 threads):

| `-s` | time | | `-s` | time |
|---:|---:|---|---:|---:|
| 16,384 | 43.58s | | 2,097,152 | 8.01s |
| 32,768 | 35.56s | | 3,145,728 | 8.01s |
| 65,536 | 29.55s | | **4,194,304** | **7.51s** |
| 131,072 | 23.04s | | 5,242,880 | 7.51s |
| 262,144 (old default) | 16.53s | | 6,291,456 | 7.51s |
| 524,288 | 12.52s | | 8,388,608 | 8.01s |
| 1,048,576 | 9.51s | | 16,777,216 | 9.01s |
| | | | 33,554,432 | 30.04s |
| | | | 67,108,864 | 165.75s |

Flat plateau between 4,194,304 and 6,291,456 (7.51s), degrading past
~8,388,608, and a sharp cliff past 33,554,432 -- that's where each thread's
bit array (proportional to `-s`) stops fitting L2/L3 and the same
memory-bandwidth problem as an oversized wheel kicks in. `4194304`
(`1<<22`) is now the default in `src/arg_parser.hpp`.

The effect grows with N (more total segments = more fixed cost to
amortize), the same shape as the bucket sieve's benefit for cache-bound
wheels, but this one applies to **any** wheel:

| N | old `-s` (262144) | new `-s` (4194304) | speedup |
|---|---:|---:|---:|
| 10^10 | 1.00s | 1.00s | ~none (too few total segments) |
| 10^11 | 16.53s | **7.51s** | 2.20x |
| 10^12 | 275.27s | **89.54s** | 3.07x |

## Cumulative result

From this project's first version (odds-only wheel, no bucket sieve, no
segment-width tuning) to the current one, N=10^12, same hardware:

| version | time | cumulative speedup |
|---|---:|---:|
| mod 2310 ("obvious" wheel, first version) | 970.67s | 1.00x |
| mod 30 (the right wheel for this N) | 303.10s | 3.20x |
| + bucket sieve | 275.27s | 3.53x |
| + tuned `-s` (4194304) | **89.54s** | **10.84x** |

## Final clean sweep, current engine

All the tables above mix engine versions (some predate the bucket sieve,
some predate the `-s` tuning). This is a single sweep of `2,3` / `2,3,5` /
`2,3,5,7` with today's engine (bucket sieve + power-of-2 branchless phase
wraparound + `-s`=4194304 default) at 10^10/10^11/10^12, same hardware:

| N | mod 6 | mod 30 | mod 210 | winner |
|---|---:|---:|---:|---|
| 10^10 | 1.00s | 1.01s | 1.01s | tie (too few segments to tell apart) |
| 10^11 | 10.51s | ~8.0s (7.52-8.51s across repeats) | **7.02s** | mod 210 |
| 10^12 | 123.58s | 89.54-100.05s across repeats | 142.60s | **mod 30** |

`2,3,5,7,11` (mod 2310) wasn't re-run here: it already loses badly in every
earlier table at every N tested, and nothing about the bucket sieve or
segment-width tuning changes the mechanism that makes it lose (see
"Bucket sieve" above) -- there was no reason to expect a different
ranking. `pi(N)` matched the known value in all nine of the runs above.

Two things stand out. First, mod 30's own N=10^12 time varies noticeably
run to run (89.54s and 100.05s, ~11% apart) on otherwise identical
conditions -- background system noise, not a real change; treat single
data points in this file as +-10% rather than exact. Second, the ranking
is unchanged from the original (pre-bucket, pre-`-s`-tuning) wheel
comparison: mod 210 wins at 10^10-10^11, mod 30 takes over at 10^12,
because that ranking is driven by table size vs L3 (see "Why the winner
changes" above), and neither the bucket sieve nor the segment-width tuning
touches table size. `WHEEL_PRIMES` stays at `{2,3,5}` (mod 30) as the
default in `src/wheel.hpp`, since 10^12-and-up is this project's main
target range.

## External reference point

On an Intel i5-13500 server (14 cores / 20 threads, bigger L3), with
`WHEEL_PRIMES = {2,3,5}` (mod 30), N=10^12 has been counted in:

| engine | time |
|---|---:|
| pre-`-s`-tuning | 211.06s |
| current (bucket sieve + branchless + tuned `-s`) | **71s** |

For reference, a JavaScript implementation by gordonBGood took ~402s on
the same hardware. With `{2,3,5,7,11}` (mod 2310, the "obvious" wheel --
more primes removed should mean fewer candidates) the pre-`-s`-tuning
engine took 673.36s on that server: slower than the JS version, until it
became clear the problem wasn't the marking algorithm but the table's size
versus cache.

Separately, a same-machine N=10^13 attempt gave ~7500s, but with two sieve
processes running concurrently by accident (contending for the same 20
threads and cache) -- not a clean measurement. A solo run is needed for a
real N=10^13 number.

## How to reproduce

```
# Edit src/wheel.hpp: uncomment the desired wheel (WHEEL_PRIMES line), comment out the rest
make clean && make
./eratostenes -n 10000000000 -t 12 --count-only    # 1e10
./eratostenes -n 100000000000 -t 12 --count-only   # 1e11
./eratostenes -n 1t -t 12 --count-only             # 1e12
```

`-s` no longer needs to be passed to reproduce the best times: the default
(`4194304`) is already the measured optimum on this machine. The wheel
comparison tables above were measured *before* tuning `-s`, with the old
default (`262144`) -- see the segment-width section for that effect in
isolation.

Always use `--count-only` from 10^10 upward: the equivalent text file
already weighs several GB at 10^11 and hundreds of GB at 10^12+.
