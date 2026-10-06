#pragma once
// What main() decides once per run from N, the detected caches, the thread
// count and --tune: the segment width (six steps, in order: the base from
// cpu0's L2, the smallest per-CPU share on a hybrid, the 32 x L1d cap, the
// whole-L2 base with one thread per core, the doubling once the sparse
// tier exists, its ceiling, and the power-of-2 fixup), the sub-block, the
// tier cutoffs, the medium-tier prefetch gate, the steal threshold, and
// the base primes split into tiers (one set, or two when the early chunks
// run a narrower segment). plan_sieve() makes the decisions, print_plan()
// says what it chose and why; every rule carries its measurement in
// docs/RESEARCH.md. No sieving happens here.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>

#include "arg_parser.hpp"
#include "base_sieve.hpp"
#include "colors.hpp"
#include "cpu_cache.hpp"
#include "presieve.hpp"
#include "wheel.hpp"

// What main() decides once for a run and the workers read: sizes the tiers
// are built with and the scheduler's knobs. Set from the detected caches,
// the thread count and --tune (see main()).
struct SieveConfig {
    // Slice the small dense tier is crossed off in (SegmentSieve): half the
    // detected L1d, machine-wide not per-thread, or the whole L1d when every
    // thread has a core to itself. See
    // docs/RESEARCH.md#cache-topology-sizing-per-cpu-minimum-step-kept.
    uint64_t sub_block_bytes = 32 * 1024;
    // Medium-tier prefetchnta (erat_small.hpp::cross_off_medium) is on for a
    // chunk's tier set when it has at least this many medium primes, i.e.
    // when their state (8 bytes each) outgrows the per-thread L3 share.
    uint64_t medium_nta_min_primes = UINT64_MAX;
    // Sparse tier on the mod-2310 multiplier wheel (SegmentSieve::process_big<true>),
    // on by default; --tune big2310=0 goes back to mod-210 (A/B). See docs/RESEARCH.md.
    bool big2310 = true;
    // --debug-idle: run_parallel_chunks prints how far apart the threads finished.
    bool debug_idle = false;
    // Smallest piece (wheel indices) a worker steals from another's run in
    // run_parallel_chunks before the run has measured anything: the thief
    // pays one activation of every base prime for it, so the piece must
    // take longer to sieve than that.
    uint64_t steal_min_k = 0;
    // Sparse ring arenas as 2 MiB huge pages (SegmentSieve): with one thread
    // per core (-2.9..-3.5% on the 1e17/1e18 tails, dev PC at 2 threads),
    // not with HT pairs (+10.5% at 12). --tune huge=1|0 forces it.
    bool huge_arenas = false;
    // First wheel index the count includes: 1 (the number 1 is not prime),
    // or with --start the index of the first number >= start.
    uint64_t skip_below_k = 1;
};

struct ChunkRange {
    uint64_t low;   // first wheel index of the chunk (inclusive)
    uint64_t high;  // upper bound in wheel index (exclusive)
};

// Segment width plus the base primes split into tiers for it (see main()).
// A run has one, or two when its early chunks use a narrower segment. The
// sparse tier is a run of the base-prime bitmap, not a copy (see classify).
struct TierSet {
    uint64_t width;
    std::vector<uint64_t> small, med64, medium;
    SparsePrimes sparse;
};

// The decisions, plus what print_plan needs to explain them.
struct SievePlan {
    SieveConfig cfg;             // sub-block, prefetch gate, steal threshold, switches
    uint64_t seg_k_width = 0;    // wide segment, in wheel indices
    uint64_t small_limit = 0, med64_limit = 0, sparse_limit = 0;
    uint64_t sparse_num = 1, sparse_den = 1, sparse_den_auto = 1;
    bool sparse_l3_gate = false;   // the 1/4 came from the L3 per active thread
    bool med64_l2_gate = false;    // med64 = the whole segment, from the small-L2 rule
    bool small_l2_gate = false;    // small cutoff 1/2 of the sub-block, same rule
    bool half_l2_few_primes = false; // base at half the whole-L2 width: one per core, few base primes
    uint64_t base_count = 0;         // startup log
    uint64_t l3_per_thread = 0;    // bytes, 0 = undetected
    bool sparse_regime = false;
    TierSet wide, narrow;        // narrow: unused unless narrow_k_end > 0
    uint64_t narrow_k_end = 0;   // chunks with high <= this use `narrow`
    bool narrow_early = false;
    // For the log: what the topology said and which rules fired.
    uint64_t min_l2_share = 0;
    unsigned l1_big_cores = 0;   // physical cores with the largest L1d (0: unknown)
    bool sub_block_whole_l1d = false;
    bool whole_l2_base = false;
    uint64_t seg_l1_capped_k = 0, seg_uncapped_k = 0, seg_unrounded_k = 0;

    const TierSet& tiers_for(const ChunkRange& r) const { return r.high <= narrow_k_end ? narrow : wide; }

    // After the chunking, since a small range can leave fewer threads than
    // -t: the sub-block grows to the whole L1d when the threads that
    // actually run fit one per core with the largest L1d (see plan_sieve).
    void finish_threads(unsigned actual_threads) {
        if (l1_big_cores && actual_threads <= l1_big_cores) { cfg.sub_block_bytes *= 2; sub_block_whole_l1d = true; }
    }
};

inline SievePlan plan_sieve(Options& opt, const BasePrimes& base, uint64_t base_limit) {
    SievePlan P;
    SieveConfig& cfg = P.cfg;
    cfg.debug_idle = opt.debug_idle;
    cfg.skip_below_k = opt.start ? std::max<uint64_t>(wheel_count_upto(opt.start - 1), 1) : 1;
    cfg.big2310 = opt.big2310;

    // --segment-width is a numeric width (so the option keeps meaning the
    // same thing to the user); it's converted to a width in wheel indices
    // (WHEEL_SIZE useful numbers out of every WHEEL_MOD), rounded down to
    // whole 64-bit words: the byte-addressed dense tiers (erat_small.hpp)
    // need every segment to start on a word boundary.
    uint64_t seg_k_width = std::max<uint64_t>(64, (opt.segment_width * WHEEL_SIZE / WHEEL_MOD) / 64 * 64);

    // A core with an L2 of 256 KiB or less (i5-3470, Ivy Bridge): the proxy
    // for an old core that the small-cutoff, half-L2 and med64 rules below
    // key on. --l2-bytes counts as the L2 here.
    constexpr uint64_t SMALL_L2_BYTES = 256 * uint64_t{1024};
    const uint64_t l2_core = opt.l2_bytes_override ? opt.l2_bytes_override : detect_l2_cache_bytes();
    const bool small_l2_core = l2_core && l2_core <= SMALL_L2_BYTES;

    // small_limit's own divisor, joint-tuned with med64_limit below; default
    // 1/4 (was 1/2 before med64 existed). --tune small=a/b for sweeps on new
    // hardware without recompiling.
    // See docs/RESEARCH.md#small_limit-re-tuned-jointly-with-med64_limit-kept-2026-09-26.
    // 1/2 on a small-L2 core: the i5-3470 marks every prime above the small
    // cutoff in the med64 kernel, which misses L1 on every byte there, so
    // the band from 1/4 to 1/2 of the sub-block (4K-8K) is better off
    // L1-blocked -- -2.9% / -1.6% / -2.8% / 0 / -0.6% at the 1e11..1e15
    // tails (x3, every B below every A at four of the five). The modern
    // cores measured 1/4 as the optimum. See
    // docs/RESEARCH.md#small-cutoff-12-of-the-sub-block-on-a-256-kib-l2-core-kept-2026-10-06.
    uint64_t small_num = 1, small_den = 4;
    bool small_l2_gate = false; // startup log
    if (opt.tune_small.den) { small_num = opt.tune_small.num; small_den = opt.tune_small.den; }
    else if (small_l2_core) { small_den = 2; small_l2_gate = true; }

    // The small tier is crossed off one sub-block at a time (see
    // SegmentSieve::sieve_and_emit); sub-block = half the machine's
    // detected L1d (sub_block_from_l1_bytes). See
    // docs/RESEARCH.md#small_limit-cutoff-tuning for the divisor.
    uint64_t l1_bytes = opt.l1_bytes_override ? opt.l1_bytes_override : detect_l1d_cache_bytes();
    cfg.sub_block_bytes = sub_block_from_l1_bytes(l1_bytes);
    uint64_t small_limit = cfg.sub_block_bytes * small_num / small_den;
    unsigned l1_big_cores = 0;         // physical cores with the largest L1d (0: unknown)
    uint64_t l1_max = l1_bytes ? l1_bytes : 32 * 1024; // largest per-core L1d (segment ceiling below)

    // Hybrid P-core/E-core correction: detect_l2_cache_bytes()/
    // detect_l1d_cache_bytes() above always read cpu0, so a P-core cpu0
    // would otherwise size E-core threads for cache they don't have. The
    // segment uses the smallest per-CPU L2 share detected
    // (CpuCacheTopology), the sub-block the LARGEST per-CPU L1d -- both
    // uniformly for every thread, not per-thread (threads migrate between
    // core types at runtime) -- see
    // docs/RESEARCH.md#cache-topology-sizing-per-cpu-minimum-step-kept.
    // Skipped when the user already forced a value (-s, --l2-bytes,
    // --l1-bytes) or detection found nothing (non-Linux, sysfs unavailable).
    // Read once: the sparse cutoff below needs it too.
    const CpuCacheTopology topo = detect_cpu_cache_topology();
    // Only recompute when some CPU's share is genuinely smaller than
    // cpu0's own -- a uniform machine's minimum trivially equals cpu0's.
    if (!opt.segment_width_set && !opt.l2_bytes_override && !topo.l2_share.empty() && topo.l2_share[0]) {
        uint64_t min_l2_share = topo.l2_share[0];
        for (uint64_t s : topo.l2_share) if (s && s < min_l2_share) min_l2_share = s;
        if (min_l2_share < topo.l2_share[0]) {
            seg_k_width = seg_k_width_from_l2_bytes(min_l2_share);
            opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE; // keep the startup log's "segment=" accurate
        }
    }
    // L1d: largest, not smallest -- sizing the P-cores' sub-block for
    // the E-cores' L1d measured slower on the i5-13500, see the same
    // RESEARCH.md entry.
    if (!topo.l1_raw.empty() && topo.l1_raw[0]) {
        uint64_t max_l1_raw = topo.l1_raw[0];
        for (uint64_t s : topo.l1_raw) if (s > max_l1_raw) max_l1_raw = s;
        if (!opt.l1_bytes_override) {
            l1_max = max_l1_raw;
            if (max_l1_raw > topo.l1_raw[0]) {
                cfg.sub_block_bytes = sub_block_from_l1_bytes(max_l1_raw);
                small_limit = cfg.sub_block_bytes * small_num / small_den;
            }
        }
        // The core count below is topology, not a size: --l1-bytes forces
        // the L1d size but not how many cores there are, so the
        // one-thread-per-core rules still apply under it.
        // One thread per core: half the L1d is the per-thread share of
        // an HT pair, which no thread has to give up when there are no
        // more threads than physical cores, so the sub-block takes the
        // whole L1d. Covers machines without SMT (VMs, HT off) and -t
        // below the core count on SMT machines -- Linux spreads the
        // threads one per core, P-cores first on a hybrid (checked on
        // the i5-13500). Only cores with the largest L1d count (each
        // CPU sharing one L1d instance is 1/sharers of a core): the
        // whole P-core L1d would overflow an E-core's smaller one.
        // small_limit stays at the half-L1d value -- letting it grow
        // with the sub-block measured worse. See
        // docs/RESEARCH.md#sub-block-the-whole-l1d-when-each-thread-has-a-core-to-itself-kept-2026-10-01.
        double big_cores = 0;
        for (size_t c = 0; c < topo.l1_raw.size(); ++c)
            if (topo.l1_raw[c] == max_l1_raw && topo.l1_sharers[c] > 0) big_cores += 1.0 / topo.l1_sharers[c];
        l1_big_cores = static_cast<unsigned>(big_cores + 0.5);
        // Applied in finish_threads, against the threads that actually run.
    }

    // Smallest L2 share per hardware thread (sysfs): the whole-L2 base below,
    // the segment ceiling and the sparse cutoff further down use it.
    uint64_t min_l2_share = 0;
    for (uint64_t s : topo.l2_share)
        if (s && (min_l2_share == 0 || s < min_l2_share)) min_l2_share = s;

    // Cap on the automatic base width: 32 x L1d, whatever sysfs claims for
    // the L2. The base is half of cpu0's L2 (arg_parser.hpp), and below the
    // sparse regime nothing else bounds it. Docker Desktop runs containers
    // in its own VM (also on Linux): on a 2010 MacBook Pro (i7 M620, Ubuntu
    // host) its sysfs reports a 4 MiB L2 per vCPU (the host's L3 -- the
    // real L2 is 256 KiB) and the base came out as 2 MiB: 2.14x primesieve on the
    // last 1e10 below 1e13, against 1.07x with 1 MiB and 0.96x with
    // 512 KiB (make benchmark-mini, 2026-10-03). Every real machine
    // measured so far already sits at or under this cap (Emerald Rapids
    // 1.5 MiB = 32 x 48 KiB, Xeon 2.80 1 MiB = 32 x 32 KiB, the HT
    // machines far below), so only a lying topology reaches it. Applied
    // before sparse_regime is evaluated, so the doubling and the ceiling
    // below see the capped width. Only -s bypasses it (--l2-bytes still
    // says how big the L2 is, not that the segment may outgrow the L1d
    // bound; a wider segment on purpose is what -s is for). See
    // docs/RESEARCH.md#base-segment-capped-at-32-x-l1d-a-vm-whose-sysfs-reports-the-hosts-l3-as-l2-kept-2026-10-03.
    uint64_t seg_l1_capped_k = 0; // startup log: the width before this cap, 0 if it didn't apply
    if (!opt.segment_width_set) {
        const uint64_t cap_k = 32 * l1_max * 8 / 64 * 64;
        if (seg_k_width > cap_k) {
            seg_l1_capped_k = seg_k_width;
            seg_k_width = cap_k;
            opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE; // keep the startup log's "segment=" accurate
        }
    }

    // Once some base prime would be sparse (isqrt(N) >= seg_k_width), use
    // the whole per-thread L2 share instead of half: every medium/med64
    // prime pays a fixed cost per segment (state load/store, loop exit
    // mispredict), and from here on halving the number of segments is
    // worth more than the extra cache pressure. Below that N it isn't
    // (1e12, no sparse: +6.5% with the wider segment). Measured on the dev
    // PC: 1e13 -2.5%, 1e14 -10..-16% by slice. Auto width only -- an
    // explicit -s is left alone. See
    // docs/RESEARCH.md#segment-width-doubled-once-the-sparse-tier-exists-kept-2026-09-27.
    // Same condition (on the pre-doubling width) also gates the lowered
    // medium/sparse cutoff below.
    const bool sparse_regime = base_limit >= seg_k_width;
    const bool one_per_core = l1_big_cores && opt.threads <= l1_big_cores;
    cfg.huge_arenas = opt.huge >= 0 ? opt.huge != 0 : one_per_core;
    bool whole_l2_base = false;      // startup log
    bool half_l2_few_primes = false; // startup log
    // With few base primes the whole-L2 width below doesn't pay: halving
    // the segment doubles the per-segment visit of every med64 entry, which
    // costs little while the base primes are few, and the smaller segment
    // leaves L2 ways to the state streams (i5-3470: the whole-L2 segment
    // fills all 8 ways of every set, 1.78 G L2 misses at 1e11, 8x
    // primesieve's). The crossover is ~40K base primes (sqrt(N) ~ 500K):
    // i5-3470, `-s 3932160 --tune med64=1/1` vs whole-L2, x3: -8.7% at
    // 1e10, -4.8% at 1e11, -1.4% at 2e11, +1.2% at 3e11, +5.5% at 1e12. The
    // same half-of-the-whole-L2 width on the 2-vCPU sandboxes at 1e11 (x3):
    // Emerald Rapids 1.5 MiB -> 768 KiB -3.3% (every B below every A; 1 MiB
    // was -1.7%, so it is half of the width, not half of the L2), Xeon
    // @2.80GHz 1 MiB -> 512 KiB a tie; at 1e12 (78K base primes) both within
    // noise. The cutoff is on base primes, not on L2 size: it was gated on
    // L2 <= 256 KiB until 2026-10-06. See
    // docs/RESEARCH.md#half-the-whole-l2-width-with-few-base-primes-on-every-one-per-core-machine-kept-2026-10-06
    constexpr uint64_t HALF_L2_MAX_BASE_PRIMES = 40000;
    if (!opt.segment_width_set && sparse_regime) {
        seg_k_width *= 2;
        opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE; // keep the startup log's "segment=" accurate
    } else if (!opt.segment_width_set && one_per_core) {
        // No sparse tier and a core to itself: the base segment is the
        // thread's whole L2 share, not half of it, within 32 x L1d. The
        // medium tier pays a fixed cost per prime per segment, and here
        // every base prime is dense. 2-vCPU Xeon with 32 KiB L1d and 1 MiB
        // L2 per vCPU (no SMT), last 1e11 below 1e13: 512 KiB -> 1 MiB is
        // -8..-13% (two hosts, all runs below); on an SMT machine the share
        // is already half the L2 and nothing changes (dev PC: 256 KiB; a
        // forced 512 KiB there was neutral at 1e11, 1e12 and the 1e13 tail).
        // --l2-bytes stands in for the detected share, so the rule (and the
        // few-primes half below) can be checked with forced caches.
        // See docs/RESEARCH.md#whole-l2-base-segment-one-thread-per-core-no-sparse-tier-kept-2026-10-03.
        const uint64_t l2_thread = opt.l2_bytes_override ? opt.l2_bytes_override : min_l2_share;
        if (l2_thread) {
            const uint64_t whole_k = std::min(l2_thread, 32 * l1_max) * 8 / 64 * 64;
            if (base.count <= HALF_L2_MAX_BASE_PRIMES) {
                const uint64_t half_k = std::max<uint64_t>(64, whole_k / 2 / 64 * 64);
                if (half_k != seg_k_width) {
                    seg_k_width = half_k;
                    opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE;
                }
                half_l2_few_primes = true;
            } else if (whole_k > seg_k_width) {
                seg_k_width = whole_k;
                opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE;
                whole_l2_base = true;
            }
        }
    }

    // Ceiling on the automatic segment: the L2 per thread, within 16 x L1d
    // (primesieve's own ceiling, api.cpp's get_sieve_size) and 32 x L1d,
    // and the power-of-2 fixup below rounds it down. Without it the
    // doubling above takes the segment past what pays on a CPU with a large
    // L2 per thread. Last 1e11 below N, A/B on the same host:
    //   - 2-vCPU Xeon Emerald Rapids (48 KiB L1d, 2 MiB L2 per vCPU): 1 MiB
    //     best everywhere; 16 x L1d alone (768/512 KiB) was +6% at 1e13 and
    //     +3% at 1e14, and the uncapped 2 MiB +4..+9% from 1e15 to 1e18.
    //     1.5 MiB (32 x L1d) is not a width the sparse tier can take, so
    //     the ceiling lands on 1 MiB whether its term is L2 or L2 / 2;
    //   - 2-vCPU Xeon @ 2.80GHz (32 KiB L1d, 1 MiB L2): 1 MiB (the whole
    //     L2) over 512 KiB (L2 / 2) in three rounds, -2..-5% at every tail
    //     from 1e14 to 1e18 (2026-10-03). The -7% at 1e15 for 512 KiB that
    //     fitted an L2 / 2 term (2026-10-02) was one host, one round.
    // The dev PC and the i5-13500 (L2 shared by HT siblings, 256 KiB per
    // thread) get 16 x 48 KiB = 768 KiB, above their 512 KiB: unchanged.
    // Auto width only. See
    // docs/RESEARCH.md#segment-ceiling-half-the-l2-per-thread-within-16-32-x-l1d-kept-2026-10-02.
    uint64_t seg_uncapped_k = 0; // startup log: the width before the ceiling, 0 if it didn't apply
    if (!opt.segment_width_set && sparse_regime) {
        const uint64_t l2_thread = opt.l2_bytes_override ? opt.l2_bytes_override : min_l2_share;
        const uint64_t cap_bytes = std::max(16 * l1_max, std::min(32 * l1_max, l2_thread));
        const uint64_t cap_k = cap_bytes * 8; // bytes -> wheel indices (one bit each)
        if (seg_k_width > cap_k) {
            seg_uncapped_k = seg_k_width;
            seg_k_width = cap_k;
            opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE; // keep the startup log's "segment=" accurate
        }
    }

    // The sparse tier's EratBig-style rewrite (segment_sieve.hpp) needs
    // the segment width in BYTES to be a power of 2 for its bucket-slot
    // math to be a shift/mask instead of a division. base_limit >=
    // seg_k_width is a conservative check for "will any base prime
    // actually end up sparse" (base_limit is isqrt(limit), an upper bound
    // on the largest base prime) -- when it's false, no prime is
    // classified sparse below and the width is left exactly as
    // auto-tuned. small_limit/the small-vs-medium cutoff are untouched.
    //
    // Medium/sparse cutoff: sparse_limit = seg_k_width * NUM / DEN. 1/2
    // (primes with ~1-2 hits per segment go to the bucket ring instead of
    // paying the medium tier's per-segment loop-exit mispredict) only when
    // the sparse tier exists anyway (sparse_regime) AND every CPU has at
    // least 512KiB of L2 to itself; 1/1 (plain `p >= seg_k_width`)
    // otherwise. The gain depends on per-thread L2: measured at 1e14 (10%
    // tail, ABBA) +8.7% wall with 256KiB (i5-11400F, 12 threads), +0.1%
    // with 512KiB (same machine, 6 threads pinned one per core), -2.3% with
    // 640KiB (i5-13500 P-cores); i5-13500 full machine (P 640KiB, E
    // 512KiB) -1.8%, and neutral there at 1e13 and 1e15. See
    // docs/RESEARCH.md#i5-13500-server-gap-vs-primesieve-medium-tier-call-count-sparse-cutoff-12-gated-on-per-thread-l2-2026-09-28 --
    // and the three earlier `seg_k_width/4` reverts at
    // docs/RESEARCH.md#eratbig-style-sparse-tier-forcing-a-power-of-2-segment-width-and-sparse_limit--seg_k_width4-all-attempts-reverted.
    // From 1 MiB of L2 per thread the optimum moves on to 1/4: on the
    // 2-vCPU sandboxes (1 MiB and 2 MiB per vCPU, no SMT) 1/4 is -3..-5%
    // against 1/2 at the 1e14 and 1e15 tails, while the i5-13500 (640KiB)
    // had 1/4 behind 1/2 (-1.1% vs -6.0% at 1e14, +6.2% vs +0.2% at 1e15).
    // See docs/RESEARCH.md#sparse-cutoff-14-from-1-mib-of-l2-per-thread-kept-2026-10-03.
    // Detected from sysfs regardless of --l2-bytes (it's a property of the
    // hardware, not of the segment sizing); undetected -> 1/1.
    // --tune sparse=a/b overrides it (lowering only).
    // Evaluated after the power-of-2 fixup below, like med64_limit.
    constexpr uint64_t SPARSE_HALF_MIN_L2_SHARE = 512 * 1024;
    constexpr uint64_t SPARSE_QUARTER_MIN_L2_SHARE = 1024 * 1024;
    uint64_t sparse_num = 1;
    uint64_t sparse_den = !sparse_regime ? 1
                        : min_l2_share >= SPARSE_QUARTER_MIN_L2_SHARE ? 4
                        : min_l2_share >= SPARSE_HALF_MIN_L2_SHARE ? 2 : 1;
    // Second gate, by the L3 each ACTIVE thread has (total / the threads
    // that run, at most the CPUs sharing it): 1/4 from 4 MiB. The cutoff
    // is really about hits per medium call -- below 1/4 of the segment a
    // prime hits fewer than 4 times per segment and the call's fixed cost
    // (state load, loop entry, the exit mispredict, state store; ~34
    // cycles, 60 instructions on the i5-11400F) outweighs its marks, while
    // the bucket ring pays per hit -- and what makes the ring affordable is
    // the bandwidth the active threads share. Dev PC (12 MiB L3), `--tune
    // sparse` vs auto, 1e10 windows, interleaved x2: 1 thread 1/4 -10.5%
    // (1e13) / -12.4% (1e14); 2 threads -7.6% / -13.8%; 6 threads (2 MiB
    // each) noise at 1e13-1e14 and +3.3% at 1e15; 12 threads +13%. The
    // i5-13500 at 20 threads (1.2 MiB) +3.2%. Every measured optimum fits:
    // the 2-vCPU Xeons (16-130 MiB) at 1/4, the HT laptops (1 MiB) at 1/1.
    // Below the sparse regime the lowered cutoff is what creates the sparse
    // tier (the power-of-2 fixup below handles the width), so it only
    // applies when at least an octave of base primes lands there: on the
    // Emerald Rapids at 1e13 (1.5 MiB segment, every prime <= isqrt(N) has
    // 4+ hits) 1/4 would only have shrunk the segment, +1.4%. See
    // docs/RESEARCH.md#one-thread-per-core-the-medium-tiers-per-call-cost-and-the-sparse-cutoff-by-active-threads-2026-10-03-evening.
    // And 1/2 from 1.5 MiB: the i5-3470 (6 MiB L3, 256 KiB L2) at 4 threads
    // (1.5 MiB each) and at 2 (3 MiB) had 1/2 at -3.5..-5.1% on the 1e13 and
    // 1e14 tails, 1/4 at -1..-3.5% (4/4 on three of the four 1/2 pairs);
    // the dev PC at 6 threads (2 MiB) -2.1% at 1e14 (4/4), noise at 1e15.
    // Below the sparse regime the 1/2 step can't have an octave (the margin
    // is the regime itself), so it only applies inside it.
    constexpr uint64_t SPARSE_QUARTER_MIN_L3_PER_THREAD = 4 * uint64_t{1024} * 1024;
    constexpr uint64_t SPARSE_HALF_MIN_L3_PER_THREAD = 3 * uint64_t{512} * 1024;
    // cpu0's L3, read once: this gate and the medium-tier prefetch gate below.
    const CpuCacheInfo l3 = detect_cpu_cache_info(0, 3);
    uint64_t l3_per_thread = 0;
    if (l3.total_bytes && l3.sharers > 0)
        l3_per_thread = l3.total_bytes / std::max<uint64_t>(1, std::min<uint64_t>(opt.threads, static_cast<uint64_t>(l3.sharers)));
    bool sparse_l3_gate = false; // startup log
    if (l3_per_thread >= SPARSE_QUARTER_MIN_L3_PER_THREAD &&
        (sparse_regime || base_limit >= 2 * (seg_k_width / 4))) {
        sparse_den = 4;
        sparse_l3_gate = true;
    } else if (l3_per_thread >= SPARSE_HALF_MIN_L3_PER_THREAD && sparse_regime && sparse_den < 2) {
        sparse_den = 2;
        sparse_l3_gate = true;
    }
    const uint64_t sparse_den_auto = sparse_den; // startup log
    if (opt.tune_sparse.den) { sparse_num = opt.tune_sparse.num; sparse_den = opt.tune_sparse.den; } // in (0, 1], parse_tune
    // Power-of-2 fixup whenever some prime may end up sparse. With the
    // default cutoff this is base_limit >= seg_k_width; a lowered cutoff
    // (NUM < DEN) can make primes sparse below that, so take the smaller.
    uint64_t seg_unrounded_k = 0; // startup log: an explicit -s the fixup rounded down, 0 otherwise
    if (base_limit >= seg_k_width || base_limit >= seg_k_width * sparse_num / sparse_den) {
        uint64_t sb = seg_k_width / 8, p2 = 1;
        while (p2 * 2 <= sb) p2 *= 2;
        if (p2 != sb) {
            if (opt.segment_width_set) seg_unrounded_k = seg_k_width;
            seg_k_width = std::max<uint64_t>(64, p2 * 8);
            opt.segment_width = seg_k_width * WHEEL_MOD / WHEEL_SIZE; // keep the startup log's "segment=" accurate
        }
    }

    // med64 tier: primes in [small_limit, med64_limit) use erat_small.hpp's
    // byte-marking cross_off<PR> (via SegmentSieve::process_med64), grouped
    // into 384 (class, entry phase) lists -- a bounded sub-band close to
    // small_limit, not the whole medium tier (see
    // docs/RESEARCH.md#medium-tier-64-list-restructuring-scoped-to-a-bounded-sub-band-med64_primes-kept-2026-09-26
    // for why the whole-tier version was reverted first). med64_limit swept
    // via --tune med64=a/b without recompiling; med64=0 disables
    // the tier, exactly reproducing the pre-med64 baseline. Default 1/6
    // since 2026-10-04 (1/12 before: on a 256 KiB segment that band ended
    // at p = 175K, and the i5-3470 wanted it at 350K, -3.8%; 1/6 is neutral
    // on the 512 KiB and 1 MiB segments of the other machines -- see
    // docs/RESEARCH.md#one-thread-per-core-the-medium-tiers-per-call-cost-and-the-sparse-cutoff-by-active-threads-2026-10-03-evening);
    // 1/12 was
    // jointly re-tuned with small_limit's own divisor above -- see
    // docs/RESEARCH.md#small_limit-re-tuned-jointly-with-med64_limit-kept-2026-09-26.
    // The whole segment on a core whose L2 is 256 KiB or less (i5-3470, Ivy
    // Bridge: the medium tier's generic per-prime stepping is what that core
    // pays; 1/2 was -5.9% at 1e12 and -4.5% at the 1e13 tail, 3/3 each, and
    // the whole segment -6.7% at 1e12 and a tie with 1/2 at 1e13). The modern cores rank the two tiers the
    // other way (i5-11400F +7.7%/+11.5%, i5-13500 +7.6%/+3.2%, the Xeon
    // VMs within noise), hence the gate on the physical L2 as the proxy for
    // an old core -- unmeasured on Skylake-class clients, which have 256
    // KiB too. --l2-bytes counts as the L2 here, --tune med64 overrides.
    // docs/RESEARCH.md#i5-3470-profile-at-1e12-the-med64-tier-over-the-whole-l2-segment-is-59-of-the-cycles-open-2026-10-04
    uint64_t med64_num = 1, med64_den = 6;
    bool med64_l2_gate = false;
    if (opt.tune_med64.den) { med64_num = opt.tune_med64.num; med64_den = opt.tune_med64.den; }
    else if (small_l2_core) { med64_den = 1; med64_l2_gate = true; }
    uint64_t med64_limit = seg_k_width * med64_num / med64_den;
    uint64_t sparse_limit = seg_k_width * sparse_num / sparse_den;

    // Primes also covered by the pre-sieve pattern (see presieve.hpp) are
    // skipped here: they're never scheduled as active markers, their
    // multiples come pre-marked from the pattern buffer instead. They
    // still come out as output/count -- nothing marks the primes
    // themselves composite either way, so they survive extraction exactly
    // as before.
    //
    // The rest split into four tiers by expected hits (see
    // segment_sieve.hpp / erat_small.hpp):
    //   - small_primes (p < small_limit): many hits per L1 sub-block,
    //     crossed off one sub-block at a time so the marks land in L1.
    //   - med64_primes (small_limit <= p < med64_limit): still many hits
    //     per segment, byte-marked like the small tier but over the whole
    //     segment at once (see above).
    //   - medium_primes (med64_limit <= p < seg_k_width): a few hits per
    //     segment, one pass over the whole segment each.
    //   - sparse_primes (p >= seg_k_width): at most ~1 hit/segment, bucket
    //     ring, EratBig-style (see segment_sieve.hpp's process_big).
    std::vector<uint64_t> presieve_primes_flat;
    for (const auto& group : PRESIEVE_GROUPS)
        presieve_primes_flat.insert(presieve_primes_flat.end(), group.begin(), group.end());
    const uint64_t presieve_max = *std::max_element(presieve_primes_flat.begin(), presieve_primes_flat.end());

    // Every prime from FIRST_WHEEL_PRIME up to presieve_max is pre-sieved
    // (checked below), so the sparse tier is exactly the bitmap's primes from
    // max(sp_limit, presieve_max + 1) up -- a run of it, not a copy (50M
    // primes at 1e18) -- and only the dense tiers' primes are listed.
    auto classify = [&](TierSet& t, uint64_t m64_limit, uint64_t sp_limit) {
        const uint64_t sparse_from = std::max(sp_limit, presieve_max + 1);
        t.sparse = base.from(sparse_from);
        base.for_each(0, sparse_from, [&](uint64_t p) {
            if (p <= presieve_max &&
                std::find(presieve_primes_flat.begin(), presieve_primes_flat.end(), p) != presieve_primes_flat.end())
                return;
            if (p >= sp_limit)
                throw std::logic_error("classify: a prime below the largest pre-sieve prime isn't pre-sieved");
            if (p < small_limit) t.small.push_back(p);
            else if (p < m64_limit) t.med64.push_back(p);
            else t.medium.push_back(p);
        });
    };
    TierSet wide{seg_k_width, {}, {}, {}, {}};
    classify(wide, med64_limit, sparse_limit);

    // Narrow segment for the chunks below narrow^2. The doubled segment
    // (see sparse_regime above) only pays off where sparse primes are
    // active; a chunk whose every number is < narrow^2 has no active prime
    // >= narrow (activation is by p^2), so it runs exactly the non-sparse
    // configuration that measured +6.5% faster at 1e12 on the narrow
    // width: narrow segment, 1/1 cutoff, med64_limit on the narrow width.
    // Its sparse list only holds primes that never activate there. narrow
    // is a power of 2 in bytes (half of the fixed-up wide width), as the
    // sparse ring requires. ~44% of a 1e13 run on a 256KiB/512KiB machine.
    TierSet narrow{seg_k_width / 2, {}, {}, {}, {}};
    uint64_t narrow_k_end = 0; // chunks with high <= this use `narrow`
    bool narrow_early = !opt.segment_width_set && sparse_regime && narrow.width >= 64 && narrow.width % 64 == 0;
    if (narrow_early) {
        narrow_k_end = wheel_count_upto(std::min(opt.limit, narrow.width * narrow.width));
        // A --start tail beginning past narrow^2 (split_ranges' first k) has
        // no narrow chunk: skip a second pass over every base prime.
        const uint64_t first_k = opt.start ? wheel_count_upto(opt.start - 1) / 64 * 64 : 0;
        if (first_k < narrow_k_end) classify(narrow, narrow.width * med64_num / med64_den, narrow.width);
        else { narrow_k_end = 0; narrow_early = false; }
    }

    // Steal threshold until the run has measured its own activation cost and
    // rates (run_parallel_chunks): a steal activates every base prime once
    // for the stolen piece; at the top of N that is all ~sqrt(N)/ln of them,
    // about as much as sieving 2.6 wheel indices each (dev PC, 1e18, 12
    // threads activating at once). At least 4 indices per base prime.
    constexpr uint64_t STEAL_K_PER_BASE_PRIME = 4;
    cfg.steal_min_k = STEAL_K_PER_BASE_PRIME * base.count;

    // Medium-tier prefetchnta gate: on once the medium state (8 bytes per
    // prime, SoA -- see erat_small.hpp::cross_off_medium) outgrows the
    // per-thread L3 share, where it comes from DRAM anyway and keeping it out
    // of L2 only protects the segment. Dev PC (1 MB L3/thread): +0.8% at 1e12
    // (0.5 MB of state), -6.1% at 1e13 (1.6 MB), -12.1% at 1e14. See
    // docs/RESEARCH.md. Undetected L3 -> 1 MiB.
    {
        uint64_t l3_share = l3.sharers > 0 ? l3.total_bytes / static_cast<uint64_t>(l3.sharers) : 0;
        if (l3_share == 0) l3_share = 1024 * 1024;
        cfg.medium_nta_min_primes = l3_share / 8;
        // --tune medium_nta=1|0 overrides the gate: under a VM the detected
        // L3 can be the whole host's (260 MB on a 2-vCPU KVM guest).
        if (opt.medium_nta >= 0) cfg.medium_nta_min_primes = opt.medium_nta ? 0 : UINT64_MAX;
    }


    P.seg_k_width = seg_k_width;
    P.small_limit = small_limit;
    P.med64_limit = med64_limit;
    P.sparse_limit = sparse_limit;
    P.sparse_num = sparse_num;
    P.sparse_den = sparse_den;
    P.sparse_den_auto = sparse_den_auto;
    P.sparse_l3_gate = sparse_l3_gate;
    P.med64_l2_gate = med64_l2_gate;
    P.small_l2_gate = small_l2_gate;
    P.half_l2_few_primes = half_l2_few_primes;
    P.base_count = base.count;
    P.l3_per_thread = l3_per_thread;
    P.sparse_regime = sparse_regime;
    P.wide = std::move(wide);
    P.narrow = std::move(narrow);
    P.narrow_k_end = narrow_k_end;
    P.narrow_early = narrow_early;
    P.min_l2_share = min_l2_share;
    P.l1_big_cores = l1_big_cores;
    P.whole_l2_base = whole_l2_base; // sub_block_whole_l1d: set by finish_threads

    P.seg_l1_capped_k = seg_l1_capped_k;
    P.seg_uncapped_k = seg_uncapped_k;
    P.seg_unrounded_k = seg_unrounded_k;
    return P;
}

// The startup log: the configuration and which rules decided it.
inline void print_plan(const SievePlan& P, const Options& opt, unsigned actual_threads,
                       const std::vector<ChunkRange>& ranges) {
    const SieveConfig& cfg = P.cfg;
    std::fprintf(stderr, "Starting %u threads, limit=%llu, segment=%llu, wheel mod %llu (%zu primes), "
                 "%zu small base primes (sub-block %llu KiB), %zu med64, %zu medium, %zu sparse...\n",
                 actual_threads,
                 static_cast<unsigned long long>(opt.limit),
                 static_cast<unsigned long long>(opt.segment_width),
                 static_cast<unsigned long long>(WHEEL_MOD),
                 WHEEL_PRIMES.size(),
                 P.wide.small.size(), static_cast<unsigned long long>(cfg.sub_block_bytes / 1024),
                 P.wide.med64.size(), P.wide.medium.size(), static_cast<size_t>(P.wide.sparse.size()));
    if (P.sub_block_whole_l1d)
        std::fprintf(stderr, "  sub-block: whole L1d (%u threads <= %u cores with the largest L1d)\n",
                     actual_threads, P.l1_big_cores);
    if (P.whole_l2_base)
        std::fprintf(stderr, "  segment: whole L2 per thread (%u threads <= %u cores, no sparse tier)\n",
                     opt.threads, P.l1_big_cores);
    if (P.seg_l1_capped_k)
        std::fprintf(stderr, "  segment: %llu KiB instead of %llu KiB (cap: 32 x L1d; sysfs reports %llu KiB of L2 per thread)\n",
                     static_cast<unsigned long long>(P.seg_k_width / 8 / 1024),
                     static_cast<unsigned long long>(P.seg_l1_capped_k / 8 / 1024),
                     static_cast<unsigned long long>(P.min_l2_share / 1024));
    if (P.seg_uncapped_k)
        std::fprintf(stderr, "  segment: %llu KiB instead of %llu KiB (ceiling: the L2 per thread, 16-32 x L1d)\n",
                     static_cast<unsigned long long>(P.seg_k_width / 8 / 1024),
                     static_cast<unsigned long long>(P.seg_uncapped_k / 8 / 1024));
    if (P.seg_unrounded_k)
        std::fprintf(stderr, "  segment: %llu KiB instead of the %llu KiB of -s (a power of 2 for the sparse tier)\n",
                     static_cast<unsigned long long>(P.seg_k_width / 8 / 1024),
                     static_cast<unsigned long long>(P.seg_unrounded_k / 8 / 1024));
    if (opt.medium_nta >= 0) {
        std::fprintf(stderr, "  medium-tier prefetchnta: %s (forced, --tune medium_nta=%d)\n",
                     opt.medium_nta ? "yes" : "no", opt.medium_nta);
    } else {
        std::fprintf(stderr, "  medium-tier prefetchnta: %s (from %s medium primes)\n",
                     P.wide.medium.size() >= cfg.medium_nta_min_primes ? "yes" : "no",
                     format_thousands(cfg.medium_nta_min_primes).c_str());
    }
    if (!P.wide.sparse.empty())
        std::fprintf(stderr, "  sparse cutoff: %llu/%llu of the segment%s\n",
                     static_cast<unsigned long long>(P.sparse_num), static_cast<unsigned long long>(P.sparse_den),
                     opt.tune_sparse.den ? " (--tune sparse)"
                     : P.sparse_l3_gate ? (P.sparse_den_auto == 4 ? " (L3 per active thread >= 4 MiB)"
                                                                   : " (L3 per active thread >= 1.5 MiB)")
                     : P.sparse_den_auto == 4 ? " (L2 per thread >= 1 MiB)"
                     : P.sparse_den_auto == 2 ? " (L2 per thread >= 512 KiB)" : "");
    if (!cfg.big2310 && !P.wide.sparse.empty()) std::fprintf(stderr, "  sparse tier on the mod-210 wheel (--tune big2310=0)\n");
    if (!P.wide.sparse.empty())
        std::fprintf(stderr, "  sparse ring: %s arenas%s\n",
                     cfg.huge_arenas ? "2 MiB huge-page" : "1 MiB",
                     opt.huge >= 0 ? " (--tune huge)" : cfg.huge_arenas ? " (one thread per core)" : "");
    if (P.half_l2_few_primes)
        std::fprintf(stderr, "  segment: half the whole-L2 width (one thread per core, %s base primes <= 40,000)\n",
                     format_thousands(P.base_count).c_str());
    if (P.small_l2_gate)
        std::fprintf(stderr, "  small cutoff: 1/2 of the sub-block (L2 of 256 KiB or less)\n");
    if (P.med64_l2_gate)
        std::fprintf(stderr, "  med64 cutoff: the whole segment (L2 of 256 KiB or less)\n");
    if (P.narrow_early) {
        unsigned narrow_chunks = 0;
        for (const auto& r : ranges) narrow_chunks += r.high <= P.narrow_k_end;
        std::fprintf(stderr, "  narrow segment (%llu) up to %llu: %u of %u chunks\n",
                     static_cast<unsigned long long>(P.narrow.width * WHEEL_MOD / WHEEL_SIZE),
                     static_cast<unsigned long long>(std::min(opt.limit, P.narrow.width * P.narrow.width)),
                     narrow_chunks, static_cast<unsigned>(ranges.size()));
    }
}
