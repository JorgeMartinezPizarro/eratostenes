#pragma once
// One thread's sieve: a segment of the bit array (bit k = wheel index k,
// set = composite) and the state of every active base prime, in four tiers
// by hits per segment -- small, med64, medium (here), sparse
// (sparse_tier.hpp); cutoffs from tuning.hpp. primesieve's
// EratSmall/EratMedium/EratBig split plus a med64 band of this project's
// own. docs/ALGORITHM.md §6 explains each tier, docs/RESEARCH.md what else
// was tried.

#include <array>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <utility>

#include "base_sieve.hpp"
#include "erat_small.hpp"
#include "presieve.hpp"
#include "sparse_tier.hpp"
#include "wheel.hpp"
#include "wheel210_big.hpp"

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
    // of 2 in bytes (SparseTier checks).
    SegmentSieve(uint64_t seg_k_width, uint64_t base_prime_max, const Presieve& presieve,
                 uint64_t sub_block_bytes, bool has_sparse, bool medium_nta,
                 bool huge_arenas = false)
        : words_((seg_k_width + 63) / 64, 0),
          seg_k_width_(seg_k_width),
          sub_block_bytes_(sub_block_bytes),
          medium_nta_(medium_nta),
          presieve_(presieve),
          sparse_(seg_k_width / 8, base_prime_max, has_sparse, huge_arenas) {
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
    }

    // Wheel indices below k are marked composite before extraction (the
    // number 1, and the numbers below a --start that split_ranges rounded
    // down to). Default 1.
    void set_skip_below_k(uint64_t k) { skip_below_k_ = std::max<uint64_t>(k, 1); }

    // The last number of the whole run (N): sparse primes with no multiple
    // up to it aren't filed (SparseTier::set_range_end).
    void set_range_end(uint64_t n) { sparse_.set_range_end(n); }

    // Must be called once before the first sieve_and_emit call for a new,
    // independent run of consecutive segments in increasing k order (a
    // thread's chunk). Resets every tier's state and the "which primes have
    // activated yet" pointers. Never reuse a SegmentSieve across threads or
    // out of order.
    void begin_chunk() {
        next_small_idx_ = 0;
        next_med64_idx_ = 0;
        next_medium_idx_ = 0;
        for (auto& v : small_) v.clear();
        for (auto& v : m64_cur_) v.clear();
        for (auto& v : m64_nxt_) v.clear();
        for (auto& v : medium_dyn_) v.clear();
        for (auto& v : medium_qd_) v.clear();
        sparse_.begin_chunk();
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
        const size_t sparse_activated = sparse_.activate(sparse_primes, high_n, low_n, k_low);
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
        if (!sparse_primes.empty()) sparse_.process_big(bytes);
        sparse_.next_segment();

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

    uint64_t skip_below_k_ = 1; // first wheel index that counts (set_skip_below_k)

    size_t next_small_idx_ = 0;
    size_t next_med64_idx_ = 0;
    size_t next_medium_idx_ = 0;

    // Sparse tier: the bucket ring and its activation cursor. Last, in the
    // constructor's order.
    SparseTier sparse_;
};
