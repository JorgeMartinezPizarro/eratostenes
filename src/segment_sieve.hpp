#pragma once
// Segmented sieve on a compile-time wheel (see wheel.hpp), bit-packed into
// uint64_t words. Four prime tiers by expected hits per segment -- small,
// med64, medium, sparse (cutoffs computed in main.cpp) -- mirroring
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

#include "erat_small.hpp"
#include "presieve.hpp"
#include "wheel.hpp"
#include "wheel210_big.hpp"

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
    // shift/mask instead of a division -- main.cpp is responsible for
    // flooring seg_k_width to the nearest power of 2 (in bytes) whenever
    // has_sparse is true; this constructor just verifies that was done.
    SegmentSieve(uint64_t seg_k_width, uint64_t base_prime_max, const Presieve& presieve,
                 uint64_t sub_block_bytes, bool has_sparse, bool medium_nta, bool med64_nta,
                 bool med64_210)
        : words_((seg_k_width + 63) / 64, 0),
          seg_k_width_(seg_k_width),
          sub_block_bytes_(sub_block_bytes),
          medium_nta_(medium_nta),
          med64_nta_(med64_nta),
          med64_210_(med64_210),
          presieve_(presieve) {
        // The byte-addressed dense tiers (erat_small.hpp) need every
        // segment to start on a byte (k multiple of 8) and to stay a whole
        // number of words; callers align chunk starts to 64 too.
        if (seg_k_width % 64 != 0 || sub_block_bytes % 8 != 0 || sub_block_bytes == 0) {
            throw std::runtime_error("SegmentSieve: ancho de segmento/sub-bloque no alineado");
        }
        // Dense primes are p < seg_k_width, so their pending hit stays
        // within a few segment widths of the segment start (fits
        // DenseState::pos), and p / 30 has to fit its packed qp field.
        if (seg_k_width / WHEEL_MOD >= erat::QP_LIMIT || seg_k_width >= (uint64_t{1} << 30)) {
            throw std::runtime_error("SegmentSieve: segmento demasiado grande para el estado denso empaquetado");
        }
        // Medium tier packs a pending hit's byte position into 26 bits
        // (erat_small.hpp::cross_off_medium): at most one segment plus one
        // step of a medium prime (p < seg_k_width, step <= qp * 10 + 16).
        if (seg_k_width / 8 + seg_k_width / WHEEL_MOD * 10 + 16 >= erat::MEDIUM_POS_LIMIT) {
            throw std::runtime_error("SegmentSieve: segmento demasiado grande para el estado del tier medio");
        }
        uint64_t sb = seg_k_width_ / 8; // segment width in bytes
        if (has_sparse && (sb & (sb - 1))) {
            throw std::runtime_error("SegmentSieve: el tier disperso (EratBig) necesita un segmento potencia de 2 (bytes)");
        }
        log2_sb_ = 0;
        while ((uint64_t{1} << log2_sb_) < sb) ++log2_sb_;
        // Largest BYTE step between one sparse prime's consecutive hits:
        // qp * max(dm) + max(corr), with max(dm) = 10 on the mod-210
        // multiplier wheel (the largest gap between consecutive 210-
        // coprime residues) -- a few extra WHEEL_SIZE's of slack (+16)
        // cost nothing (buckets are cheap) and keep this comfortably safe.
        uint64_t maxstep = base_prime_max / WHEEL_MOD * 10 + 16;
        uint64_t ahead = (maxstep >> log2_sb_) + 2;
        num_buckets_ = 1;
        while (num_buckets_ < ahead * 2) num_buckets_ <<= 1; // power of 2, 2x margin
        head_.assign(num_buckets_, nullptr);
        tail_.assign(num_buckets_, nullptr);
    }

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
        next_sparse_idx_ = 0;
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
            for (size_t i = 0; i < CHUNK_BLOCKS; ++i)
                free_.push_back(reinterpret_cast<Blk*>(c.get() + i * BLK_BYTES));
    }

    template <typename Writer>
    void sieve_and_emit(uint64_t k_low, uint64_t k_high,
                         const std::vector<uint64_t>& small_primes,
                         const std::vector<uint64_t>& med64_primes,
                         const std::vector<uint64_t>& medium_primes,
                         const std::vector<uint64_t>& sparse_primes,
                         Writer& out, uint64_t& prime_count) {
        uint64_t count = (k_high > k_low) ? (k_high - k_low) : 0;
        if (count == 0) return;

        size_t words_needed = (count + 63) / 64;
        uint64_t bytes_needed = (count + 7) / 8;

        uint64_t high_n = wheel_number(k_high); // exclusive numeric bound, valid for the p*p cutoff
        uint64_t low_n = wheel_number(k_low);

        // Activate any base primes that just became relevant (p*p < high_n).
        // Each of the four lists is sorted by p and gets its own
        // monotonically-advancing pointer, so every prime is visited here
        // exactly once for the whole chunk, not once per segment.
        //
        // med64_'s own lists reserve capacity once, on this SegmentSieve's
        // very first activation (not per chunk -- a vector's capacity
        // survives clear(), so this only needs to happen once ever), to
        // avoid growing 64 small vectors one push_back at a time while
        // hot. Distribution across (class, phase) is expected to be close
        // to uniform, not exact, hence the margin.
        if (!med64_reserved_ && !med64_primes.empty()) {
            size_t per_list = med64_primes.size() / (med64_210_ ? 384 : 64) * 2 + 16;
            for (auto& v : m64_cur_) v.reserve(per_list);
            for (auto& v : m64_nxt_) v.reserve(per_list);
            med64_reserved_ = true;
        }
        activate_dense(small_primes, next_small_idx_, small_, true, high_n, low_n, k_low);
        if (med64_210_) activate_med64_210(med64_primes, next_med64_idx_, m64_cur_.data(), high_n, low_n, k_low);
        else activate_med64(med64_primes, next_med64_idx_, m64_cur_.data(), high_n, low_n, k_low);
        activate_medium(medium_primes, next_medium_idx_, medium_dyn_, medium_qd_, medium_qp_base_, medium_qp_last_,
                        high_n, low_n, k_low);

        // EratBig-style activation (see header comment): find the smallest
        // multiplier m coprime to 210 (not just 30) with p*m >= max(p*p,
        // low_n), then pack qp/residue-class/mod-210 phase into one word
        // exactly like the dense tiers' DenseState, and file it directly
        // into the bucket ring by byte position (shift/mask, no division).
        while (next_sparse_idx_ < sparse_primes.size()) {
            uint64_t p = sparse_primes[next_sparse_idx_];
            if (p * p >= high_n) break;
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
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
                    "bucket sieve: salto de un primo disperso mayor que el margen del anillo de "
                    "cubos (bug de dimensionamiento en el constructor de SegmentSieve)");
            }
            push_sparse_entry(static_cast<uint32_t>((cur_segment_ + ahead) & (num_buckets_ - 1)), e);
            ++next_sparse_idx_;
        }

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
        // Wheel index 0 is the number 1: not prime, and nothing marks it.
        if (k_low == 0) words_[0] |= 1;

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
            if (med64_210_) {
                if (med64_nta_) run_med64<true, true>(bytes, bytes_needed);
                else run_med64<false, true>(bytes, bytes_needed);
            } else {
                if (med64_nta_) run_med64<true, false>(bytes, bytes_needed);
                else run_med64<false, false>(bytes, bytes_needed);
            }
            for (auto& v : m64_cur_) v.clear();
            std::swap(m64_cur_, m64_nxt_);
        }

        // Medium tier: one pass over the whole segment each, one list per
        // residue class so PR is a compile-time template parameter in
        // cross_off_medium<PR>, same reasoning as the small tier's
        // cross_off_class<PR> calls just above; NTA picked per TierSet (see
        // main.cpp's MEDIUM_NTA_MIN_PRIMES).
        if (medium_nta_) run_medium<true>(bytes, bytes_needed);
        else run_medium<false>(bytes, bytes_needed);

        // Sparse tier: see process_sparse_bucket below (pulled out of this
        // function on purpose -- see its own comment). sparse_primes is
        // either empty for the whole run or not -- never changes segment
        // to segment -- so skipping the call entirely when it's empty
        // avoids paying a real (non-inlined) call's overhead every single
        // segment for N where this tier never has anything to do (every N
        // tested up to 1e12 on this machine, see README#benchmarks).
        if (!sparse_primes.empty()) process_big();
        ++cur_segment_;

        // Extraction: bit=0 => prime candidate. Accumulated locally and
        // added to prime_count once at the end, instead of read-modify-
        // writing through the reference every word (count-only) or every
        // single prime (value extraction) -- prime_count is a reference
        // into the caller's frame, so the compiler can't always prove
        // nothing else aliases it and keep it in a register across this
        // loop; a local can't be aliased by anything, so it stays in a
        // register for the whole function.
        uint64_t local_prime_count = 0;
        for (size_t w = 0; w < words_needed; ++w) {
            uint64_t bits = ~words_[w];
            uint64_t base_idx = w * 64ULL;
            uint64_t remaining = count - base_idx;
            if (remaining < 64) {
                bits &= (remaining == 0) ? 0ULL : ((1ULL << remaining) - 1ULL);
            }
            if (bits == 0) continue;

            // count-only (NullSink) never looks at the value written --
            // skip straight to how many bits are set instead of decoding
            // each one (ctz + wheel-index math) just to discard it below.
            if constexpr (!Writer::WANTS_VALUES) {
                local_prime_count += static_cast<uint64_t>(__builtin_popcountll(bits));
                continue;
            }

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
                ++local_prime_count;
                bits &= bits - 1; // clear the lowest set bit
            }
        }
        prime_count += local_prime_count;
    }

private:
    // Activates (appends state for) every prime in `primes` from `next`
    // on whose square falls below this segment's end; `primes` is sorted,
    // so this touches each prime exactly once per chunk. by_class: the
    // small tier -- byte positions, and one output list per residue class
    // (state[pr]).
    __attribute__((noinline)) static void activate_dense(const std::vector<uint64_t>& primes, size_t& next,
                               std::vector<erat::DenseState>* state, bool by_class,
                               uint64_t high_n, uint64_t low_n, uint64_t k_low) {
        while (next < primes.size()) {
            uint64_t p = primes[next];
            if (p * p >= high_n) break;
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            (void)by_class; // small tier only now; kept for call-site symmetry with activate_medium

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

    // med64 tier: exactly activate_dense's small-tier derivation (mod-30,
    // byte position, (qp<<6)|(pr<<3)|j packing) -- the only difference is
    // the push target: a 64-way (class, entry phase) split (state64[pr*8+j])
    // instead of activate_dense's 8-way (class only) split, since
    // process_med64<PR> (below) groups entries so every call into
    // cross_off<PR> for a given list shares one entry phase.
    __attribute__((noinline)) static void activate_med64(const std::vector<uint64_t>& primes, size_t& next,
                                std::vector<erat::DenseState>* state64,
                                uint64_t high_n, uint64_t low_n, uint64_t k_low) {
        while (next < primes.size()) {
            uint64_t p = primes[next];
            if (p * p >= high_n) break;
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;
            uint64_t pos = (p * m) / WHEEL_MOD - k_low / 8;
            uint64_t j = static_cast<uint64_t>(WHEEL_POS[r]);
            state64[pr * 8 + j].push_back({static_cast<uint32_t>(((p / WHEEL_MOD) << 6) | (pr << 3) | j),
                                           static_cast<uint32_t>(pos)});
            ++next;
        }
    }

    // med64 on the mod-210 multiplier wheel (MED64_210): activate_medium's
    // start derivation (smallest m coprime with 210, byte position), filed
    // under state[pr*48+w] with qw = (qp << 6) | w, so every
    // cross_off_checked210<PR> call in one inner loop shares entry phase w.
    __attribute__((noinline)) static void activate_med64_210(const std::vector<uint64_t>& primes, size_t& next,
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
                    throw std::runtime_error("SegmentSieve: salto entre primos medianos de la misma clase > 255*30");
                }
                qds[pr].push_back(static_cast<uint8_t>(d));
            }
            qp_last[pr] = qp;
            ++next;
        }
    }

    // med64 tier (kept default, see docs/RESEARCH.md): PR a compile-time
    // template parameter like cross_off_class<PR>/cross_off_medium<PR>
    // above, looping over that class's own 8 entry-phase lists
    // (m64_cur_[PR*8+j]) so every cross_off<PR> call within one inner loop
    // shares the same entry phase j -- the entry-side switch in cross_off
    // becomes a well-predicted branch (same outcome every call in that
    // loop) instead of an unpredictable per-prime dispatch. The exit side
    // (cross_off's own straight-line ERAT_HIT chain) still depends on each
    // individual prime's own phase alignment against the segment boundary
    // and isn't fixed by this grouping -- see docs/RESEARCH.md if
    // branch-misses:u turns out to still dominate.
    // Runs over the WHOLE segment (bytes_needed), not sub-blocked like the
    // small tier: this tier's population is bounded/saturated by
    // small_limit's own tuning (see main.cpp), not chasing L1 residency
    // for a large one. Each entry is read from m64_cur_, stepped by
    // cross_off<PR>, and re-filed into m64_nxt_ keyed by its NEW exit
    // phase and rebased by bytes_needed (this segment's own width, since
    // there's no sub-block rebase to chain off) -- sieve_and_emit clears
    // m64_cur_ and swaps the two buffers once every PR has run, so next
    // segment reads what this one just wrote.
    //
    // NTA: prefetchnta m64_cur_ MED64_NTA_DIST entries ahead, once per
    // entry, like cross_off_medium's own NTA path: the double-buffered state
    // (~2 x 233 KB per thread from 5e12 up) is streamed through once per
    // segment. Dev PC: 1e13 322.55s vs the README's best-of-7 326.03s
    // (-1.1%), 1e14 5% tail -2.6% over 6+6 runs; 1e10-1e12 unchanged. See
    // docs/RESEARCH.md. ERATOSTENES_MED64_NTA=0 turns it off (main.cpp).
    template <int PR, bool NTA, bool M210>
    void process_med64(uint8_t* bytes, uint64_t bytes_needed) {
        if constexpr (M210) {
            for (int w = 0; w < 48; ++w) {
                for (erat::DenseState& st : m64_cur_[PR * 48 + w]) {
                    if constexpr (NTA) __builtin_prefetch(&st + MED64_NTA_DIST, 0, 0);
                    uint64_t i = st.pos;
                    uint64_t qp = st.qw >> 6;
                    uint32_t ww = st.qw & 63;
                    erat::cross_off_checked210<PR>(bytes, bytes_needed, qp, i, ww);
                    m64_nxt_[PR * 48 + ww].push_back(
                        {static_cast<uint32_t>((qp << 6) | ww), static_cast<uint32_t>(i - bytes_needed)});
                }
            }
            return;
        }
        for (int j = 0; j < 8; ++j) {
            for (erat::DenseState& st : m64_cur_[PR * 8 + j]) {
                if constexpr (NTA) __builtin_prefetch(&st + MED64_NTA_DIST, 0, 0);
                uint64_t i = st.pos;
                uint64_t qp = st.qw >> 6;
                uint32_t jj = st.qw & 7;
                erat::cross_off_checked<PR>(bytes, bytes_needed, qp, i, jj);
                m64_nxt_[PR * 8 + jj].push_back(
                    {static_cast<uint32_t>((qp << 6) | (PR << 3) | jj), static_cast<uint32_t>(i - bytes_needed)});
            }
        }
    }

    // Out of line (pinned): with a second (mod-210) instantiation GCC starts
    // inlining it into sieve_chunk on its own.
    template <bool NTA, bool M210>
    __attribute__((noinline)) void run_med64(uint8_t* bytes, uint64_t bytes_needed) {
        process_med64<0, NTA, M210>(bytes, bytes_needed);
        process_med64<1, NTA, M210>(bytes, bytes_needed);
        process_med64<2, NTA, M210>(bytes, bytes_needed);
        process_med64<3, NTA, M210>(bytes, bytes_needed);
        process_med64<4, NTA, M210>(bytes, bytes_needed);
        process_med64<5, NTA, M210>(bytes, bytes_needed);
        process_med64<6, NTA, M210>(bytes, bytes_needed);
        process_med64<7, NTA, M210>(bytes, bytes_needed);
    }

    // Medium tier over the whole segment, one call per residue class; the
    // rebase is the segment's own width, like process_med64's.
    template <bool NTA>
    void run_medium(uint8_t* bytes, uint64_t bytes_needed) {
        erat::cross_off_medium<0, NTA>(bytes, bytes_needed, medium_dyn_[0].data(), medium_dyn_[0].data() + medium_dyn_[0].size(), medium_qd_[0].data(), medium_qp_base_[0], bytes_needed);
        erat::cross_off_medium<1, NTA>(bytes, bytes_needed, medium_dyn_[1].data(), medium_dyn_[1].data() + medium_dyn_[1].size(), medium_qd_[1].data(), medium_qp_base_[1], bytes_needed);
        erat::cross_off_medium<2, NTA>(bytes, bytes_needed, medium_dyn_[2].data(), medium_dyn_[2].data() + medium_dyn_[2].size(), medium_qd_[2].data(), medium_qp_base_[2], bytes_needed);
        erat::cross_off_medium<3, NTA>(bytes, bytes_needed, medium_dyn_[3].data(), medium_dyn_[3].data() + medium_dyn_[3].size(), medium_qd_[3].data(), medium_qp_base_[3], bytes_needed);
        erat::cross_off_medium<4, NTA>(bytes, bytes_needed, medium_dyn_[4].data(), medium_dyn_[4].data() + medium_dyn_[4].size(), medium_qd_[4].data(), medium_qp_base_[4], bytes_needed);
        erat::cross_off_medium<5, NTA>(bytes, bytes_needed, medium_dyn_[5].data(), medium_dyn_[5].data() + medium_dyn_[5].size(), medium_qd_[5].data(), medium_qp_base_[5], bytes_needed);
        erat::cross_off_medium<6, NTA>(bytes, bytes_needed, medium_dyn_[6].data(), medium_dyn_[6].data() + medium_dyn_[6].size(), medium_qd_[6].data(), medium_qp_base_[6], bytes_needed);
        erat::cross_off_medium<7, NTA>(bytes, bytes_needed, medium_dyn_[7].data(), medium_dyn_[7].data() + medium_dyn_[7].size(), medium_qd_[7].data(), medium_qp_base_[7], bytes_needed);
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
    // instead of relinking an index. SPARSE_BLOCK_ENTRIES=128 (1 KiB/
    // block). See
    // docs/RESEARCH.md#sparse-tier-design-current-fixed-size-pooled-blocks-attempt-6
    // and docs/RESEARCH.md#sparse_block_entries-tuning-1024-vs-128 for why
    // this design (not an earlier idx-indexed intrusive list) and this
    // block size.
    //
    // Drains this segment's ring slot: for each due entry, mark its one
    // hit (byte marking, mod-210 table lookup for mask/step -- see
    // wheel210_big.hpp), advance to the next hit, and re-file it by byte
    // position with a shift/mask instead of a division. `pos` in a live
    // entry is always relative to whichever segment it's due in, so no
    // k_low/k_high parameters are needed here.
    __attribute__((noinline))
    void process_big() {
        const uint32_t slot = static_cast<uint32_t>(cur_segment_ & (num_buckets_ - 1));
        uint8_t* const s = reinterpret_cast<uint8_t*>(words_.data());
        // Local copy of the ring's tail array base: the s[pos] byte store may
        // alias anything, so tail_.data() would otherwise be reloaded from
        // `this` on every hit.
        erat::DenseState** const tails = tail_.data();
        const uint32_t log2sb = log2_sb_;
        const uint64_t modsb = (uint64_t{1} << log2sb) - 1;
        const uint64_t bmask = num_buckets_ - 1;
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
                // fetch the whole next block into L2 now, one block's worth
                // of work ahead. See
                // docs/RESEARCH.md#sparse-tier-prefetch-the-next-block-of-the-chain-once-per-block-kept-2026-09-27.
                if (next_blk) {
                    const char* nb = reinterpret_cast<const char*>(next_blk);
                    for (size_t off = 0; off < BLK_BYTES; off += 64) __builtin_prefetch(nb + off, 0, 2);
                }
                erat::DenseState* it = blk->entries();
                erat::DenseState* end = next_blk ? blk->block_end() : last_end;
                // One 8-byte load per entry and per table row (fields split
                // with shifts), the entry rebuilt as one 8-byte store, and the
                // new-block path out of line: the loop is load-port bound, and
                // field-by-field loads plus spills of the loop constants
                // around the inline allocation cost ~12 loads per hit.
                for (; it != end; ++it) {
                    uint64_t ent;
                    std::memcpy(&ent, it, sizeof(ent)); // qw | pos << 32
                    uint64_t qw = static_cast<uint32_t>(ent);
                    uint64_t pos = ent >> 32;
                    uint64_t a = qw >> 9;
                    uint64_t te = big::TABLE64[qw & 511]; // mask | dm << 8 | corr << 16 | next << 32
                    s[pos] |= static_cast<uint8_t>(te);
                    pos += a * ((te >> 8) & 0xff) + ((te >> 16) & 0xff);
                    uint64_t sl = (cur + (pos >> log2sb)) & bmask;
                    uint64_t e = (a << 9) | (te >> 32) | ((pos & modsb) << 32);
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
    // CHUNK_BLOCKS-sized arenas (indices/pointers into chunks_ stay valid
    // across pool growth since chunks_ holds owning pointers, never moved
    // or resized in place). free_ is a stack of blocks not currently in
    // any ring slot; begin_chunk() repopulates it from every arena ever
    // allocated, never shrinking the pool.
    static constexpr size_t BLK_BYTES = 1024;
    static constexpr size_t CHUNK_BLOCKS = 256;
    struct Blk {
        Blk* next;
        uint64_t pad;
        erat::DenseState* entries() { return reinterpret_cast<erat::DenseState*>(this + 1); }
        erat::DenseState* block_end() { return reinterpret_cast<erat::DenseState*>(reinterpret_cast<char*>(this) + BLK_BYTES); }
    };
    struct AlignedFree { void operator()(char* p) const { std::free(p); } };
    Blk* alloc_blk() {
        if (free_.empty()) {
            char* c = static_cast<char*>(std::aligned_alloc(BLK_BYTES, BLK_BYTES * CHUNK_BLOCKS));
            chunks_.emplace_back(c);
            for (size_t i = 0; i < CHUNK_BLOCKS; ++i) free_.push_back(reinterpret_cast<Blk*>(c + i * BLK_BYTES));
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
    bool med64_nta_;  // prefetchnta the med64 state (process_med64)
    bool med64_210_;  // med64 on the mod-210 wheel: 384 (class, phase) lists, cross_off_checked210
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

    // med64 tier (kept default, see docs/RESEARCH.md): double-buffered, one
    // pair of (class, entry phase) lists swapped every segment instead of
    // migrating entries in place -- see process_med64's own comment.
    // med64_reserved_ survives begin_chunk() on purpose: a vector's
    // capacity survives clear(), so the one-time reserve (sieve_and_emit)
    // only needs to happen once per SegmentSieve instance, not per chunk.
    // 64 lists used on the mod-30 wheel (PR*8+j), all 384 on mod-210 (PR*48+w).
    std::array<std::vector<erat::DenseState>, 384> m64_cur_{};
    std::array<std::vector<erat::DenseState>, 384> m64_nxt_{};
    bool med64_reserved_ = false;

    uint32_t log2_sb_ = 0; // log2(segment width in bytes) -- see constructor
    uint64_t num_buckets_ = 1;
    uint64_t cur_segment_ = 0;

    size_t next_small_idx_ = 0;
    size_t next_med64_idx_ = 0;
    size_t next_medium_idx_ = 0;
    size_t next_sparse_idx_ = 0;
};
