#pragma once
// Segmented sieve on a compile-time wheel (see wheel.hpp), bit-packed into
// uint64_t words. Four prime tiers by expected hits per segment -- small,
// med64, medium, sparse (cutoffs computed in tuning.hpp) -- mirroring
// primesieve's own EratSmall/EratMedium/EratBig split, plus this project's
// own med64 sub-band. See docs/ALGORITHM.md §6 for how and why each tier
// works the way it does, and docs/RESEARCH.md for every tried-and-reverted
// alternative along the way (64-list medium restructurings, dTLB pressure,
// sparse-tier stepping-math variants, the 7-byte SparseEntry attempt).
//
// Extraction (turning the finished bit array into actual prime values):
// invert each word, decompose into (q, r) = (k / WHEEL_SIZE, k %
// WHEEL_SIZE) once per word, then walk set bits with ctz + clear-lowest-bit.

#include <array>
#include <cstdint>
#include <vector>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <utility>

#include "base_sieve.hpp"
#include "erat_small.hpp"
#include "presieve.hpp"
#include "wheel.hpp"
#include "wheel210_big.hpp"

// Sparse tier: bucket entries process_big takes per iteration (see its
// comment); -DERA_BIG_UNROLL=1 restores one per iteration for an A/B.
// Sparse tier: process_big prefetches the segment byte of the entries this
// many positions ahead in the bucket (see its comment); 0 turns it off.
// Medium tier in predicated bands (erat_small.hpp::cross_off_medium_banded):
// -DERA_MED_BANDS=1 for an A/B on CPUs where bad speculation dominates.
#ifndef ERA_MED_BANDS
#define ERA_MED_BANDS 0
#endif
// Medium tier two primes per iteration (erat_small.hpp::cross_off_medium_pairs):
// -DERA_MED_PAIRS=1 for an A/B (`make variant DEFS=-DERA_MED_PAIRS=1`).
#ifndef ERA_MED_PAIRS
#define ERA_MED_PAIRS 0
#endif
constexpr double MEDIUM_BAND_FACTOR = 1.2;
constexpr double MEDIUM_BAND_MAX_HITS = 8.0;

#ifndef ERA_BIG_PF
#define ERA_BIG_PF 16
#endif
// Sparse tier: a prime with more hits in the current segment marks them in a
// loop before being re-filed (EratBig's loop), instead of one re-file per hit
// (process_big). -DERA_BIG_LOOP=1 for the A/B (`make variant DEFS=-DERA_BIG_LOOP=1`).
#ifndef ERA_BIG_LOOP
#define ERA_BIG_LOOP 0
#endif
// Sparse tier: also prefetch the push target (the tail block of the slot the
// entry's next hit files into) ERA_BIG_PF entries ahead. -DERA_BIG_PFPUSH=1.
#ifndef ERA_BIG_PFPUSH
#define ERA_BIG_PFPUSH 0
#endif
// Sparse tier: the next block's prefetch spread over the first half of the
// current block (one line per 4 entries, default) instead of 64 prefetcht1
// in a burst at the block boundary (-DERA_BIG_PFSPREAD=0 for the A/B). The
// burst filled the core's miss queue and stalled on it: 22% of process_big's
// cycles sat on that prefetch loop (dev PC, perf annotate, 1e15 tail).
// Spread: -4.8..-5.3% at the 1e14-1e17 tails with 12 threads, -6.6% at
// 1e15 with one thread per core, 3/3 each; cycles:u -3..-4.4% with +3.4%
// instructions. See docs/RESEARCH.md#sparse-tier-next-block-prefetch-spread-over-the-current-block-kept-2026-10-04.
#ifndef ERA_BIG_PFSPREAD
#define ERA_BIG_PFSPREAD 1
#endif
// Sparse activation experiments (--debug-idle prints the per-prime cost):
// ERA_FPDIV=1 computes the first multiplier with a double division plus an
// exact fixup instead of a 64-bit integer division (slow on pre-Ice-Lake
// cores: ~40-90 cycles on Nehalem/Ivy Bridge, ~15 on Rocket Lake, where it
// was neutral-to-slower). ERA_ACT_BATCH=1 stages the activation's pushes
// per group of 64 ring slots and drains a group when it fills, so the
// ring's thousands of tail lines are touched in L1-sized groups instead of
// one random RFO per prime. -DERA_FPDIV=1 / -DERA_ACT_BATCH=1.
#ifndef ERA_FPDIV
#define ERA_FPDIV 0
#endif
#ifndef ERA_ACT_BATCH
#define ERA_ACT_BATCH 0
#endif
// Sparse tier: with one thread per core the bucket arenas are 2 MiB regions
// advised MADV_HUGEPAGE (SegmentSieve's huge_arenas, decided in tuning.hpp),
// so the ring's active write set (slots x block: 4 MiB at the 1e18 tail, a
// thousand 4 KiB pages) costs a couple of TLB entries. Dev PC, 2 threads:
// -2.9% on the 1e17 tail (4/4), -3.5% on 1e18, -12% on the 1e18 tail with a
// 1e10 window (the activation of 50M primes files into that write set); 6
// threads neutral; 12 threads (HT pairs) +10.5% worse (6/6), hence the gate.
// Best effort: THP in "madvise" (Ubuntu's default) or "always" mode.
#include <sys/mman.h>
#ifndef ERA_BIG_UNROLL
#define ERA_BIG_UNROLL 2
#endif

class SegmentSieve {
public:
    // seg_k_width: wheel-index width of a normal (non-final) segment, same
    // as passed to sieve_and_emit's k_high-k_low in every call but the
    // last of a chunk.
    // base_prime_max: the largest base prime this sieve will ever be given
    // (i.e. isqrt(limit)) -- used to size the bucket ring generously
    // enough that no sparse prime's skip between hits can ever wrap around
    // it.
    // presieve: shared, read-only pre-sieve pattern (see presieve.hpp) used
    // to fill each segment instead of zeroing it; its primes must already
    // be excluded from small/medium/sparse primes by the caller.
    // sub_block_bytes: L1-sized slice the small tier (and presieve fill)
    // is run over, one slice at a time -- see sieve_and_emit.
    // has_sparse: true when this run actually has any sparse-tier primes.
    // The new EratBig-style tier below needs seg_k_width/8 (the segment
    // width in BYTES) to be a power of 2 so its bucket-slot math is a
    // shift/mask instead of a division -- tuning.hpp is responsible for
    // flooring seg_k_width to the nearest power of 2 (in bytes) whenever
    // has_sparse is true; this constructor just verifies that was done.
    // m64s_limit: med64 primes below it are crossed off one L1 sub-block at
    // a time (process_med64s), like the small tier; 0 turns that band off.
    SegmentSieve(uint64_t seg_k_width, uint64_t base_prime_max, const Presieve& presieve,
                 uint64_t sub_block_bytes, uint64_t m64s_limit, bool has_sparse, bool medium_nta, bool big2310,
                 bool huge_arenas = false)
        : words_((seg_k_width + 63) / 64 + 1, 0), // +1 word: s[bytes_needed] is the banded medium tier's spare byte
          seg_k_width_(seg_k_width),
          sub_block_bytes_(sub_block_bytes),
          medium_nta_(medium_nta),
          big2310_(big2310),
          presieve_(presieve),
          arena_bytes_(huge_arenas ? (size_t{2} << 20) : BLK_BYTES * 256),
          huge_arenas_(huge_arenas),
          m64s_limit_(m64s_limit) {
        // The byte-addressed dense tiers (erat_small.hpp) need every
        // segment to start on a byte (k multiple of 8) and to stay a whole
        // number of words; callers align chunk starts to 64 too.
        if (seg_k_width % 64 != 0 || sub_block_bytes % 8 != 0 || sub_block_bytes == 0) {
            throw std::runtime_error("SegmentSieve: segment/sub-block width not aligned");
        }
        // Dense primes are p < seg_k_width, so their pending hit stays
        // within a few segment widths of the segment start (fits
        // DenseState::pos), and p / 30 has to fit its packed qp field.
        if (seg_k_width / WHEEL_MOD >= erat::QP_LIMIT || seg_k_width >= (uint64_t{1} << 30)) {
            throw std::runtime_error("SegmentSieve: segment too large for the packed dense state");
        }
        // Medium tier packs a pending hit's byte position into 26 bits
        // (erat_small.hpp::cross_off_medium): at most one segment plus one
        // step of a medium prime (p < seg_k_width, step <= qp * 10 + 16).
        if (seg_k_width / 8 + seg_k_width / WHEEL_MOD * 10 + 16 >= erat::MEDIUM_POS_LIMIT) {
            throw std::runtime_error("SegmentSieve: segment too large for the medium-tier state");
        }
        uint64_t sb = seg_k_width_ / 8; // segment width in bytes
        if (has_sparse && (sb & (sb - 1))) {
            throw std::runtime_error("SegmentSieve: the sparse tier (EratBig) needs a power-of-2 segment (bytes)");
        }
        log2_sb_ = 0;
        while ((uint64_t{1} << log2_sb_) < sb) ++log2_sb_;
        // Largest BYTE step between one sparse prime's consecutive hits:
        // qp * max(dm) + max(corr), with max(dm) = 10 on the mod-210
        // multiplier wheel (the largest gap between consecutive 210-
        // coprime residues) -- a few extra WHEEL_SIZE's of slack (+16)
        // cost nothing (buckets are cheap) and keep this comfortably safe.
        // mod-2310 (big2310_): max(dm) = 14, and the packed entry (see
        // activation) holds pos in 24 bits and qp in 28.
        if (big2310_ && (log2_sb_ > 24 || base_prime_max / WHEEL_MOD >= (uint64_t{1} << 28))) {
            throw std::runtime_error("SegmentSieve: segment or base prime too large for the mod-2310 sparse tier");
        }
        // mod-210 (--tune big2310=0): qw = (qp << 9) | idx in 32 bits, so qp
        // < 2^23, i.e. p < ~2.5e8 and N < ~6.3e16 -- this wrapped silently.
        if (!big2310_ && base_prime_max / WHEEL_MOD >= (uint64_t{1} << 23)) {
            throw std::runtime_error("SegmentSieve: base prime too large for the mod-210 sparse tier "
                                     "(N must be below ~6.3e16 with --tune big2310=0)");
        }
        uint64_t maxstep = base_prime_max / WHEEL_MOD * (big2310_ ? 14 : 10) + 16;
        uint64_t ahead = (maxstep >> log2_sb_) + 2;
        num_buckets_ = 1;
        while (num_buckets_ < ahead * 2) num_buckets_ <<= 1; // power of 2, 2x margin
        // Twice num_buckets_ slots, so a hit's slot is plainly cur_segment_ +
        // ahead (ahead < num_buckets_, cur_segment_ < num_buckets_) and the
        // hot loop has no wrap mask; wrap_ring() shifts the upper half down
        // once the cursor reaches num_buckets_. See process_big.
        head_.assign(2 * num_buckets_, nullptr);
        tail_.assign(2 * num_buckets_, nullptr);
        if (ERA_ACT_BATCH) {
            act_groups_.resize(static_cast<size_t>(std::max<uint64_t>(1, (2 * num_buckets_) >> 6)));
            for (std::vector<ActEnt>& g : act_groups_) g.reserve(ACT_GROUP_CAP);
        }
    }

    // Wheel indices below k are marked composite before extraction (the
    // number 1, and the numbers below a --start that split_ranges rounded
    // down to). Default 1.
    void set_skip_below_k(uint64_t k) { skip_below_k_ = std::max<uint64_t>(k, 1); }

    // Must be called once before the first sieve_and_emit call for a new,
    // independent run of consecutive segments in increasing k order (a
    // thread's chunk). Resets all bucket/flat-tier state and the "which
    // primes have activated yet" pointers. Never reuse a SegmentSieve
    // across threads or out of order.
    void begin_chunk() {
        cur_segment_ = 0;
        next_small_idx_ = 0;
        next_med64_idx_ = 0;
        next_medium_idx_ = 0;
        next_sparse_k_ = 0; // activate() starts from the run's own first index
        for (auto& v : small_) v.clear();
        for (auto& v : m64_cur_) v.clear();
        for (auto& v : m64_nxt_) v.clear();
        for (auto& v : m64s_cur_) v.clear();
        for (auto& v : m64s_nxt_) v.clear();
        m64s_count_ = 0;
        for (auto& v : medium_dyn_) v.clear();
        for (auto& v : medium_qd_) v.clear();
        for (auto& v : medium_bands_) v.clear();
        std::fill(head_.begin(), head_.end(), nullptr);
        std::fill(tail_.begin(), tail_.end(), nullptr);
        // Blocks aren't freed, just handed back to the pool: every block
        // ever allocated for this SegmentSieve is reusable, so repopulate
        // the free list from scratch rather than reallocate.
        free_.clear();
        for (auto& c : chunks_)
            for (size_t i = 0; i < arena_bytes_ / BLK_BYTES; ++i)
                free_.push_back(reinterpret_cast<Blk*>(c.get() + i * BLK_BYTES));
    }

    // Activates every base prime that becomes relevant in [k_low, k_high)
    // (p*p below its end) and returns how many: run by sieve_and_emit on
    // every segment, and by main.cpp's sieve_chunk on its own (timed) at the
    // start of a chunk that doesn't carry on from the previous one, where it
    // activates every base prime up to sqrt(k_high's number) at once.
    size_t activate(uint64_t k_low, uint64_t k_high,
                    const std::vector<uint64_t>& small_primes,
                    const std::vector<uint64_t>& med64_primes,
                    const std::vector<uint64_t>& medium_primes,
                    const SparsePrimes& sparse_primes) {
        const size_t before = next_small_idx_ + next_med64_idx_ + next_medium_idx_;
        uint64_t high_n = wheel_number(k_high); // exclusive numeric bound, valid for the p*p cutoff
        uint64_t low_n = wheel_number(k_low);

        // Activate any base primes that just became relevant (p*p < high_n).
        // Each of the four lists is sorted by p and gets its own
        // monotonically-advancing pointer, so every prime is visited here
        // exactly once for the whole chunk, not once per segment.
        //
        // med64_'s own lists reserve capacity once, on this SegmentSieve's
        // very first activation (not per chunk -- a vector's capacity
        // survives clear(), and main.cpp's sieve_chunk reuses one instance
        // per thread across chunks), to avoid growing 384 small vectors one
        // push_back at a time while hot. Distribution across (class, phase)
        // is expected to be close to uniform, not exact, hence the margin.
        if (!med64_reserved_ && !med64_primes.empty()) {
            size_t per_list = med64_primes.size() / 384 * 2 + 16;
            for (auto& v : m64_cur_) v.reserve(per_list);
            for (auto& v : m64_nxt_) v.reserve(per_list);
            for (auto& v : m64s_cur_) v.reserve(per_list);
            for (auto& v : m64s_nxt_) v.reserve(per_list);
            med64_reserved_ = true;
        }
        activate_dense(small_primes, next_small_idx_, small_, high_n, low_n, k_low);
        activate_med64(med64_primes, next_med64_idx_, m64_cur_.data(), m64s_cur_.data(), m64s_limit_, m64s_count_,
                       high_n, low_n, k_low);
        activate_medium(medium_primes, next_medium_idx_, medium_dyn_, medium_qd_, medium_qp_base_, medium_qp_last_,
                        medium_bands_, seg_k_width_ / 8, high_n, low_n, k_low);

        // Sparse tier: its primes are a run of the base-prime bitmap
        // (base_sieve.hpp's SparsePrimes), walked in increasing order from
        // next_sparse_k_ until p*p reaches this segment's end; file_sparse
        // files each one into the bucket ring.
        size_t sparse_activated = 0;
        if (next_sparse_k_ < sparse_primes.k_begin) next_sparse_k_ = sparse_primes.k_begin;
        while (next_sparse_k_ < sparse_primes.k_end) {
            const uint64_t wi = next_sparse_k_ >> 6;
            const uint64_t word_end = std::min((wi + 1) << 6, sparse_primes.k_end);
            uint64_t bits = sparse_primes.words[wi] & (~uint64_t{0} << (next_sparse_k_ & 63));
            bool reached = false; // p*p >= high_n: the rest activate in a later segment
            while (bits) {
                const uint64_t k = (wi << 6) + static_cast<uint64_t>(__builtin_ctzll(bits));
                if (k >= word_end) break;
                const uint64_t p = wheel_number(k);
                if (p * p >= high_n) {
                    next_sparse_k_ = k;
                    reached = true;
                    break;
                }
                file_sparse(p, low_n, k_low);
                ++sparse_activated;
                bits &= bits - 1;
            }
            if (reached) break;
            next_sparse_k_ = word_end;
        }
        flush_activation(); // ERA_ACT_BATCH: everything staged lands before this segment is sieved
        return next_small_idx_ + next_med64_idx_ + next_medium_idx_ - before + sparse_activated;
    }

    template <typename Writer>
    void sieve_and_emit(uint64_t k_low, uint64_t k_high,
                         const std::vector<uint64_t>& small_primes,
                         const std::vector<uint64_t>& med64_primes,
                         const std::vector<uint64_t>& medium_primes,
                         const SparsePrimes& sparse_primes,
                         Writer& out, uint64_t& prime_count) {
        uint64_t count = (k_high > k_low) ? (k_high - k_low) : 0;
        if (count == 0) return;

        uint64_t bytes_needed = (count + 7) / 8;

        activate(k_low, k_high, small_primes, med64_primes, medium_primes, sparse_primes);

        // Small tier, one L1-sized sub-block at a time: presieve fill,
        // then every small prime crossed off inside that sub-block while
        // it's still L1-resident, instead of each prime sweeping the whole
        // (L2-sized) segment. Pending hits stay relative to the segment's
        // first byte until the last sub-block rebases them.
        uint8_t* bytes = reinterpret_cast<uint8_t*>(words_.data());
        for (uint64_t sb = 0; sb < bytes_needed; sb += sub_block_bytes_) {
            uint64_t se = std::min(sb + sub_block_bytes_, bytes_needed);
            uint64_t sb_bit = sb * 8;
            presieve_.fill(words_.data() + sb / 8, k_low + sb_bit, std::min<uint64_t>(count - sb_bit, (se - sb) * 8));
            uint64_t rebase = (se == bytes_needed) ? bytes_needed : 0;
            erat::cross_off_class<0>(bytes, se, small_[0].data(), small_[0].data() + small_[0].size(), rebase);
            erat::cross_off_class<1>(bytes, se, small_[1].data(), small_[1].data() + small_[1].size(), rebase);
            erat::cross_off_class<2>(bytes, se, small_[2].data(), small_[2].data() + small_[2].size(), rebase);
            erat::cross_off_class<3>(bytes, se, small_[3].data(), small_[3].data() + small_[3].size(), rebase);
            erat::cross_off_class<4>(bytes, se, small_[4].data(), small_[4].data() + small_[4].size(), rebase);
            erat::cross_off_class<5>(bytes, se, small_[5].data(), small_[5].data() + small_[5].size(), rebase);
            erat::cross_off_class<6>(bytes, se, small_[6].data(), small_[6].data() + small_[6].size(), rebase);
            erat::cross_off_class<7>(bytes, se, small_[7].data(), small_[7].data() + small_[7].size(), rebase);
            // The sub-blocked med64 band, while the sub-block is still in L1.
            if (m64s_count_) run_med64s(bytes, se, rebase);
        }
        // Wheel indices below skip_below_k_ are not part of the range: index
        // 0 (the number 1, nothing marks it) and, with --start, the head of
        // the first word (split_ranges rounds the start down to a multiple
        // of 64 indices; without this the primes in it were counted -- 3
        // extra at the 2e16 tail from 19999900000000000, found 2026-10-04).
        if (k_low < skip_below_k_) {
            for (uint64_t k = k_low; k < std::min(skip_below_k_, k_high); ++k)
                words_[(k - k_low) >> 6] |= uint64_t{1} << ((k - k_low) & 63);
        }

        // med64 tier (see docs/RESEARCH.md and this class's header comment
        // for why this is scoped to a bounded sub-band rather than the
        // whole medium tier): byte marking like the small tier, but one
        // pass over the WHOLE segment (no sub-blocking -- this tier's
        // point is amortizing cross_off<PR>'s entry/exit cost over many
        // hits, not L1 residency for a huge population). process_med64<PR>
        // reads this segment's due entries from m64_cur_ and pushes each
        // one's next hit into m64_nxt_, grouped by its OUTGOING phase so
        // next segment's calls again share one entry phase per list; the
        // segment-wide rebase (pos -= bytes_needed) matches cross_off_class's
        // own end-of-sub-block rebase above. Skipped entirely when this
        // run has no med64 primes, matching the sparse tier's own
        // skip-when-empty guard below.
        if (!med64_primes.empty()) {
            run_med64(bytes, bytes_needed);
            for (auto& v : m64_cur_) v.clear();
            std::swap(m64_cur_, m64_nxt_);
        }

        // Medium tier: one pass over the whole segment each, one list per
        // residue class so PR is a compile-time template parameter in
        // cross_off_medium<PR>, same reasoning as the small tier's
        // cross_off_class<PR> calls just above; NTA picked per TierSet (see
        // tuning.hpp's SieveConfig::medium_nta_min_primes).
        if (medium_nta_) run_medium<true>(bytes, bytes_needed);
        else run_medium<false>(bytes, bytes_needed);

        // Sparse tier: see process_sparse_bucket below (pulled out of this
        // function on purpose -- see its own comment). sparse_primes is
        // either empty for the whole run or not -- never changes segment
        // to segment -- so skipping the call entirely when it's empty
        // avoids paying a real (non-inlined) call's overhead every single
        // segment for N where this tier never has anything to do (every N
        // tested up to 1e12 on this machine, see README#benchmarks).
        if (!sparse_primes.empty()) {
            if (big2310_) process_big<true>();
            else process_big<false>();
        }
        if (++cur_segment_ == num_buckets_) wrap_ring();

        // Extraction: bit=0 => prime candidate. Three paths, by what the
        // sink can take (see below).
        if constexpr (!Writer::WANTS_VALUES) prime_count += count_primes(count);
        else if constexpr (requires { out.write_k(uint64_t{0}); }) prime_count += emit_indices(k_low, count, out);
        else prime_count += emit_values(k_low, count, out);
    }

private:
    // count-only (NullSink): a plain popcount over the full words, four
    // independent accumulators, and the partial last word masked once
    // after the loop. A generic loop that checked "last word?" and "zero
    // word?" on every word was kept by GCC as a cmove chain on the running
    // sum: ~13 instructions per word instead of ~4, ~60% of sieve_chunk's
    // own cycles on the i5-13500 (perf annotate, 1e14 tail).
    uint64_t count_primes(uint64_t count) const {
        const uint64_t* wp = words_.data();
        const size_t full = count / 64;
        uint64_t c0 = 0, c1 = 0, c2 = 0, c3 = 0;
        size_t w = 0;
        for (; w + 4 <= full; w += 4) {
            c0 += static_cast<uint64_t>(__builtin_popcountll(wp[w]));
            c1 += static_cast<uint64_t>(__builtin_popcountll(wp[w + 1]));
            c2 += static_cast<uint64_t>(__builtin_popcountll(wp[w + 2]));
            c3 += static_cast<uint64_t>(__builtin_popcountll(wp[w + 3]));
        }
        for (; w < full; ++w) c0 += static_cast<uint64_t>(__builtin_popcountll(wp[w]));
        // bit = 0 => prime: zeros among the full words, then the partial word's.
        uint64_t primes = full * 64 - (c0 + c1 + c2 + c3);
        if (const uint64_t rem = count % 64)
            primes += static_cast<uint64_t>(__builtin_popcountll(~wp[full] & ((uint64_t{1} << rem) - 1)));
        return primes;
    }

    // Sinks that take wheel indices (GapBlockSink::write_k): a prime's index
    // is k_low plus its bit position, no value to rebuild. The count is
    // accumulated in a local and returned (a reference into the caller's
    // frame could alias the sink's state; a local stays in a register).
    template <typename Writer>
    uint64_t emit_indices(uint64_t k_low, uint64_t count, Writer& out) const {
        const size_t words_needed = (count + 63) / 64;
        uint64_t n = 0;
        for (size_t w = 0; w < words_needed; ++w) {
            uint64_t bits = ~words_[w];
            uint64_t remaining = count - w * 64ULL; // >= 1 for every w < words_needed
            if (remaining < 64) bits &= (1ULL << remaining) - 1ULL;
            const uint64_t k_word = k_low + w * 64ULL;
            while (bits) {
                out.write_k(k_word + static_cast<uint64_t>(__builtin_ctzll(bits)));
                ++n;
                bits &= bits - 1;
            }
        }
        return n;
    }

    // Value sinks: invert each word, decompose into (q, r) = (k / WHEEL_SIZE,
    // k % WHEEL_SIZE) once per word, then walk the set bits with ctz +
    // clear-lowest-bit, stepping (q, r) by the bit distance.
    template <typename Writer>
    uint64_t emit_values(uint64_t k_low, uint64_t count, Writer& out) const {
        const size_t words_needed = (count + 63) / 64;
        uint64_t n = 0;
        for (size_t w = 0; w < words_needed; ++w) {
            uint64_t bits = ~words_[w];
            uint64_t base_idx = w * 64ULL;
            uint64_t remaining = count - base_idx;
            if (remaining < 64) {
                bits &= (remaining == 0) ? 0ULL : ((1ULL << remaining) - 1ULL);
            }
            if (bits == 0) continue;

            uint64_t k_word_start = k_low + base_idx;
            uint64_t q = k_word_start / WHEEL_SIZE;
            uint64_t r = k_word_start % WHEEL_SIZE;

            uint64_t prev_bit = 0;
            while (bits) {
                uint64_t bit_pos = static_cast<uint64_t>(__builtin_ctzll(bits));
                uint64_t step2 = bit_pos - prev_bit;
                prev_bit = bit_pos;
                if constexpr (WHEEL_SIZE_IS_POW2) {
                    uint64_t rq = r + step2;
                    q += rq >> WHEEL_SIZE_LOG2;
                    r = rq & (static_cast<uint64_t>(WHEEL_SIZE) - 1);
                } else {
                    r += step2;
                    while (r >= static_cast<uint64_t>(WHEEL_SIZE)) { r -= WHEEL_SIZE; ++q; }
                }

                uint64_t value = q * WHEEL_MOD + WHEEL_R[r];
                out.write_uint64(value);
                ++n;
                bits &= bits - 1; // clear the lowest set bit
            }
        }
        return n;
    }

    // Small tier: activates (appends state for) every prime in `primes`
    // from `next` on whose square falls below this segment's end; `primes`
    // is sorted, so this touches each prime exactly once per chunk. Byte
    // positions, one output list per residue class (state[pr]).
    __attribute__((noinline)) static void activate_dense(const std::vector<uint64_t>& primes, size_t& next,
                               std::vector<erat::DenseState>* state,
                               uint64_t high_n, uint64_t low_n, uint64_t k_low) {
        while (next < primes.size()) {
            uint64_t p = primes[next];
            if (p * p >= high_n) break;
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            // Small tier: smallest m coprime with WHEEL_MOD (30) with
            // p*m >= start_val -- byte position, (qp<<6)|(pr<<3)|j
            // packing (erat_small.hpp::cross_off_class).
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;
            uint64_t pos = (p * m) / WHEEL_MOD - k_low / 8;
            uint64_t j = static_cast<uint64_t>(WHEEL_POS[r]);
            state[pr].push_back({static_cast<uint32_t>(((p / WHEEL_MOD) << 6) | (pr << 3) | j),
                                 static_cast<uint32_t>(pos)});
            ++next;
        }
    }

    // med64 tier, on the mod-210 multiplier wheel: activate_medium's start
    // derivation (smallest m coprime with 210, byte position), filed under
    // state[pr*48+w] with qw = (qp << 6) | w, so every
    // cross_off_checked210<PR> call in one inner loop shares entry phase w.
    // (A mod-30 version with 64 lists was the default until 2026-09-30; see
    // docs/RESEARCH.md.)
    // Primes below m64s_limit go to the sub-blocked band's lists (state384s)
    // instead, counted in m64s_count.
    __attribute__((noinline)) static void activate_med64(const std::vector<uint64_t>& primes, size_t& next,
                                   std::vector<erat::DenseState>* state384,
                                   std::vector<erat::DenseState>* state384s, uint64_t m64s_limit, size_t& m64s_count,
                                   uint64_t high_n, uint64_t low_n, uint64_t k_low) {
        while (next < primes.size()) {
            uint64_t p = primes[next];
            if (p * p >= high_n) break;
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            uint64_t m0 = (start_val + p - 1) / p;
            uint64_t t = m0 / 210, sres = m0 % 210;
            uint32_t w = big::NEXT_W[sres];
            if (w == 48) { ++t; w = 0; }
            uint64_t m = t * 210 + big::M210[w];
            uint64_t pos = (p * m) / WHEEL_MOD - k_low / 8;
            std::vector<erat::DenseState>* target = state384;
            if (p < m64s_limit) { target = state384s; ++m64s_count; }
            target[pr * 48 + w].push_back({static_cast<uint32_t>(((p / WHEEL_MOD) << 6) | w),
                                           static_cast<uint32_t>(pos)});
            ++next;
        }
    }

    // Medium tier: smallest m coprime with 210 (not just 30) with
    // p*m >= start_val -- every medium prime is > 163, so multiples of 7
    // are always redundant here (see erat_small.hpp::cross_off_medium).
    // Byte position, (qp<<6)|w packing, one list per residue class
    // (medium_[pr]) so cross_off_medium<PR> gets PR as a compile-time
    // template parameter -- same shape as activate_dense's small-tier
    // branch above, just mod-210 stepping instead of mod-30. Same t/w
    // decomposition as the sparse tier's own EratBig-style activation.
    //
    // qp goes in as a 1-byte delta from the class's previous prime (see
    // cross_off_medium); the first prime of a class sets qp_base.
    //
    // Bands (erat_small.hpp::MedBand, ERA_MED_BANDS): a prime's fixed
    // iteration count is its expected hits per segment (6.857 * segment
    // bytes / p: a 210-multiplier cycle is 7p bytes and 48 hits) times
    // MEDIUM_BAND_FACTOR, plus one; above MEDIUM_BAND_MAX_HITS expected,
    // h = 0 (plain loop). Consecutive primes with the same h share a band.
    __attribute__((noinline)) static void activate_medium(const std::vector<uint64_t>& primes, size_t& next,
                                 std::vector<uint32_t>* dyn, std::vector<uint8_t>* qds,
                                 uint32_t* qp_base, uint32_t* qp_last,
                                 std::vector<erat::MedBand>* bands, uint64_t seg_bytes,
                                 uint64_t high_n, uint64_t low_n, uint64_t k_low) {
        while (next < primes.size()) {
            uint64_t p = primes[next];
            if (p * p >= high_n) break;
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            if constexpr (ERA_MED_BANDS) {
                uint8_t h = 0;
                const double mean = 6.857 * static_cast<double>(seg_bytes) / static_cast<double>(p);
                if (mean <= MEDIUM_BAND_MAX_HITS) h = static_cast<uint8_t>(std::min(250.0, mean * MEDIUM_BAND_FACTOR) + 1);
                if (bands[pr].empty() || bands[pr].back().h != h) bands[pr].push_back({static_cast<uint32_t>(dyn[pr].size() + 1), h});
                else bands[pr].back().end = static_cast<uint32_t>(dyn[pr].size() + 1);
            } else {
                (void)bands; (void)seg_bytes;
            }
            uint64_t m0 = (start_val + p - 1) / p;
            uint64_t t = m0 / 210, sres = m0 % 210;
            uint32_t w = big::NEXT_W[sres];
            if (w == 48) { ++t; w = 0; }
            uint64_t m = t * 210 + big::M210[w];
            uint64_t pos = (p * m) / WHEEL_MOD - k_low / 8; // byte position, like the small tier's
            dyn[pr].push_back(static_cast<uint32_t>((pos << 6) | w));
            uint32_t qp = static_cast<uint32_t>(p / WHEEL_MOD);
            if (qds[pr].empty()) {
                qp_base[pr] = qp;
                qds[pr].push_back(0);
            } else {
                uint32_t d = qp - qp_last[pr];
                if (d > 255) {
                    throw std::runtime_error("SegmentSieve: gap between same-class medium primes > 255*30");
                }
                qds[pr].push_back(static_cast<uint8_t>(d));
            }
            qp_last[pr] = qp;
            ++next;
        }
    }

    // med64 tier (see docs/RESEARCH.md): PR a compile-time template parameter
    // like cross_off_class<PR>/cross_off_medium<PR> above, looping over that
    // class's own 48 entry-phase lists (m64_cur_[PR*48+w]) so every
    // cross_off_checked210<PR> call within one inner loop enters its switch
    // at the same phase w -- a well-predicted jump instead of a per-prime
    // dispatch. Runs over the WHOLE segment (bytes_needed), not sub-blocked
    // like the small tier: this tier's population is bounded by
    // small_limit/med64_limit (see tuning.hpp), not chasing L1 residency.
    // Each entry is read from m64_cur_, stepped, and re-filed into m64_nxt_
    // keyed by its NEW exit phase and rebased by bytes_needed --
    // sieve_and_emit clears m64_cur_ and swaps the two buffers once every
    // PR has run, so next segment reads what this one just wrote.
    //
    // prefetchnta m64_cur_ MED64_NTA_DIST entries ahead, once per entry,
    // like cross_off_medium's NTA path: the double-buffered state (~2 x 233
    // KB per thread from 5e12 up) is streamed through once per segment. Dev
    // PC: 1e13 -1.1%, 1e14 5% tail -2.6%; 1e10-1e12 unchanged (RESEARCH.md).
    template <int PR>
    void process_med64(uint8_t* bytes, uint64_t bytes_needed) {
        for (int w = 0; w < 48; ++w) {
            for (erat::DenseState& st : m64_cur_[PR * 48 + w]) {
                __builtin_prefetch(&st + MED64_NTA_DIST, 0, 0);
                uint64_t i = st.pos;
                uint64_t qp = st.qw >> 6;
                uint32_t ww = st.qw & 63;
                erat::cross_off_checked210<PR>(bytes, bytes_needed, qp, i, ww);
                m64_nxt_[PR * 48 + ww].push_back(
                    {static_cast<uint32_t>((qp << 6) | ww), static_cast<uint32_t>(i - bytes_needed)});
            }
        }
    }

    // Out of line (pinned): GCC otherwise inlines it into sieve_chunk on its
    // own when surrounding code changes.
    __attribute__((noinline)) void run_med64(uint8_t* bytes, uint64_t bytes_needed) {
        process_med64<0>(bytes, bytes_needed);
        process_med64<1>(bytes, bytes_needed);
        process_med64<2>(bytes, bytes_needed);
        process_med64<3>(bytes, bytes_needed);
        process_med64<4>(bytes, bytes_needed);
        process_med64<5>(bytes, bytes_needed);
        process_med64<6>(bytes, bytes_needed);
        process_med64<7>(bytes, bytes_needed);
    }

    // The sub-blocked med64 band: the same kernel and the same (class, phase)
    // lists as process_med64, run once per L1 sub-block [.., se) right after
    // the small tier, so every one of its marks lands in a sub-block that is
    // still L1-resident. Measured on the i5-11400F at one thread (1e13 tail,
    // perf): the whole-segment med64 took 0.81 L1 misses per hit, 3.2 cycles
    // per hit against the small tier's 1.16 -- 44% of the cycles and 67% of
    // the program's L1 misses once the cutoffs moved. Primes below
    // m64s_limit_ (about 2 x the sub-block: 14+ hits per sub-block, 64% of
    // the med64 hits for 17% of its primes) pay one list entry copy per
    // sub-block instead of one per segment; `rebase` is bytes_needed on the
    // segment's last sub-block (the entry's pos becomes relative to the next
    // segment), 0 before. An entry whose next hit is past `se` just passes
    // through (one compare, one push).
    template <int PR>
    void process_med64s(uint8_t* bytes, uint64_t se, uint64_t rebase) {
        for (int w = 0; w < 48; ++w) {
            for (erat::DenseState& st : m64s_cur_[PR * 48 + w]) {
                uint64_t i = st.pos;
                uint64_t qp = st.qw >> 6;
                uint32_t ww = st.qw & 63;
                erat::cross_off_checked210<PR>(bytes, se, qp, i, ww);
                m64s_nxt_[PR * 48 + ww].push_back(
                    {static_cast<uint32_t>((qp << 6) | ww), static_cast<uint32_t>(i - rebase)});
            }
        }
    }
    __attribute__((noinline)) void run_med64s(uint8_t* bytes, uint64_t se, uint64_t rebase) {
        process_med64s<0>(bytes, se, rebase);
        process_med64s<1>(bytes, se, rebase);
        process_med64s<2>(bytes, se, rebase);
        process_med64s<3>(bytes, se, rebase);
        process_med64s<4>(bytes, se, rebase);
        process_med64s<5>(bytes, se, rebase);
        process_med64s<6>(bytes, se, rebase);
        process_med64s<7>(bytes, se, rebase);
        for (auto& v : m64s_cur_) v.clear();
        std::swap(m64s_cur_, m64s_nxt_);
    }

    // Medium tier over the whole segment, one call per residue class; the
    // rebase is the segment's own width, like process_med64's.
    // One call per residue class PR (a compile-time template parameter in
    // erat_small.hpp's kernels, like cross_off_class<PR> for the small tier).
    template <bool NTA, int PR>
    void run_medium_class(uint8_t* bytes, uint64_t bytes_needed) {
        if constexpr (ERA_MED_BANDS) {
            erat::cross_off_medium_banded<PR, NTA>(bytes, bytes_needed, medium_dyn_[PR].data(), medium_qd_[PR].data(),
                                                   medium_qp_base_[PR], bytes_needed, medium_bands_[PR].data(),
                                                   medium_bands_[PR].data() + medium_bands_[PR].size());
        } else if constexpr (ERA_MED_PAIRS) {
            erat::cross_off_medium_pairs<PR, NTA>(bytes, bytes_needed, medium_dyn_[PR].data(),
                                                  medium_dyn_[PR].data() + medium_dyn_[PR].size(), medium_qd_[PR].data(),
                                                  medium_qp_base_[PR], bytes_needed);
        } else {
            erat::cross_off_medium<PR, NTA>(bytes, bytes_needed, medium_dyn_[PR].data(),
                                            medium_dyn_[PR].data() + medium_dyn_[PR].size(), medium_qd_[PR].data(),
                                            medium_qp_base_[PR], bytes_needed);
        }
    }
    template <bool NTA, int... PR>
    void run_medium_all(uint8_t* bytes, uint64_t bytes_needed, std::integer_sequence<int, PR...>) {
        (run_medium_class<NTA, PR>(bytes, bytes_needed), ...);
    }
    template <bool NTA>
    void run_medium(uint8_t* bytes, uint64_t bytes_needed) {
        run_medium_all<NTA>(bytes, bytes_needed, std::make_integer_sequence<int, 8>{});
    }

    // Processes exactly the sparse-tier entries due this segment: mark,
    // advance, reschedule into whichever future bucket the next hit lands
    // in. Pulled into its own noinline function to keep this tier's live
    // ranges from competing for registers with the other two tiers -- see
    // docs/RESEARCH.md#sparse-tier-process_bigprocess_sparse_bucket-split-into-its-own-noinline-function.
    // Skipped entirely by sieve_and_emit when sparse_primes is empty.
    //
    // Fixed-size blocks of erat::DenseState pulled from a pool, one queue
    // (linked list of blocks) per ring slot -- primesieve's own EratBig
    // design. The entry carries everything needed to process it (qp/pr/j
    // packed into `qw`, `pos` relative to whichever segment it's due in,
    // same layout as the dense tiers' DenseState, 8 bytes total), so
    // rescheduling COPIES the entry into the target slot's tail block
    // instead of relinking an index. Blocks of 4 KiB (512 entries): 1 KiB
    // was chosen over 8 KiB in September; 4 KiB, never tried then, is
    // -3.6..-3.9% at the 1e15 tail with 2 and with 12 threads (dev PC), a
    // tie at 1e13. See
    // docs/RESEARCH.md#sparse-tier-design-current-fixed-size-pooled-blocks-attempt-6,
    // docs/RESEARCH.md#sparse_block_entries-tuning-1024-vs-128 and
    // docs/RESEARCH.md#sparse-tier-block-size-4-kib-kept-2026-10-02.
    //
    // Drains this segment's ring slot: for each due entry, mark its one
    // hit (byte marking, mod-210 table lookup for mask/step -- see
    // wheel210_big.hpp), advance to the next hit, and re-file it by byte
    // position with a shift/mask instead of a division. `pos` in a live
    // entry is always relative to whichever segment it's due in, so no
    // k_low/k_high parameters are needed here.
    //
    // W2310 (the default; --tune big2310=0 goes back to mod-210):
    // mod-2310 multiplier wheel instead, entry packed as
    // idx | pos << 12 | qp << 36 and big::TABLE2310 (32-bit rows, next index
    // included) -- same work per hit (38 instructions vs 37), ~9.1% fewer
    // hits. See docs/RESEARCH.md#sparse-tier-mod-2310-multiplier-wheel-kept-2026-09-30.
    // Ring slots: this segment's is cur_segment_ (kept below num_buckets_,
    // see wrap_ring), a hit `ahead` segments on files into cur_segment_ +
    // ahead, always below the 2 x num_buckets_ slots allocated (the ring's
    // sizing has ahead < num_buckets_ / 2 for a re-filed hit, and
    // file_sparse checks ahead < num_buckets_ for an activation). No wrap
    // mask in the loop: `& bmask` was one of ~47 instructions per hit and
    // kept bmask live across an issue-bound loop (see docs/RESEARCH.md).
    template <bool W2310>
    __attribute__((noinline))
    void process_big() {
        const uint32_t slot = static_cast<uint32_t>(cur_segment_);
        uint8_t* const s = reinterpret_cast<uint8_t*>(words_.data());
        // Local copy of the ring's tail array base: the s[pos] byte store may
        // alias anything, so tail_.data() would otherwise be reloaded from
        // `this` on every hit.
        erat::DenseState** const tails = tail_.data();
        const uint32_t log2sb = log2_sb_;
        const uint64_t modsb = (uint64_t{1} << log2sb) - 1;
        const uint64_t cur = cur_segment_;
        while (head_[slot]) {
            Blk* blk = head_[slot];
            erat::DenseState* last_end = tail_[slot];
            head_[slot] = nullptr;
            tail_[slot] = nullptr;
            while (blk) {
                Blk* next_blk = blk->next;
                // Chain blocks are scattered in memory (LIFO free list), so
                // the hardware streamer restarts at every block boundary;
                // the next block is prefetched into L2 one block's worth of
                // work ahead -- spread over this block's first half (the
                // loop below, ERA_BIG_PFSPREAD) or, the 2026-09-27 form, as
                // a burst of 64 prefetcht1 here. See
                // docs/RESEARCH.md#sparse-tier-prefetch-the-next-block-of-the-chain-once-per-block-kept-2026-09-27
                // and docs/RESEARCH.md#sparse-tier-next-block-prefetch-spread-over-the-current-block-kept-2026-10-04.
                const char* nb = reinterpret_cast<const char*>(next_blk);
                if constexpr (!ERA_BIG_PFSPREAD) {
                    if (next_blk)
                        for (size_t off = 0; off < BLK_BYTES; off += 64) __builtin_prefetch(nb + off, 0, 2);
                }
                erat::DenseState* it = blk->entries();
                [[maybe_unused]] erat::DenseState* const blk_base = it;
                erat::DenseState* end = next_blk ? blk->block_end() : last_end;
                // One 8-byte load per entry and per table row (fields split
                // with shifts), the entry rebuilt as one 8-byte store, and the
                // new-block path out of line: the loop is load-port bound, and
                // field-by-field loads plus spills of the loop constants
                // around the inline allocation cost ~12 loads per hit.
                //
                // ERA_BIG_UNROLL entries per iteration (mod-2310 path): every
                // entry's load, table row and segment RMW is issued before any
                // push, so one entry's misses overlap the others' -- EratBig's
                // own loop shape (it takes two). The pushes stay in order: a
                // shared slot's later push reads the tail the earlier one just
                // wrote. The constexpr inner loops unroll fully at -O3.
                if constexpr (W2310 && ERA_BIG_UNROLL > 1) {
                    constexpr int U = ERA_BIG_UNROLL;
                    for (; it + U <= end; it += U) {
                        uint64_t ent[U], pos[U], te[U], e[U], sl[U];
                        if constexpr (ERA_BIG_PFSPREAD) {
                            // Line idx/4 of the next block during entries 0..255
                            // of this one: 64 lines over half a block, each
                            // requested twice (U = 2), never in a burst.
                            const size_t idx = static_cast<size_t>(it - blk_base);
                            if (next_blk && idx < BLK_BYTES / 16) __builtin_prefetch(nb + (idx >> 2) * 64, 0, 2);
                        }
                        // The segment byte of the entries ERA_BIG_PF ahead
                        // (same block, stale entries past `end` excluded):
                        // pos is the entry's own bits, no table needed, and
                        // that RMW is the load that misses once two threads
                        // share an L2 (dev PC, 12 threads: -2..-5% at the
                        // 1e15/1e18 tails; a tie with one thread per core).
                        // 16 beat 8 and 32; see docs/RESEARCH.md.
                        if constexpr (ERA_BIG_PF > 0) {
                            if (it + ERA_BIG_PF + U <= end) {
                                for (int k = 0; k < U; ++k) {
                                    uint64_t pe;
                                    std::memcpy(&pe, it + ERA_BIG_PF + k, sizeof(uint64_t));
                                    __builtin_prefetch(s + ((pe >> 12) & 0xffffff), 1, 3);
                                    if constexpr (ERA_BIG_PFPUSH) {
                                        // The push target too: the tail block of the
                                        // slot the entry's NEXT hit files into (its
                                        // table row and step, computed early), for
                                        // the regime where the ring's write set is
                                        // past L2 (1e17-1e18 tails).
                                        const uint64_t t2 = big::TABLE2310[pe & 4095];
                                        const uint64_t np = ((pe >> 12) & 0xffffff) + (pe >> 36) * ((t2 >> 8) & 0xff) + ((t2 >> 16) & 15);
                                        __builtin_prefetch(tails[cur + (np >> log2sb)], 1, 3);
                                    }
                                }
                            }
                        }
                        for (int k = 0; k < U; ++k) std::memcpy(&ent[k], it + k, sizeof(uint64_t));
                        for (int k = 0; k < U; ++k) {
                            pos[k] = (ent[k] >> 12) & 0xffffff;
                            te[k] = big::TABLE2310[ent[k] & 4095];
                        }
                        for (int k = 0; k < U; ++k) s[pos[k]] |= static_cast<uint8_t>(te[k]);
                        uint64_t nidx[U];
                        for (int k = 0; k < U; ++k) {
                            pos[k] += (ent[k] >> 36) * ((te[k] >> 8) & 0xff) + ((te[k] >> 16) & 15);
                            nidx[k] = te[k] >> 20;
                        }
                        if constexpr (ERA_BIG_LOOP) {
                            // EratBig's loop: a prime whose next hit is still in
                            // this segment marks on, instead of being re-filed
                            // into this same slot and read back later in the
                            // pass (one 8-byte copy, a tail update and maybe a
                            // new block per extra hit). With the cutoff at 1/4
                            // the primes between K/4 and K/2 have 2-4 hits per
                            // segment, ~15-20% of the tier's hits at 1e14-1e15.
                            for (int k = 0; k < U; ++k) {
                                while (pos[k] <= modsb) {
                                    const uint64_t t2 = big::TABLE2310[nidx[k]];
                                    s[pos[k]] |= static_cast<uint8_t>(t2);
                                    pos[k] += (ent[k] >> 36) * ((t2 >> 8) & 0xff) + ((t2 >> 16) & 15);
                                    nidx[k] = t2 >> 20;
                                }
                            }
                        }
                        for (int k = 0; k < U; ++k) {
                            sl[k] = cur + (pos[k] >> log2sb);
                            e[k] = (ent[k] & ~((uint64_t{1} << 36) - 1)) | nidx[k] | ((pos[k] & modsb) << 12);
                        }
                        for (int k = 0; k < U; ++k) {
                            erat::DenseState* w = tails[sl[k]];
                            if ((reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) [[unlikely]]
                                w = new_block(static_cast<uint32_t>(sl[k]));
                            std::memcpy(w, &e[k], sizeof(uint64_t));
                            tails[sl[k]] = w + 1;
                        }
                    }
                }
                for (; it != end; ++it) {
                    uint64_t ent;
                    std::memcpy(&ent, it, sizeof(ent));
                    uint64_t pos, e_keep, nidx;
                    if constexpr (W2310) { // idx | pos << 12 | qp << 36
                        uint64_t idx = ent & 4095;
                        pos = (ent >> 12) & 0xffffff;
                        uint64_t a = ent >> 36;
                        uint64_t te = big::TABLE2310[idx]; // mask | dm << 8 | corr << 16 | next << 20
                        s[pos] |= static_cast<uint8_t>(te);
                        pos += a * ((te >> 8) & 0xff) + ((te >> 16) & 15);
                        nidx = te >> 20;
                        if constexpr (ERA_BIG_LOOP) { // see the unrolled path above
                            while (pos <= modsb) {
                                const uint64_t t2 = big::TABLE2310[nidx];
                                s[pos] |= static_cast<uint8_t>(t2);
                                pos += a * ((t2 >> 8) & 0xff) + ((t2 >> 16) & 15);
                                nidx = t2 >> 20;
                            }
                        }
                        e_keep = ent & ~((uint64_t{1} << 36) - 1);
                    } else { // qw | pos << 32
                        uint64_t qw = static_cast<uint32_t>(ent);
                        pos = ent >> 32;
                        uint64_t a = qw >> 9;
                        uint64_t te = big::TABLE64[qw & 511]; // mask | dm << 8 | corr << 16 | next << 32
                        s[pos] |= static_cast<uint8_t>(te);
                        pos += a * ((te >> 8) & 0xff) + ((te >> 16) & 0xff);
                        nidx = te >> 32;
                        if constexpr (ERA_BIG_LOOP) {
                            while (pos <= modsb) {
                                const uint64_t t2 = big::TABLE64[nidx];
                                s[pos] |= static_cast<uint8_t>(t2);
                                pos += a * ((t2 >> 8) & 0xff) + ((t2 >> 16) & 0xff);
                                nidx = t2 >> 32;
                            }
                        }
                        e_keep = a << 9;
                    }
                    uint64_t sl = cur + (pos >> log2sb);
                    uint64_t e = W2310 ? (e_keep | nidx | ((pos & modsb) << 12))
                                       : (e_keep | nidx | ((pos & modsb) << 32));
                    erat::DenseState* w = tails[sl];
                    // Null (empty slot) or on a block boundary (block full).
                    if ((reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) [[unlikely]]
                        w = new_block(static_cast<uint32_t>(sl));
                    std::memcpy(w, &e, sizeof(e));
                    tails[sl] = w + 1;
                }
                free_.push_back(blk);
                blk = next_blk;
            }
        }
    }

    // Files sparse prime p (EratBig-style activation, see the header comment):
    // the smallest multiplier m coprime to 210 (2310 with big2310_) with
    // p*m >= max(p*p, low_n), packed with qp/residue class/phase into one
    // word exactly like the dense tiers' DenseState, into the bucket ring by
    // byte position (shift/mask, no division).
    void file_sparse(uint64_t p, uint64_t low_n, uint64_t k_low) {
        uint64_t start_val = std::max(p * p, low_n);
#if ERA_FPDIV
        // ceil(start_val / p) via a double quotient (53 bits: off by a unit
        // or two at 1e18) made exact by the two fixups.
        uint64_t m = static_cast<uint64_t>(static_cast<double>(start_val) / static_cast<double>(p));
        while (m * p < start_val) ++m;
        while (m > 0 && (m - 1) * p >= start_val) --m;
#else
        uint64_t m = (start_val + p - 1) / p;
#endif
        if (big2310_) {
            // Packed as one word: idx (ri * 480 + w) in bits 0-11, pos
            // in 12-35, qp in 36-63 -- see process_big<true>.
            uint64_t t = m / 2310, sres = m % 2310;
            uint64_t w = big::NEXT_W2310[sres];
            if (w == big::W2310) { ++t; w = 0; }
            m = t * 2310 + big::M2310[w];
            uint64_t pos = p * m / WHEEL_MOD - k_low / 8;
            uint64_t ri = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            uint64_t ahead = pos >> log2_sb_;
            if (ahead >= num_buckets_) {
                throw std::runtime_error(
                    "bucket sieve: a sparse prime's step exceeds the bucket ring's margin "
                    "(sizing bug in SegmentSieve's constructor)");
            }
            uint64_t ent = (ri * big::W2310 + w) | ((pos & ((uint64_t{1} << log2_sb_) - 1)) << 12) |
                           ((p / WHEEL_MOD) << 36);
            erat::DenseState e;
            std::memcpy(&e, &ent, sizeof(e));
            stage_sparse_entry(static_cast<uint32_t>(cur_segment_ + ahead), e);
            return;
        }
        uint64_t t = m / 210, sres = m % 210;
        uint64_t w = big::NEXT_W[sres];
        if (w == 48) { ++t; w = 0; }
        m = t * 210 + big::M210[w];
        uint64_t n = p * m;
        uint64_t pos = n / WHEEL_MOD - k_low / 8;
        uint64_t ri = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
        erat::DenseState e{static_cast<uint32_t>(((p / WHEEL_MOD) << 9) | (ri * 48 + w)), 0};
        uint64_t ahead = pos >> log2_sb_;
        e.pos = static_cast<uint32_t>(pos & ((uint64_t{1} << log2_sb_) - 1));
        if (ahead >= num_buckets_) {
            throw std::runtime_error(
                "bucket sieve: a sparse prime's step exceeds the bucket ring's margin "
                "(sizing bug in SegmentSieve's constructor)");
        }
        stage_sparse_entry(static_cast<uint32_t>(cur_segment_ + ahead), e);
    }

    // The ring's cursor reached num_buckets_: every slot below it has been
    // drained (process_big empties this segment's slot before moving on),
    // every pending entry sits in [num_buckets_, 2 x num_buckets_). Move
    // that half down and start the cursor over -- a few hundred pointers
    // once every num_buckets_ segments, in place of a mask on every hit.
    void wrap_ring() {
        const auto nb = static_cast<std::ptrdiff_t>(num_buckets_);
        std::copy(head_.begin() + nb, head_.end(), head_.begin());
        std::fill(head_.begin() + nb, head_.end(), nullptr);
        std::copy(tail_.begin() + nb, tail_.end(), tail_.begin());
        std::fill(tail_.begin() + nb, tail_.end(), nullptr);
        cur_segment_ = 0;
    }

    // Activation push: straight into the ring, or (ERA_ACT_BATCH) staged in
    // act_buf_ and flushed in slot groups by flush_activation().
    struct ActEnt { uint32_t slot; erat::DenseState e; };
    static constexpr size_t ACT_GROUP_CAP = 512;
    void stage_sparse_entry(uint32_t slot, erat::DenseState e) {
#if ERA_ACT_BATCH
        // One sequential append into the slot group's buffer (slot >> 6:
        // <= num_buckets_/64 groups, one hot line each); a full group is
        // pushed on the spot, 64 slots at a time, so its 64 tail lines and
        // 8 lines of tail_ stay in L1 while it drains. The first version
        // staged everything in one buffer and counting-sorted it: 4 memory
        // ops per entry, +6 ns per prime on a Xeon @2.10GHz (operator 3).
        std::vector<ActEnt>& g = act_groups_[slot >> 6];
        g.push_back({slot, e});
        if (g.size() == ACT_GROUP_CAP) flush_group(g);
#else
        push_sparse_entry(slot, e);
#endif
    }
    void flush_group(std::vector<ActEnt>& g) {
        for (const ActEnt& a : g) push_sparse_entry(a.slot, a.e);
        g.clear();
    }
    void flush_activation() {
#if ERA_ACT_BATCH
        for (std::vector<ActEnt>& g : act_groups_) if (!g.empty()) flush_group(g);
#endif
    }

    // Blocks are BLK_BYTES-aligned: a tail pointer that lands exactly on a
    // BLK_BYTES boundary means "block full" (primesieve's Bucket trick) --
    // no count field to load on every push. Appends `entry` to ring slot
    // `slot`'s tail block, starting a new one if the current tail is full
    // or the slot is empty.
    void push_sparse_entry(uint32_t slot, erat::DenseState entry) {
        erat::DenseState* w = tail_[slot];
        if ((reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) w = new_block(slot);
        *w = entry;
        tail_[slot] = w + 1;
    }

    // Slow path of a push (tail_[slot] null or at a block boundary): links a
    // fresh block into ring slot `slot` and returns its first entry. Out of
    // line so process_big's loop keeps its constants in registers.
    __attribute__((noinline))
    erat::DenseState* new_block(uint32_t slot) {
        erat::DenseState* w = tail_[slot];
        Blk* nb = alloc_blk();
        nb->next = nullptr;
        if (w == nullptr) head_[slot] = nb;
        else reinterpret_cast<Blk*>(reinterpret_cast<char*>(w) - BLK_BYTES)->next = nb;
        return nb->entries();
    }

    // BLK_BYTES-aligned blocks pulled from a pool of aligned_alloc'd
    // arena_bytes_-sized arenas (indices/pointers into chunks_ stay valid
    // across pool growth since chunks_ holds owning pointers, never moved
    // or resized in place). free_ is a stack of blocks not currently in
    // any ring slot; begin_chunk() repopulates it from every arena ever
    // allocated, never shrinking the pool.
#ifndef ERA_BLK_BYTES
#define ERA_BLK_BYTES 4096
#endif
    static constexpr size_t BLK_BYTES = ERA_BLK_BYTES; // -DERA_BLK_BYTES for A/B
    // ERA_BLK_COLOR: the entries of a block start ERA_BLK_COLOR-1 lines at
    // most past its header, by the block's address. All slots fill at about
    // the same rate, so without it the ring's thousands of tail pointers sit
    // at the same offset inside their 4 KiB blocks -- the same cache-set
    // index bits 6-11 -- and only ways x (sets with those bits) of them can
    // be cached at once: 64 lines in a 256 KiB 8-way L2, 1024 in a 4 MiB
    // 16-way L3 (i7-620M: 83 ns per activated prime, DRAM). Costs
    // (ERA_BLK_COLOR-1)/2 lines of capacity per block on average.
#ifndef ERA_BLK_COLOR
#define ERA_BLK_COLOR 1
#endif
    struct Blk {
        Blk* next;
        uint64_t pad;
        erat::DenseState* entries() {
            if constexpr (ERA_BLK_COLOR > 1) {
                const size_t c = (reinterpret_cast<uintptr_t>(this) / BLK_BYTES) % ERA_BLK_COLOR;
                return reinterpret_cast<erat::DenseState*>(reinterpret_cast<char*>(this + 1) + 64 * c);
            }
            return reinterpret_cast<erat::DenseState*>(this + 1);
        }
        erat::DenseState* block_end() { return reinterpret_cast<erat::DenseState*>(reinterpret_cast<char*>(this) + BLK_BYTES); }
    };
    struct AlignedFree { void operator()(char* p) const { std::free(p); } };
    Blk* alloc_blk() {
        if (free_.empty()) {
            char* c = static_cast<char*>(std::aligned_alloc(huge_arenas_ ? arena_bytes_ : BLK_BYTES, arena_bytes_));
            if (c && huge_arenas_) madvise(c, arena_bytes_, MADV_HUGEPAGE); // best effort
            chunks_.emplace_back(c);
            for (size_t i = 0; i < arena_bytes_ / BLK_BYTES; ++i) free_.push_back(reinterpret_cast<Blk*>(c + i * BLK_BYTES));
        }
        Blk* b = free_.back();
        free_.pop_back();
        return b;
    }
    std::vector<Blk*> head_;
    std::vector<erat::DenseState*> tail_;
    std::vector<Blk*> free_;
    std::vector<std::unique_ptr<char, AlignedFree>> chunks_;

    std::vector<uint64_t> words_;
    uint64_t seg_k_width_;
    uint64_t sub_block_bytes_;
    bool medium_nta_; // prefetchnta the medium state (erat_small.hpp::cross_off_medium)
    bool big2310_;    // sparse tier on the mod-2310 wheel (process_big<true>)
    static constexpr ptrdiff_t MED64_NTA_DIST = 32; // entries ahead (4 cache lines)
    const Presieve& presieve_;

    // Dense tiers' per-prime state (erat_small.hpp), in lockstep with
    // small_primes/medium_primes as they activate -- no bucket, walked
    // every segment (small: every sub-block).
    std::vector<erat::DenseState> small_[8]; // one list per residue class p % 30
    // Medium tier, struct of arrays per residue class p % 30 (see
    // erat_small.hpp::cross_off_medium): (pos << 6) | w, and read-only qp as
    // 1-byte deltas from the class's first prime (medium_qp_base_).
    std::vector<uint32_t> medium_dyn_[8];
    std::vector<uint8_t> medium_qd_[8];
    std::vector<erat::MedBand> medium_bands_[8]; // ERA_MED_BANDS: fixed-iteration bands over each class's list
    uint32_t medium_qp_base_[8] = {};
    uint32_t medium_qp_last_[8] = {}; // activation only: qp of the class's last activated prime

    // med64 tier (kept default, see docs/RESEARCH.md): double-buffered, one
    // pair of (class, entry phase) lists swapped every segment instead of
    // migrating entries in place -- see process_med64's own comment.
    // med64_reserved_ survives begin_chunk() on purpose: a vector's
    // capacity survives clear(), so the one-time reserve (sieve_and_emit)
    // only needs to happen once per SegmentSieve instance, not per chunk.
    // 384 lists, one per (class, phase): PR*48+w.
    std::array<std::vector<erat::DenseState>, 384> m64_cur_{};
    std::array<std::vector<erat::DenseState>, 384> m64_nxt_{};
    bool med64_reserved_ = false;
    // The sub-blocked med64 band (p < m64s_limit_, see process_med64s): the
    // same (class, phase) lists, swapped once per SUB-BLOCK instead of once
    // per segment. m64s_count_: entries activated so far in the chunk, so
    // the sub-block loop skips the band's calls while it is empty.
    std::array<std::vector<erat::DenseState>, 384> m64s_cur_{};
    std::array<std::vector<erat::DenseState>, 384> m64s_nxt_{};
    // Bucket arena: 256 blocks (1 MiB), or one 2 MiB huge page (huge_arenas_,
    // decided in tuning.hpp). Declared here in the constructor's order.
    size_t arena_bytes_;
    bool huge_arenas_;
    uint64_t m64s_limit_ = 0;
    size_t m64s_count_ = 0;

    // ERA_ACT_BATCH staging (see stage_sparse_entry): one buffer of
    // ACT_GROUP_CAP entries x 12 B per group of 64 ring slots.
    std::vector<std::vector<ActEnt>> act_groups_;

    uint64_t skip_below_k_ = 1; // first wheel index that counts (set_skip_below_k)
    uint32_t log2_sb_ = 0; // log2(segment width in bytes) -- see constructor
    uint64_t num_buckets_ = 1;
    uint64_t cur_segment_ = 0; // this segment's ring slot, in [0, num_buckets_) -- see wrap_ring

    size_t next_small_idx_ = 0;
    size_t next_med64_idx_ = 0;
    size_t next_medium_idx_ = 0;
    uint64_t next_sparse_k_ = 0; // wheel index of the next sparse prime to activate
};
