#pragma once
// What main() decides once per run from N, the detected caches, the thread
// count and --tune: the segment width (in order: the base from cpu0's L2,
// the smallest per-CPU share on a hybrid, the 32 x L1d cap, the whole-L2
// base with one thread per core, the doubling once the sparse tier exists,
// its ceiling, and the power-of-2 fixup), the sub-block, the tier cutoffs,
// the medium-tier prefetch gate, the steal threshold, and the base primes
// split into tiers (one set, or two when the early chunks run a narrower
// segment). plan_sieve() decides, print_plan() says what it chose and why.
// Every rule is derived from caches, threads or CPU flags, never from a
// CPU model; its measurements are in docs/RESEARCH.md (linked per rule).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "arg_parser.hpp"
#include "base_sieve.hpp"
#include "colors.hpp"
#include "cpu_cache.hpp"
#include "presieve.hpp"
#include "segment_sieve.hpp"
#include "wheel.hpp"

static_assert(MAX_SEGMENT_WIDTH / 30 * 8 == MAX_SEG_K_WIDTH, "-s's bound is the widest segment");

// What main() decides once for a run and the workers read: sizes the tiers
// are built with and the scheduler's knobs. Set from the detected caches,
// the thread count and --tune (see main()).
struct SieveConfig {
    // Slice the small dense tier is crossed off in (SegmentSieve): half the
    // detected L1d, the same for every thread, or the whole L1d when every
    // thread has a core to itself.
    uint64_t sub_block_bytes = 32 * 1024;
    // Medium-tier prefetchnta (erat_small.hpp::cross_off_medium) is on for a
    // tier set with at least this many medium primes (see plan_sieve).
    uint64_t medium_nta_min_primes = UINT64_MAX;
    // --debug-idle: run_parallel_chunks prints how far apart the threads finished.
    bool debug_idle = false;
    // Smallest piece (wheel indices) a worker steals from another's run in
    // run_parallel_chunks before the run has measured anything: the thief
    // pays one activation of every base prime for it, so the piece must
    // take longer to sieve than that.
    uint64_t steal_min_k = 0;
    // Sparse ring arenas as 2 MiB huge pages (SegmentSieve): with one thread
    // per core, not with HT pairs (slower there). --tune huge=1|0 forces it.
    // See docs/RESEARCH.md#sparse-ring-arenas-as-2-mib-huge-pages-with-one-thread-per-core-kept-2026-10-04.
    bool huge_arenas = false;
    // First wheel index the count includes: 1 (the number 1 is not prime),
    // or with --start the index of the first number >= start.
    uint64_t skip_below_k = 1;
    // N: sparse primes whose first multiple lies past it aren't filed
    // (SegmentSieve::set_range_end).
    uint64_t range_end = UINT64_MAX;
};

struct ChunkRange {
    uint64_t low;   // first wheel index of the chunk (inclusive)
    uint64_t high;  // upper bound in wheel index (exclusive)
};

// Segment width plus the base primes split into tiers for it (see main()).
// A run has one, or two when its early chunks use a narrower segment. The
// sparse tier is a run of the base-prime bitmap, not a copy (see classify).
struct TierSet {
    uint64_t width = 0;
    std::vector<uint64_t> small, med64, medium;
    SparsePrimes sparse;
};

// The decisions, plus what print_plan needs to explain them.
struct SievePlan {
    SieveConfig cfg;             // sub-block, prefetch gate, steal threshold, switches
    uint64_t seg_k_width = 0;    // wide segment, in wheel indices
    TierSet wide, narrow;        // narrow: unused unless narrow_k_end > 0
    uint64_t narrow_k_end = 0;   // chunks with high <= this use `narrow`
    unsigned l1_big_cores = 0;   // physical cores with the largest L1d (0: unknown)

    // For the startup log: what the topology said and which rules fired.
    uint64_t sparse_num = 1, sparse_den = 1, sparse_den_auto = 1;
    bool sparse_l3_gate = false;     // the cutoff came from the L3 per active thread
    bool med64_l2_gate = false;      // med64 = the whole segment, from the small-L2 rule
    bool small_l2_gate = false;      // small cutoff 1/2 of the sub-block, same rule
    bool half_l2_few_primes = false; // base at half the whole-L2 width: one per core, few base primes
    bool whole_l2_base = false;
    bool sub_block_whole_l1d = false;
    uint64_t base_count = 0;
    uint64_t seg_l1_capped_k = 0, seg_uncapped_k = 0, seg_unrounded_k = 0; // widths before a cap / fixup
    uint64_t seg_max_capped_k = 0;

    const TierSet& tiers_for(const ChunkRange& r) const { return r.high <= narrow_k_end ? narrow : wide; }

    // After the chunking, since a small range can leave fewer threads than
    // -t: the sub-block grows to the whole L1d when the threads that
    // actually run fit one per core with the largest L1d (see plan_sieve).
    void finish_threads(unsigned actual_threads) {
        if (l1_big_cores && actual_threads <= l1_big_cores) { cfg.sub_block_bytes *= 2; sub_block_whole_l1d = true; }
    }
};

// MemAvailable from /proc/meminfo, in bytes; 0 when unknown (not Linux).
inline uint64_t mem_available_bytes() {
    std::ifstream f("/proc/meminfo");
    std::string key;
    uint64_t kib = 0;
    std::string unit;
    while (f >> key >> kib >> unit)
        if (key == "MemAvailable:") return kib * 1024;
    return 0;
}

// The threads the run's memory allows. Each worker keeps its own state for
// every base prime -- one 8-byte entry per sparse prime in its ring, about
// as much per dense prime -- so it takes ~8 B x pi(sqrt N) (x 1.02: a
// 16-byte header per 4 KiB ring block) plus a fixed ~16 MiB (segment
// buffers, arena granularity, partly filled blocks, an output buffer).
// Shared: the base-prime bitmap and its rank index, plus 64 MiB. At the
// 64-bit ceiling this estimates 9.6 GiB on 6 threads (measured 9.24-9.38).
//
// Budget: --max-mem, or 90% of MemAvailable (0 / unknown: no cap). A run
// that wouldn't fit runs on fewer threads instead of being killed by the
// kernel mid-way; at those heights the sparse tier is bound by DRAM
// bandwidth and fewer threads lose little (docs/RESEARCH.md).
struct MemCap {
    unsigned threads = 0;     // the threads to run
    unsigned requested = 0;   // -t (or the core count)
    uint64_t per_thread = 0;  // estimated bytes per worker
    uint64_t shared = 0;      // estimated bytes shared by all
    uint64_t budget = 0;      // bytes the run may take, 0 = no cap
    bool from_flag = false;   // budget given by --max-mem
    bool capped() const { return threads < requested; }
    bool over() const { return budget && shared + per_thread * threads > budget; } // even one thread doesn't fit
};
inline MemCap cap_threads_by_memory(const Options& opt, const BasePrimes& base) {
    MemCap c;
    c.requested = c.threads = opt.threads;
    c.per_thread = base.count * 8 * 102 / 100 + (uint64_t{16} << 20);
    c.shared = (base.bits.size() + base.rank.size()) * 8 + (uint64_t{64} << 20);
    c.from_flag = opt.max_mem_set;
    c.budget = opt.max_mem_set ? opt.max_mem : mem_available_bytes() / 10 * 9;
    if (c.budget == 0) return c;
    const uint64_t room = c.budget > c.shared ? c.budget - c.shared : 0;
    const uint64_t fit = room / c.per_thread;
    if (fit < c.threads) c.threads = static_cast<unsigned>(std::max<uint64_t>(fit, 1));
    return c;
}

inline SievePlan plan_sieve(const Options& opt, const BasePrimes& base, uint64_t base_limit) {
    SievePlan P;
    SieveConfig& cfg = P.cfg;
    cfg.debug_idle = opt.debug_idle;
    cfg.skip_below_k = opt.start ? std::max<uint64_t>(wheel_count_upto(opt.start - 1), 1) : 1;
    cfg.range_end = opt.limit;

    // The base segment: half of cpu0's L2 (--l2-bytes stands in for it;
    // 256 KiB when undetected), not capped at isqrt(N), see
    // docs/RESEARCH.md#auto-segment-width-dropping-the-isqrtlimit-cap-kept.
    // An explicit -s is a numeric width, converted to wheel indices. Either
    // way the width is whole 64-bit words, so every segment starts on a word
    // boundary. The steps below adjust the automatic width only, except the
    // power-of-2 fixup.
    const uint64_t l2_core = opt.l2_bytes_override ? opt.l2_bytes_override : detect_cpu_cache_info(0, 2).total_bytes;
    uint64_t seg_k_width = opt.segment_width_set
                         ? std::max<uint64_t>(64, (opt.segment_width * WHEEL_SIZE / WHEEL_MOD) / 64 * 64)
                         : seg_k_width_from_l2_bytes(l2_core);

    // A core with an L2 of 256 KiB or less (i5-3470, Ivy Bridge): the old
    // core the small-cutoff and med64 rules below key on. --l2-bytes counts
    // as the L2 here.
    constexpr uint64_t SMALL_L2_BYTES = 256 * uint64_t{1024};
    const bool small_l2_core = l2_core && l2_core <= SMALL_L2_BYTES;

    // Small/med64 cutoff, as a fraction of the sub-block: 1/4, tuned jointly
    // with med64_limit below
    // (docs/RESEARCH.md#small_limit-re-tuned-jointly-with-med64_limit-kept-2026-09-26).
    // 1/2 on a small-L2 core, where the primes just above it are better off
    // L1-blocked
    // (docs/RESEARCH.md#small-cutoff-12-of-the-sub-block-on-a-256-kib-l2-core-kept-2026-10-06).
    // --tune small=a/b overrides.
    uint64_t small_num = 1, small_den = 4;
    if (opt.tune_small.den) { small_num = opt.tune_small.num; small_den = opt.tune_small.den; }
    else if (small_l2_core) { small_den = 2; P.small_l2_gate = true; }

    // The small tier is crossed off one sub-block at a time (see
    // SegmentSieve::sieve_and_emit); sub-block = half the detected L1d
    // (sub_block_from_l1_bytes), the whole of it with one thread per core
    // (finish_threads).
    uint64_t l1_bytes = opt.l1_bytes_override ? opt.l1_bytes_override : detect_cpu_cache_info(0, 1).total_bytes;
    cfg.sub_block_bytes = sub_block_from_l1_bytes(l1_bytes);
    uint64_t small_limit = cfg.sub_block_bytes * small_num / small_den;
    uint64_t l1_max = l1_bytes ? l1_bytes : 32 * 1024; // largest per-core L1d (segment ceiling below)

    // Hybrid P-core/E-core correction: the detection above reads cpu0 only.
    // The segment takes the smallest per-CPU L2 share, the sub-block the
    // largest per-CPU L1d -- the same for every thread, since threads
    // migrate between core types. See
    // docs/RESEARCH.md#cache-topology-sizing-per-cpu-minimum-step-kept.
    // Skipped when the user forced a value (-s, --l2-bytes, --l1-bytes) or
    // detection found nothing.
    const CpuCacheTopology topo = detect_cpu_cache_topology();
    // Smallest L2 share per hardware thread (0: undetected); the whole-L2
    // base, the segment ceiling and the sparse cutoff below use it too.
    uint64_t min_l2_share = 0;
    for (uint64_t s : topo.l2_share)
        if (s && (min_l2_share == 0 || s < min_l2_share)) min_l2_share = s;
    if (!opt.segment_width_set && !opt.l2_bytes_override && !topo.l2_share.empty() && topo.l2_share[0] &&
        min_l2_share < topo.l2_share[0])
        seg_k_width = seg_k_width_from_l2_bytes(min_l2_share);
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
        // One thread per core: half the L1d is an HT pair's share, so with
        // no more threads than physical cores the sub-block takes the whole
        // L1d (Linux spreads threads one per core, P-cores first). Only
        // cores with the largest L1d count, each CPU on a shared L1d as
        // 1/sharers of a core; --l1-bytes doesn't switch it off.
        // small_limit keeps the half-L1d value. See
        // docs/RESEARCH.md#sub-block-the-whole-l1d-when-each-thread-has-a-core-to-itself-kept-2026-10-01.
        double big_cores = 0;
        for (size_t c = 0; c < topo.l1_raw.size(); ++c)
            if (topo.l1_raw[c] == max_l1_raw && topo.l1_sharers[c] > 0) big_cores += 1.0 / topo.l1_sharers[c];
        P.l1_big_cores = static_cast<unsigned>(big_cores + 0.5);
        // Applied in finish_threads, against the threads that actually run.
    }

    // Cap on the automatic base width: 32 x L1d, whatever sysfs claims for
    // the L2 (a VM can report the host's L3 as its L2). Applied before
    // sparse_regime, so the doubling and the ceiling below see the capped
    // width; only -s bypasses it. See
    // docs/RESEARCH.md#base-segment-capped-at-32-x-l1d-a-vm-whose-sysfs-reports-the-hosts-l3-as-l2-kept-2026-10-03.
    if (!opt.segment_width_set) {
        const uint64_t cap_k = 32 * l1_max * 8 / 64 * 64;
        if (seg_k_width > cap_k) {
            P.seg_l1_capped_k = seg_k_width;
            seg_k_width = cap_k;
        }
    }

    // The sparse regime: some base prime would be sparse (isqrt(N) >=
    // seg_k_width). From there the segment doubles to the whole L2 share:
    // every med64/medium prime pays a fixed cost per segment, and halving
    // the number of segments outweighs the extra cache pressure. Auto width
    // only. See
    // docs/RESEARCH.md#segment-width-doubled-once-the-sparse-tier-exists-kept-2026-09-27.
    // The same condition gates the lowered sparse cutoffs below.
    const bool sparse_regime = base_limit >= seg_k_width;
    const bool one_per_core = P.l1_big_cores && opt.threads <= P.l1_big_cores;
    cfg.huge_arenas = opt.huge >= 0 ? opt.huge != 0 : one_per_core;
    // With few base primes (<= 40K, sqrt(N) ~ 500K) the whole-L2 width
    // below doesn't pay: more segments cost little while the primes are
    // few, and the smaller one leaves L2 ways to the state streams. Half of
    // the whole-L2 width then, on any one-per-core machine. See
    // docs/RESEARCH.md#half-the-whole-l2-width-with-few-base-primes-on-every-one-per-core-machine-kept-2026-10-06
    constexpr uint64_t HALF_L2_MAX_BASE_PRIMES = 40000;
    if (!opt.segment_width_set && sparse_regime) {
        seg_k_width *= 2;
    } else if (!opt.segment_width_set && one_per_core) {
        // No sparse tier and a core to itself: the base segment is the
        // thread's whole L2 share, not half of it (the half is an HT
        // sibling's), within 32 x L1d -- every base prime is dense here and
        // the medium tier pays per prime per segment. --l2-bytes stands in
        // for the detected share. See
        // docs/RESEARCH.md#whole-l2-base-segment-one-thread-per-core-no-sparse-tier-kept-2026-10-03.
        const uint64_t l2_thread = opt.l2_bytes_override ? opt.l2_bytes_override : min_l2_share;
        if (l2_thread) {
            const uint64_t whole_k = std::min(l2_thread, 32 * l1_max) * 8 / 64 * 64;
            if (base.count <= HALF_L2_MAX_BASE_PRIMES) {
                seg_k_width = std::max<uint64_t>(64, whole_k / 2 / 64 * 64);
                P.half_l2_few_primes = true;
            } else if (whole_k > seg_k_width) {
                seg_k_width = whole_k;
                P.whole_l2_base = true;
            }
        }
    }

    // Ceiling on the automatic segment in the sparse regime: the L2 per
    // thread, within 16 x L1d (primesieve's own ceiling) and 32 x L1d;
    // the power-of-2 fixup below rounds it down. It keeps the doubling from
    // filling a large L2 per thread (2 MiB on Emerald Rapids). Auto width
    // only. See
    // docs/RESEARCH.md#segment-ceiling-half-the-l2-per-thread-within-16-32-x-l1d-kept-2026-10-02.
    if (!opt.segment_width_set && sparse_regime) {
        const uint64_t l2_thread = opt.l2_bytes_override ? opt.l2_bytes_override : min_l2_share;
        const uint64_t cap_bytes = std::max(16 * l1_max, std::min(32 * l1_max, l2_thread));
        const uint64_t cap_k = cap_bytes * 8; // bytes -> wheel indices (one bit each)
        if (seg_k_width > cap_k) {
            P.seg_uncapped_k = seg_k_width;
            seg_k_width = cap_k;
        }
    }
    // The widest segment the tiers support (16 MiB): only forced cache sizes
    // or a made-up topology reach it.
    if (seg_k_width > MAX_SEG_K_WIDTH) {
        P.seg_max_capped_k = seg_k_width;
        seg_k_width = MAX_SEG_K_WIDTH;
    }

    // Medium/sparse cutoff: sparse_limit = seg_k_width * NUM / DEN. Primes
    // below the cutoff pay the medium tier's fixed cost per segment (its
    // loop-exit mispredict), primes above it the bucket ring's cost per hit,
    // and what the ring costs depends on the cache the threads share. 1/1
    // by default; inside the sparse regime 1/2 from 512 KiB of L2 per thread
    // and 1/4 from 1 MiB (sysfs, regardless of --l2-bytes; undetected:
    // 1/1). See
    // docs/RESEARCH.md#i5-13500-server-gap-vs-primesieve-medium-tier-call-count-sparse-cutoff-12-gated-on-per-thread-l2-2026-09-28
    // and docs/RESEARCH.md#sparse-cutoff-14-from-1-mib-of-l2-per-thread-kept-2026-10-03.
    // --tune sparse=a/b overrides it (lowering only). sparse_limit and
    // med64_limit are computed after the power-of-2 fixup below.
    constexpr uint64_t SPARSE_HALF_MIN_L2_SHARE = 512 * 1024;
    constexpr uint64_t SPARSE_QUARTER_MIN_L2_SHARE = 1024 * 1024;
    uint64_t& sparse_num = P.sparse_num;
    uint64_t& sparse_den = P.sparse_den;
    sparse_den = !sparse_regime ? 1
                        : min_l2_share >= SPARSE_QUARTER_MIN_L2_SHARE ? 4
                        : min_l2_share >= SPARSE_HALF_MIN_L2_SHARE ? 2 : 1;
    // Second gate, by the L3 each ACTIVE thread has (total / the threads
    // that run, at most the CPUs sharing it): 1/4 from 4 MiB, 1/2 from 1.5
    // MiB -- the ring's traffic is what the active threads share. Below the
    // sparse regime the lowered cutoff is what creates the sparse tier, so
    // 1/4 applies there only when an octave of base primes lands in it, and
    // 1/2 not at all. See
    // docs/RESEARCH.md#one-thread-per-core-the-medium-tiers-per-call-cost-and-the-sparse-cutoff-by-active-threads-2026-10-03-evening.
    constexpr uint64_t SPARSE_QUARTER_MIN_L3_PER_THREAD = 4 * uint64_t{1024} * 1024;
    constexpr uint64_t SPARSE_HALF_MIN_L3_PER_THREAD = 3 * uint64_t{512} * 1024;
    // cpu0's L3, read once: this gate and the medium-tier prefetch gate below.
    const CpuCacheInfo l3 = detect_cpu_cache_info(0, 3);
    uint64_t l3_per_thread = 0;
    if (l3.total_bytes && l3.sharers > 0)
        l3_per_thread = l3.total_bytes / std::max<uint64_t>(1, std::min<uint64_t>(opt.threads, static_cast<uint64_t>(l3.sharers)));
    if (l3_per_thread >= SPARSE_QUARTER_MIN_L3_PER_THREAD &&
        (sparse_regime || base_limit >= 2 * (seg_k_width / 4))) {
        sparse_den = 4;
        P.sparse_l3_gate = true;
    } else if (l3_per_thread >= SPARSE_HALF_MIN_L3_PER_THREAD && sparse_regime && sparse_den < 2) {
        sparse_den = 2;
        P.sparse_l3_gate = true;
    }
    P.sparse_den_auto = sparse_den;
    if (opt.tune_sparse.den) { sparse_num = opt.tune_sparse.num; sparse_den = opt.tune_sparse.den; } // in (0, 1], parse_tune
    // Power-of-2 fixup (in bytes, for the ring's slot shift) whenever some
    // prime may end up sparse: base_limit (isqrt(N)) reaches the cutoff,
    // the default one or a lowered one.
    if (base_limit >= seg_k_width || base_limit >= seg_k_width * sparse_num / sparse_den) {
        uint64_t sb = seg_k_width / 8, p2 = 1;
        while (p2 * 2 <= sb) p2 *= 2;
        if (p2 != sb) {
            if (opt.segment_width_set) P.seg_unrounded_k = seg_k_width;
            seg_k_width = std::max<uint64_t>(64, p2 * 8);
        }
    }

    // med64/medium cutoff: primes in [small_limit, med64_limit) go to the
    // med64 tier (SegmentSieve::process_med64) -- a band close to
    // small_limit, not the whole medium tier, whose population keeps growing
    // with N
    // (docs/RESEARCH.md#medium-tier-64-list-restructuring-scoped-to-a-bounded-sub-band-med64_primes-kept-2026-09-26).
    // 1/6 of the segment. The whole segment on a small-L2 core, where the
    // medium tier's table-driven stepping costs more than med64's lists
    // (docs/RESEARCH.md#i5-3470-profile-at-1e12-the-med64-tier-over-the-whole-l2-segment-is-59-of-the-cycles-open-2026-10-04).
    // --tune med64=a/b overrides; 0 disables the tier.
    uint64_t med64_num = 1, med64_den = 6;
    if (opt.tune_med64.den) { med64_num = opt.tune_med64.num; med64_den = opt.tune_med64.den; }
    else if (small_l2_core) { med64_den = 1; P.med64_l2_gate = true; }

    // The base primes into tiers by expected hits (segment_sieve.hpp):
    //   - small (p < small_limit): many hits per L1 sub-block, crossed off
    //     one sub-block at a time so the marks land in L1;
    //   - med64 (small_limit <= p < med64_limit): many hits per segment,
    //     over the whole segment;
    //   - medium (med64_limit <= p < sparse_limit): a few hits per segment;
    //   - sparse (p >= sparse_limit): about one hit per segment or fewer,
    //     the bucket ring.
    // The pre-sieved primes (7..163, presieve.hpp) are in no tier: their
    // multiples come pre-marked, and nothing marks the primes themselves.
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
    P.seg_k_width = seg_k_width;
    P.wide.width = seg_k_width;
    classify(P.wide, seg_k_width * med64_num / med64_den, seg_k_width * sparse_num / sparse_den);

    // Narrow segment for the chunks below narrow^2. The doubled segment only
    // pays where sparse primes are active, and a chunk below narrow^2 has
    // no active prime >= narrow (activation is by p^2), so it runs the
    // non-sparse configuration: the narrow segment, the 1/1 cutoff,
    // med64_limit on the narrow width (its sparse list never activates).
    // narrow is half the fixed-up wide width, a power of 2 in bytes. See
    // docs/RESEARCH.md#narrow-segment-for-the-chunks-below-narrow-squared-kept-2026-09-28.
    TierSet& narrow = P.narrow;
    narrow.width = seg_k_width / 2;
    if (!opt.segment_width_set && sparse_regime && narrow.width >= 64 && narrow.width % 64 == 0) {
        const uint64_t k_end = wheel_count_upto(std::min(opt.limit, narrow.width * narrow.width));
        // A --start tail beginning past narrow^2 (split_ranges' first k) has
        // no narrow chunk: skip a second pass over every base prime.
        const uint64_t first_k = opt.start ? wheel_count_upto(opt.start - 1) / 64 * 64 : 0;
        if (first_k < k_end) {
            classify(narrow, narrow.width * med64_num / med64_den, narrow.width);
            P.narrow_k_end = k_end;
        }
    }

    // Steal threshold until the run has measured its own activation cost and
    // rates (run_parallel_chunks): a steal activates every base prime once
    // for the stolen piece, about as much as sieving 2.6 wheel indices per
    // prime when every thread activates at once. At least 4 per base prime.
    constexpr uint64_t STEAL_K_PER_BASE_PRIME = 4;
    cfg.steal_min_k = STEAL_K_PER_BASE_PRIME * base.count;

    // Medium-tier prefetchnta gate: on once the medium state outgrows the
    // per-thread L3 share, where it comes from DRAM anyway and keeping it
    // out of L2 only protects the segment. The threshold counts 8 bytes per
    // prime, as it was measured (the state is 5 now). See
    // docs/RESEARCH.md#cross_off_medium-struct-of-arrays-state--gated-prefetchnta-kept-2026-09-29.
    // Undetected L3 -> 1 MiB.
    {
        uint64_t l3_share = l3.sharers > 0 ? l3.total_bytes / static_cast<uint64_t>(l3.sharers) : 0;
        if (l3_share == 0) l3_share = 1024 * 1024;
        cfg.medium_nta_min_primes = l3_share / 8;
        // --tune medium_nta=1|0 overrides the gate: under a VM the detected
        // L3 can be the whole host's (260 MB on a 2-vCPU KVM guest).
        if (opt.medium_nta >= 0) cfg.medium_nta_min_primes = opt.medium_nta ? 0 : UINT64_MAX;
    }

    P.base_count = base.count;
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
                 static_cast<unsigned long long>(P.seg_k_width * WHEEL_MOD / WHEEL_SIZE),
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
        std::fprintf(stderr, "  segment: %llu KiB instead of %llu KiB (cap: 32 x L1d; half of an L2 of %llu KiB)\n",
                     static_cast<unsigned long long>(P.seg_k_width / 8 / 1024),
                     static_cast<unsigned long long>(P.seg_l1_capped_k / 8 / 1024),
                     static_cast<unsigned long long>(P.seg_l1_capped_k / 8 * 2 / 1024));
    if (P.seg_uncapped_k)
        std::fprintf(stderr, "  segment: %llu KiB instead of %llu KiB (ceiling: the L2 per thread, 16-32 x L1d)\n",
                     static_cast<unsigned long long>(P.seg_k_width / 8 / 1024),
                     static_cast<unsigned long long>(P.seg_uncapped_k / 8 / 1024));
    if (P.seg_max_capped_k)
        std::fprintf(stderr, "  segment: %llu KiB instead of %llu KiB (the widest the tiers support)\n",
                     static_cast<unsigned long long>(P.seg_k_width / 8 / 1024),
                     static_cast<unsigned long long>(P.seg_max_capped_k / 8 / 1024));
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
    if (P.narrow_k_end) {
        unsigned narrow_chunks = 0;
        for (const auto& r : ranges) narrow_chunks += r.high <= P.narrow_k_end;
        std::fprintf(stderr, "  narrow segment (%llu) up to %llu: %u of %u chunks\n",
                     static_cast<unsigned long long>(P.narrow.width * WHEEL_MOD / WHEEL_SIZE),
                     static_cast<unsigned long long>(std::min(opt.limit, P.narrow.width * P.narrow.width)),
                     narrow_chunks, static_cast<unsigned>(ranges.size()));
    }
}
