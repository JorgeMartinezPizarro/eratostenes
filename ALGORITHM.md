## How eratostenes works

Eratostenes works as a 5-step system, applied to get as efficient a sieve as possible:

1. Segmentation — problem: memory. A flat sieve needs O(N) bits; for large N that doesn't fit in RAM. [1,N] is chopped into small, fixed windows, reusing the same array; only the primes ≤ sqrt(N) need to be known upfront (base_sieve.hpp, a normal flat sieve over an already-small range). Each base prime keeps track of "where it was" (k, phase) between windows — O(1) per prime, not O(range). → segment_sieve.hpp.

2. Wheel — problem: redundant candidates. Out of every 30 numbers, only 8 are coprime with 2·3·5 — the other 22 are trivial multiples of 2, 3, or 5. Representing only those 8 residues per block of 30 shrinks the array by ~73% before sieving even starts. Fixed at compile time (WHEEL_MOD=30) because going bigger (mod 210, 2310) grows the per-prime tables faster than it saves on marking. → wheel.hpp.

3. Parallelism — problem: a single thread is slow. The full range is split into many more chunks than threads (CHUNKS_PER_THREAD=16), handed out from a shared queue (not one fixed chunk per thread), because the work isn't uniform: chunks near the end of the range have far more active primes per segment than chunks near the start. Two passes for text mode (count bytes, then write) so each thread can write in parallel without overlapping or merging afterward; .db mode avoids even that duplication with a single writer thread draining a queue. → main.cpp::run_parallel_chunks.

4. Presieve — problem: the smallest primes hit every segment, without exception. The "sparse bucket" (item 5) doesn't help with 7, 11, 13... because their period is much smaller than the segment — they always land a hit inside it. Instead of marking them every time, the periodic pattern of several small prime groups is precomputed (7,23,37 / 11,19,31 / ... up to 163) and combined with OR when filling the segment — a block copy instead of a per-prime marking loop. The cost is per group (fixed), not per table, so grouping well matters more than covering more primes (measured: extending past 163 doesn't pay off on this machine). → presieve.hpp.

5. Cache — a cross-cutting concern, not one more phase. Two parameters auto-tune to the real detected hardware (/sys/.../cache), not a fixed value:
- Segment width ≤ half the detected L2, so each thread's working array fits in cache even while sharing L2 with its hyperthread sibling — measured that removing this cap makes things worse, not better.
- Dense base-prime table budget ≤ half the detected L3, so that table (shared across threads) stays cache-resident.
