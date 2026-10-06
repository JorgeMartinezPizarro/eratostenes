#pragma once
// One thread's sieve: a segment of the bit array (bit k = wheel index k,
// set = composite) and the state of every active base prime, in four tiers
// by hits per segment -- small, med64, medium, sparse; cutoffs from
// tuning.hpp. primesieve's EratSmall/EratMedium/EratBig split plus a med64
// band of this project's own. docs/ALGORITHM.md §6 explains each tier,
// docs/RESEARCH.md what else was tried.

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

// Sparse tier: process_big prefetches the segment byte of the entries this
// many positions ahead in the bucket (see its comment); 0 turns it off.
#ifndef ERA_BIG_PF
#define ERA_BIG_PF 16
#endif
// madvise(MADV_HUGEPAGE) for the sparse ring's arenas (SegmentSieve's
// huge_arenas, decided in tuning.hpp). Best effort: THP in "madvise" or
// "always" mode.
#include <sys/mman.h>
#ifdef __BMI2__
#include <immintrin.h>
#endif

// v mod 2^nbits, with mask = 2^nbits - 1: one BMI2 instruction (bzhi) that
// takes the bit count from a register, so process_big's loop needn't keep
// the mask live; plain `and` where there is no BMI2 (Ivy Bridge, the
// portable build).
static inline uint64_t low_bits(uint64_t v, uint32_t nbits, uint64_t mask) {
#ifdef __BMI2__
    (void)mask;
    return _bzhi_u64(v, nbits);
#else
    (void)nbits;
    return v & mask;
#endif
}
// Sparse activation: ERA_ACT_IDX takes p / 30 and p % 30 from the prime's
// wheel index instead of dividing. Fewer instructions and faster on modern
// cores, but slower on Ivy Bridge (its 64-bit division stalls longer once
// the independent work around it is gone), so it is on only where BMI2
// exists. See docs/RESEARCH.md#sparse-activation-from-the-bitmap-index-18-fewer-instructions-per-prime-kept-2026-10-05.
#ifndef ERA_ACT_IDX
#ifdef __BMI2__
#define ERA_ACT_IDX 1
#else
#define ERA_ACT_IDX 0
#endif
#endif

class SegmentSieve {
public:
    // seg_k_width: wheel-index width of a full segment (the last one of a
    // chunk may be shorter).
    // base_prime_max: the largest base prime (isqrt(limit)); sizes the
    // bucket ring so no sparse prime's step can wrap around it.
    // presieve: the shared pre-sieve pattern (presieve.hpp) each segment is
    // filled with instead of zeroed; its primes are excluded from the tiers.
    // sub_block_bytes: the L1-sized slice the small tier is crossed off in.
    // has_sparse: this run has sparse primes, so the segment must be a power
    // of 2 in bytes (the ring's slot math is a shift); tuning.hpp rounds it,
    // the constructor checks.
    SegmentSieve(uint64_t seg_k_width, uint64_t base_prime_max, const Presieve& presieve,
                 uint64_t sub_block_bytes, bool has_sparse, bool medium_nta,
                 bool huge_arenas = false)
        : words_((seg_k_width + 63) / 64, 0),
          seg_k_width_(seg_k_width),
          sub_block_bytes_(sub_block_bytes),
          medium_nta_(medium_nta),
          presieve_(presieve),
          arena_bytes_(huge_arenas ? (size_t{2} << 20) : BLK_BYTES * 256),
          huge_arenas_(huge_arenas) {
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
        sb_mask_ = (uint64_t{1} << log2_sb_) - 1;
        // The packed sparse entry (see file_sparse) holds pos in 24 bits and
        // qp in 28.
        if (log2_sb_ > 24 || base_prime_max / WHEEL_MOD >= (uint64_t{1} << 28)) {
            throw std::runtime_error("SegmentSieve: segment or base prime too large for the sparse tier");
        }
        // Largest BYTE step between one sparse prime's consecutive hits:
        // qp * max(dm) + max(corr), with max(dm) = 14 on the mod-2310
        // multiplier wheel (the largest gap between consecutive 2310-coprime
        // residues) -- a few extra WHEEL_SIZE's of slack (+16) cost nothing
        // (buckets are cheap) and keep this comfortably safe.
        uint64_t maxstep = base_prime_max / WHEEL_MOD * 14 + 16;
        uint64_t ahead = (maxstep >> log2_sb_) + 2;
        num_buckets_ = 1;
        while (num_buckets_ < ahead * 2) num_buckets_ <<= 1; // power of 2, 2x margin
        // Twice num_buckets_ slots, so a hit's slot is plainly cur_segment_ +
        // ahead (ahead < num_buckets_, cur_segment_ < num_buckets_) and the
        // hot loop has no wrap mask; wrap_ring() shifts the upper half down
        // once the cursor reaches num_buckets_. See process_big.
        head_.assign(2 * num_buckets_, nullptr);
        tail_.assign(2 * num_buckets_, nullptr);
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
        for (auto& v : medium_dyn_) v.clear();
        for (auto& v : medium_qd_) v.clear();
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

        // Each tier's primes are sorted and walked by a pointer that only
        // advances, so every prime is activated once per chunk, not once per
        // segment.
        //
        // The 384 med64 lists reserve their capacity once per SegmentSieve
        // (capacity survives clear() and the instance is reused across
        // chunks), with a margin: the spread over (class, phase) is close
        // to uniform, not exact.
        if (!med64_reserved_ && !med64_primes.empty()) {
            size_t per_list = med64_primes.size() / 384 * 2 + 16;
            for (auto& v : m64_cur_) v.reserve(per_list);
            for (auto& v : m64_nxt_) v.reserve(per_list);
            med64_reserved_ = true;
        }
        activate_dense(small_primes, next_small_idx_, small_, high_n, low_n, k_low);
        activate_med64(med64_primes, next_med64_idx_, m64_cur_.data(), high_n, low_n, k_low);
        activate_medium(medium_primes, next_medium_idx_, medium_dyn_, medium_qd_, medium_qp_base_, medium_qp_last_,
                        high_n, low_n, k_low);

        // Sparse tier: its primes are a run of the base-prime bitmap
        // (base_sieve.hpp's SparsePrimes), walked up to k_stop, the first
        // index whose prime's square reaches this segment's end (one isqrt
        // per segment instead of a p * p compare per prime).
        size_t sparse_activated = 0;
        if (next_sparse_k_ < sparse_primes.k_begin) next_sparse_k_ = sparse_primes.k_begin;
        const uint64_t k_cut = wheel_count_upto(isqrt(high_n - 1)); // primes with p*p < high_n have k < k_cut
        const uint64_t k_stop = std::min(k_cut, sparse_primes.k_end);
        while (next_sparse_k_ < k_stop) {
            const uint64_t wi = next_sparse_k_ >> 6;
            const uint64_t word_end = std::min((wi + 1) << 6, k_stop);
            uint64_t bits = sparse_primes.words[wi] & (~uint64_t{0} << (next_sparse_k_ & 63));
            if (word_end & 63) bits &= (uint64_t{1} << (word_end & 63)) - 1; // partial last word
            while (bits) {
                file_sparse((wi << 6) + static_cast<uint64_t>(__builtin_ctzll(bits)), low_n, k_low);
                ++sparse_activated;
                bits &= bits - 1;
            }
            next_sparse_k_ = word_end;
        }
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
        }
        // Wheel indices below skip_below_k_ are not part of the range: index
        // 0 (the number 1, nothing marks it) and, with --start, the head of
        // the first word that split_ranges rounded the start down into.
        if (k_low < skip_below_k_) {
            for (uint64_t k = k_low; k < std::min(skip_below_k_, k_high); ++k)
                words_[(k - k_low) >> 6] |= uint64_t{1} << ((k - k_low) & 63);
        }

        // med64 tier: one pass over the whole segment (see process_med64).
        // Its entries are re-filed into m64_nxt_ by their exit phase; the
        // two buffers swap here, so the next segment reads what this one
        // wrote.
        if (!med64_primes.empty()) {
            run_med64(bytes, bytes_needed);
            for (auto& v : m64_cur_) v.clear();
            std::swap(m64_cur_, m64_nxt_);
        }

        // Medium tier: one pass over the whole segment, one list per residue
        // class; the prefetchnta variant is picked per tier set (tuning.hpp's
        // medium_nta_min_primes).
        if (medium_nta_) run_medium<true>(bytes, bytes_needed);
        else run_medium<false>(bytes, bytes_needed);

        // Sparse tier: only when the run has sparse primes at all (decided
        // once per run), so dense-only runs never pay the call.
        if (!sparse_primes.empty()) process_big();
        if (++cur_segment_ == num_buckets_) wrap_ring();

        // Extraction: bit=0 => prime candidate. Three paths, by what the
        // sink can take (see below).
        if constexpr (!Writer::WANTS_VALUES) prime_count += count_primes(count);
        else if constexpr (requires { out.write_k(uint64_t{0}); }) prime_count += emit_indices(k_low, count, out);
        else prime_count += emit_values(k_low, count, out);
    }

private:
    // Count-only (NullSink): a plain popcount over the full words with four
    // accumulators, the partial last word masked once after the loop (a
    // per-word "last word?" test made GCC build a cmove chain, ~3x the
    // instructions).
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
                const uint64_t rq = r + step2;
                q += rq >> WHEEL_SIZE_LOG2;
                r = rq & (static_cast<uint64_t>(WHEEL_SIZE) - 1);

                uint64_t value = q * WHEEL_MOD + WHEEL_R[r];
                out.write_uint64(value);
                ++n;
                bits &= bits - 1; // clear the lowest set bit
            }
        }
        return n;
    }

    // Small tier: appends the state of every prime from `next` on whose
    // square falls below this segment's end, one list per residue class
    // (state[pr]): the pending hit's byte position and the packing
    // (qp << 6) | (pr << 3) | j that erat_small.hpp::cross_off_class reads.
    __attribute__((noinline)) static void activate_dense(const std::vector<uint64_t>& primes, size_t& next,
                               std::vector<erat::DenseState>* state,
                               uint64_t high_n, uint64_t low_n, uint64_t k_low) {
        while (next < primes.size()) {
            uint64_t p = primes[next];
            if (p * p >= high_n) break;
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            // Smallest m coprime with 30 with p*m >= start_val.
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

    // med64 tier: the smallest multiplier coprime with 210 (multiples of 7
    // are presieved), filed under state[pr * 48 + w] with qw = (qp << 6) | w,
    // so every cross_off_checked210<PR> call of one inner loop enters at the
    // same phase w.
    __attribute__((noinline)) static void activate_med64(const std::vector<uint64_t>& primes, size_t& next,
                                   std::vector<erat::DenseState>* state384,
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
            state384[pr * 48 + w].push_back({static_cast<uint32_t>(((p / WHEEL_MOD) << 6) | w),
                                             static_cast<uint32_t>(pos)});
            ++next;
        }
    }

    // Medium tier: the same mod-210 start as med64, one list per residue
    // class as struct of arrays -- dyn = (pos << 6) | w, rewritten every
    // segment, and qp as a 1-byte delta from the class's previous prime
    // (read-only; the first prime of a class sets qp_base). See
    // erat_small.hpp::cross_off_medium.
    __attribute__((noinline)) static void activate_medium(const std::vector<uint64_t>& primes, size_t& next,
                                 std::vector<uint32_t>* dyn, std::vector<uint8_t>* qds,
                                 uint32_t* qp_base, uint32_t* qp_last,
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

    // med64 tier, class PR: the class's 48 lists in phase order, so every
    // cross_off_checked210<PR> call of one inner loop enters its switch at
    // the same phase (a predicted jump). Over the whole segment, not
    // sub-blocked: these primes have too few hits per sub-block to pay a call
    // each. Each entry is re-filed into m64_nxt_ by its exit phase, rebased
    // to the next segment. The state stream is read with prefetchnta, once
    // per entry, to keep it out of L2. See
    // docs/RESEARCH.md#med64-mod-210-stepping-on-the-checked-loop-cross_off_checked210-kept-2026-09-30
    // and docs/RESEARCH.md#med64-prefetchnta-on-the-state-stream-kept-2026-09-29.
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

    // Medium tier over the whole segment, one call per residue class PR (a
    // template parameter of the kernel); the rebase is the segment's width.
    template <bool NTA, int PR>
    void run_medium_class(uint8_t* bytes, uint64_t bytes_needed) {
        erat::cross_off_medium<PR, NTA>(bytes, bytes_needed, medium_dyn_[PR].data(),
                                        medium_dyn_[PR].data() + medium_dyn_[PR].size(), medium_qd_[PR].data(),
                                        medium_qp_base_[PR], bytes_needed);
    }
    template <bool NTA, int... PR>
    void run_medium_all(uint8_t* bytes, uint64_t bytes_needed, std::integer_sequence<int, PR...>) {
        (run_medium_class<NTA, PR>(bytes, bytes_needed), ...);
    }
    template <bool NTA>
    void run_medium(uint8_t* bytes, uint64_t bytes_needed) {
        run_medium_all<NTA>(bytes, bytes_needed, std::make_integer_sequence<int, 8>{});
    }

    // Sparse tier (primesieve's EratBig design): every sparse prime sits in
    // the ring slot of the segment its next hit falls in, as one 8-byte
    // entry in a chain of 4 KiB pooled blocks. This drains this segment's
    // slot: for each entry, mark its hit, step to the next one with the
    // mod-2310 table (big::TABLE2310; 11 is presieved too, ~9% fewer hits
    // than mod 210) and copy the entry into the tail block of the slot that
    // hit falls in. The entry is idx (class and phase) | pos << 12 | qp << 36,
    // pos relative to the segment it is due in. noinline: its own register
    // allocation, away from the dense tiers'. See
    // docs/RESEARCH.md#sparse-tier-design-current-fixed-size-pooled-blocks-attempt-6
    // and docs/RESEARCH.md#sparse-tier-mod-2310-multiplier-wheel-kept-2026-09-30.
    //
    // Ring slots: this segment's is cur_segment_ (kept below num_buckets_,
    // see wrap_ring), a hit `ahead` segments on files into cur_segment_ +
    // ahead, always below the 2 x num_buckets_ slots allocated (the ring's
    // sizing has ahead < num_buckets_ / 2 for a re-filed hit, and
    // file_sparse checks ahead < num_buckets_ for an activation), so the
    // loop needs no wrap mask.
    __attribute__((noinline))
    void process_big() {
        const uint32_t slot = static_cast<uint32_t>(cur_segment_);
        uint8_t* const s = reinterpret_cast<uint8_t*>(words_.data());
        // This segment's tail pointer, as a pointer: a hit `ahead` segments
        // on files into tails_cur[ahead], so neither the array base (which
        // the s[pos] store would force GCC to reload) nor the cursor is live
        // in the loop. The slow path recovers the slot index from
        // tail_.data().
        erat::DenseState** const tails_cur = tail_.data() + cur_segment_;
        // uint32_t on purpose: as uint64_t GCC kept two copies and spilled
        // tails_cur (docs/RESEARCH.md).
        const uint32_t log2sb = log2_sb_;
        const uint64_t modsb = (uint64_t{1} << log2sb) - 1;
        while (head_[slot]) {
            Blk* blk = head_[slot];
            erat::DenseState* last_end = tail_[slot];
            head_[slot] = nullptr;
            tail_[slot] = nullptr;
            while (blk) {
                Blk* next_blk = blk->next;
                // The blocks of a chain are scattered in memory (LIFO free
                // list), so the next one is prefetched into L2 a line at a
                // time over this block's first half. The last block of a
                // chain prefetches itself (already cached), so the loops
                // below test nothing for it. See
                // docs/RESEARCH.md#sparse-tier-next-block-prefetch-spread-over-the-current-block-kept-2026-10-04.
                const char* nb = reinterpret_cast<const char*>(next_blk ? next_blk : blk);
                erat::DenseState* it = blk->entries();
                erat::DenseState* end = next_blk ? blk->block_end() : last_end;
                // Two entries per step: both entries' loads, table rows and
                // segment RMWs are issued before either push, so their misses
                // overlap (EratBig's loop shape). The pushes stay in order: a
                // shared slot's second push reads the tail the first one just
                // wrote. Each entry and table row is one 8-byte load, the new
                // entry one 8-byte store, the new-block path out of line.
                constexpr int U = 2;
                // The segment byte of the entries ERA_BIG_PF ahead of p, cnt
                // of them (inside the block; the caller keeps it so): that RMW
                // is the load that misses once two threads share an L2. See
                // docs/RESEARCH.md#sparse-tier-two-entries-per-iteration-in-process_big-and-a-segment-byte-prefetch-16-entries-ahead-both-kept-2026-10-02.
                auto seg_pf = [&](const erat::DenseState* p, size_t cnt) __attribute__((always_inline)) {
                    if constexpr (ERA_BIG_PF > 0) {
                        for (size_t k = 0; k < cnt; ++k) {
                            uint64_t pe;
                            std::memcpy(&pe, p + ERA_BIG_PF + k, sizeof(uint64_t));
                            __builtin_prefetch(s + ((pe >> 12) & 0xffffff), 1, 3);
                        }
                    } else {
                        (void)p; (void)cnt;
                    }
                };
                // The U entries [p, p + U): loads, table rows and segment
                // RMWs first, then the pushes in order.
                auto body = [&](const erat::DenseState* p) __attribute__((always_inline)) {
                    uint64_t ent[U], pos[U], te[U], e[U], sl[U];
                    for (int k = 0; k < U; ++k) std::memcpy(&ent[k], p + k, sizeof(uint64_t));
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
                    for (int k = 0; k < U; ++k) {
                        sl[k] = pos[k] >> log2sb; // segments ahead: the slot is tails_cur[sl]
                        e[k] = (ent[k] & ~((uint64_t{1} << 36) - 1)) | nidx[k] | (low_bits(pos[k], log2sb, modsb) << 12);
                    }
                    for (int k = 0; k < U; ++k) {
                        erat::DenseState** const tp = tails_cur + sl[k];
                        erat::DenseState* w = *tp;
                        if ((reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) [[unlikely]]
                            w = new_block(static_cast<uint32_t>(tp - tail_.data()));
                        std::memcpy(w, &e[k], sizeof(uint64_t));
                        *tp = w + 1;
                    }
                };
                // Groups of G entries, counted once per block: the first
                // `spread` groups also prefetch line g of the next block (64
                // lines over the block's first 256 entries), the rest only the
                // segment bytes ERA_BIG_PF ahead; the entries whose
                // ERA_BIG_PF-ahead neighbours are past `end` go through the
                // plain pairs. No test inside either loop but its own end. See
                // docs/RESEARCH.md#sparse-tier-process_big-in-groups-of-4-entries-no-per-iteration-edge-tests-kept-2026-10-06.
                constexpr size_t G = 4;
                constexpr size_t PF = ERA_BIG_PF > 0 ? ERA_BIG_PF : 0;
                const size_t n = static_cast<size_t>(end - it);
                const size_t groups = n >= PF + G ? (n - PF) / G : 0;
                const size_t spread = std::min<size_t>(groups, BLK_BYTES / 64);
                const char* line = nb;
                for (erat::DenseState* const ge = it + spread * G; it != ge; it += G) {
                    __builtin_prefetch(line, 0, 2);
                    line += 64;
                    seg_pf(it, G);
                    for (size_t k = 0; k < G; k += U) body(it + k);
                }
                for (erat::DenseState* const ge = it + (groups - spread) * G; it != ge; it += G) {
                    seg_pf(it, G);
                    for (size_t k = 0; k < G; k += U) body(it + k);
                }
                for (; it + U <= end; it += U) body(it);
                // An odd last entry, one at a time.
                for (; it != end; ++it) {
                    uint64_t ent;
                    std::memcpy(&ent, it, sizeof(ent)); // idx | pos << 12 | qp << 36
                    uint64_t pos = (ent >> 12) & 0xffffff;
                    const uint64_t te = big::TABLE2310[ent & 4095]; // mask | dm << 8 | corr << 16 | next << 20
                    s[pos] |= static_cast<uint8_t>(te);
                    pos += (ent >> 36) * ((te >> 8) & 0xff) + ((te >> 16) & 15);
                    const uint64_t e = (ent & ~((uint64_t{1} << 36) - 1)) | (te >> 20) | (low_bits(pos, log2sb, modsb) << 12);
                    erat::DenseState** const tp = tails_cur + (pos >> log2sb);
                    erat::DenseState* w = *tp;
                    // Null (empty slot) or on a block boundary (block full).
                    if ((reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) [[unlikely]]
                        w = new_block(static_cast<uint32_t>(tp - tail_.data()));
                    std::memcpy(w, &e, sizeof(e));
                    *tp = w + 1;
                }
                free_.push_back(blk);
                blk = next_blk;
            }
        }
    }

    // Files the sparse prime of wheel index k into the ring: the smallest
    // multiplier m coprime to 2310 with p*m >= max(p*p, low_n), packed into
    // one entry, into the slot of the segment that hit falls in. qp and the
    // residue class come from the index itself (ERA_ACT_IDX; k >> 3 and
    // k & 7), not from dividing p.
    void file_sparse(uint64_t k, uint64_t low_n, uint64_t k_low) {
        uint64_t qp, ri, p;
        if constexpr (ERA_ACT_IDX) {
            qp = k >> WHEEL_SIZE_LOG2;             // p / WHEEL_MOD
            ri = k & (WHEEL_SIZE - 1);             // WHEEL_POS[p % WHEEL_MOD]
            p = qp * WHEEL_MOD + WHEEL_R[ri];
        } else {
            // From the prime: a constant division and a constant modulo per prime.
            p = wheel_number(k);
            qp = p / WHEEL_MOD;
            ri = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
        }
        uint64_t start_val = std::max(p * p, low_n);
        uint64_t m = (start_val + p - 1) / p;
        // Packed as one word: idx (ri * 480 + w) in bits 0-11, pos in 12-35,
        // qp in 36-63 -- see process_big.
        uint64_t t = m / 2310, sres = m % 2310;
        uint64_t w = big::NEXT_W2310[sres];
        if (w == big::W2310) { ++t; w = 0; }
        m = t * 2310 + big::M2310[w];
        uint64_t pos = p * m / WHEEL_MOD - k_low / 8;
        uint64_t ahead = pos >> log2_sb_;
        if (ahead >= num_buckets_) {
            throw std::runtime_error(
                "bucket sieve: a sparse prime's step exceeds the bucket ring's margin "
                "(sizing bug in SegmentSieve's constructor)");
        }
        uint64_t ent = (ri * big::W2310 + w) | ((pos & sb_mask_) << 12) | (qp << 36);
        erat::DenseState e;
        std::memcpy(&e, &ent, sizeof(e));
        push_sparse_entry(static_cast<uint32_t>(cur_segment_ + ahead), e);
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
    struct Blk {
        Blk* next;
        uint64_t pad;
        erat::DenseState* entries() { return reinterpret_cast<erat::DenseState*>(this + 1); }
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
    uint32_t medium_qp_base_[8] = {};
    uint32_t medium_qp_last_[8] = {}; // activation only: qp of the class's last activated prime

    // med64 tier: 384 lists, one per (class, entry phase), PR * 48 + w,
    // double-buffered (see process_med64). med64_reserved_ survives
    // begin_chunk() on purpose: capacity survives clear().
    std::array<std::vector<erat::DenseState>, 384> m64_cur_{};
    std::array<std::vector<erat::DenseState>, 384> m64_nxt_{};
    bool med64_reserved_ = false;
    // Bucket arena: 256 blocks (1 MiB), or one 2 MiB huge page (huge_arenas_,
    // decided in tuning.hpp). Declared here in the constructor's order.
    size_t arena_bytes_;
    bool huge_arenas_;

    uint64_t skip_below_k_ = 1; // first wheel index that counts (set_skip_below_k)
    uint32_t log2_sb_ = 0; // log2(segment width in bytes) -- see constructor
    uint64_t sb_mask_ = 0; // (1 << log2_sb_) - 1: a hit's byte offset inside its segment
    uint64_t num_buckets_ = 1;
    uint64_t cur_segment_ = 0; // this segment's ring slot, in [0, num_buckets_) -- see wrap_ring

    size_t next_small_idx_ = 0;
    size_t next_med64_idx_ = 0;
    size_t next_medium_idx_ = 0;
    uint64_t next_sparse_k_ = 0; // wheel index of the next sparse prime to activate
};
