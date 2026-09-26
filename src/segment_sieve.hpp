#pragma once
// Segmented sieve on a compile-time wheel (see wheel.hpp), bit-packed into
// uint64_t words.
//
// Three prime tiers, chosen by expected hits (see main.cpp for the actual
// cutoffs) -- this mirrors primesieve's own split (EratSmall / EratMedium /
// EratBig): a prime p's average gap between hits, in wheel-index terms, is
// ~p (each wheel-coprime multiplier step advances the value by
// ~p*WHEEL_MOD/WHEEL_SIZE, which converts back to a k-gap of ~p).
//
//   - small_primes (p < L1d/2 bytes, >= ~16 hits per L1-sized sub-block):
//     FLAT, crossed off one L1-sized sub-block at a time (presieve fill
//     first, then every small prime), so the marks -- ~80%+ of all of them
//     -- land in L1 instead of the L2-sized segment. Uses erat_small.hpp's
//     unrolled 8-hits-per-cycle loop with compile-time bit masks, one list
//     per residue class p % 30 (small_[pr]) so the class dispatch isn't an
//     unpredictable branch per prime.
//   - medium_primes (up to the segment width, so >= ~1 hit/segment): FLAT,
//     one pass over the whole segment, generic one-hit-per-iteration
//     stepping via shared mod-210 tables (wheel210_big.hpp::GAP_K210/
//     ONFLY_CORRECTION210: one multiply by qp=p/30, one shared-table
//     lookup, one add -- 48 multiplier phases instead of the mod-30
//     wheel's 8, skipping hits redundant with presieve's own coverage of
//     7 -- see erat_small.hpp::cross_off_medium for the full numbers).
//     The unrolled loop was measured slower here: with only a few hits
//     per segment it can't amortize its unpredictable entry/exit (see
//     erat_small.hpp::cross_off_medium). An EratMedium-style 64-list
//     restructuring (byte marking like the small tier, keyed by (class,
//     entry phase)) was tried twice, in two different implementations,
//     and reverted both times -- see docs/RESEARCH.md.
//
//     EXPERIMENT (med64_primes, process_med64): a THIRD variant of that
//     same 64-list idea, scoped to only the sub-band of medium primes
//     closest to small_limit ([small_limit, med64_limit), main.cpp) --
//     the two prior attempts applied it to the whole medium tier, whose
//     population keeps growing with N past small_limit's own saturation
//     point (see erat_small.hpp), which is what sank both of them at
//     large N; this band's own population saturates far earlier (at or
//     before small_limit's), so it shouldn't. Double-buffered like the
//     second prior attempt (m64_cur_/m64_nxt_, swapped per segment) --
//     see process_med64's own comment. Swept via
//     ERATOSTENES_MED64_NUM/_DEN (main.cpp): 1/8 kept as the default,
//     measured a real win at both N=1e12 and N=1e13 (unlike the full-tier
//     attempts above) -- see docs/RESEARCH.md for the full sweep.
//     Both flat tiers keep 8 bytes/prime of state (erat::DenseState),
//     walked every segment in place -- there is never a segment these
//     primes "skip", so a bucket would buy nothing.
//
//     These two tiers replaced (dev PC, i5-11400F, cycles:u) a per-prime
//     delta[] table tier (40 bytes/prime, capped by an L3/2 budget) plus
//     the ONFLY_CORRECTION loop for everything past that budget, both on
//     the whole L2-sized segment: -26% at N=1e11, -30% at N=1e12.
//
// dTLB pressure from a thread's whole working set at large N (the 256KiB
// segment array, ~1.2MB of medium-tier DenseState at the N=1e13 cliff,
// plus the sparse ring's blocks scattered across many 4KiB pages) was
// investigated as a possible explanation for this project's ratio gap
// against primesieve and ruled out -- measured dTLB miss rates are
// ~0.003-0.004%, nowhere near enough to matter. See docs/RESEARCH.md.
//
//   - sparse_primes (p >= segment width, at most ~1 hit/segment): BUCKET,
//     EratBig-style (adopted after an isolated A/B against the original
//     design below) -- byte marking (not bit), a mod-210 multiplier wheel
//     (48/210 phases instead of 8/30, same redundant-multiple-of-7
//     reasoning as the medium tier above), and pointer-aligned blocks (a
//     tail pointer landing exactly on a block boundary means "full",
//     primesieve's own Bucket trick, no per-block count field to load).
//     Requires a power-of-2 segment width in BYTES (see the constructor's
//     has_sparse check) for the bucket-slot math to become a shift/mask
//     instead of a division -- main.cpp floors seg_k_width to the nearest
//     power of 2 whenever this tier is used.
//
//     Most segments have nothing to do for most of these primes, so
//     scheduling each one into the future segment where its next hit
//     actually falls (a fixed-size ring of block-pooled queues, see
//     process_big's own comment) means a segment's processing only ever
//     looks at the (few) sparse primes actually due, not all of them.
//     Steps forward the same qp*GAP_K[j] + ONFLY_CORRECTION[pr][j] way the
//     medium tier does -- no division by the runtime value p anywhere in
//     this tier -- with qp/pr/the wheel phase j packed into one word
//     (erat::DenseState::qw, same layout as the dense tiers) alongside a
//     `pos` relative to whichever segment the entry is due in, 8 bytes
//     total per live entry. Several earlier designs for this tier's
//     stepping math (a shared-table scheme, an AoS relayout, a couple of
//     division-free variants) were tried and reverted before landing on
//     this one -- see docs/RESEARCH.md for that history.
//
// Extraction (turning the finished bit array into actual prime values):
// invert each word, decompose into (q, r) = (k / WHEEL_SIZE, k %
// WHEEL_SIZE) once per word, then walk set bits with ctz + clear-lowest-bit.

#include <array>
#include <cstdint>
#include <vector>
#include <memory>
#include <cstdlib>
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
                 uint64_t sub_block_bytes, bool has_sparse)
        : words_((seg_k_width + 63) / 64, 0),
          seg_k_width_(seg_k_width),
          sub_block_bytes_(sub_block_bytes),
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
        for (auto& v : medium_) v.clear();
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
            size_t per_list = med64_primes.size() / 64 * 2 + 16;
            for (auto& v : m64_cur_) v.reserve(per_list);
            for (auto& v : m64_nxt_) v.reserve(per_list);
            med64_reserved_ = true;
        }
        activate_dense(small_primes, next_small_idx_, small_, true, high_n, low_n, k_low);
        activate_med64(med64_primes, next_med64_idx_, m64_cur_.data(), high_n, low_n, k_low);
        activate_medium(medium_primes, next_medium_idx_, medium_, high_n, low_n, k_low);

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
            process_med64<0>(bytes, bytes_needed);
            process_med64<1>(bytes, bytes_needed);
            process_med64<2>(bytes, bytes_needed);
            process_med64<3>(bytes, bytes_needed);
            process_med64<4>(bytes, bytes_needed);
            process_med64<5>(bytes, bytes_needed);
            process_med64<6>(bytes, bytes_needed);
            process_med64<7>(bytes, bytes_needed);
            for (auto& v : m64_cur_) v.clear();
            std::swap(m64_cur_, m64_nxt_);
        }

        // Medium tier: one pass over the whole segment each, one list per
        // residue class (medium_[pr]) so PR is a compile-time template
        // parameter in cross_off_medium<PR>, same reasoning as the small
        // tier's cross_off_class<PR> calls just above.
        erat::cross_off_medium<0>(words_.data(), count, medium_[0].data(), medium_[0].data() + medium_[0].size(), count);
        erat::cross_off_medium<1>(words_.data(), count, medium_[1].data(), medium_[1].data() + medium_[1].size(), count);
        erat::cross_off_medium<2>(words_.data(), count, medium_[2].data(), medium_[2].data() + medium_[2].size(), count);
        erat::cross_off_medium<3>(words_.data(), count, medium_[3].data(), medium_[3].data() + medium_[3].size(), count);
        erat::cross_off_medium<4>(words_.data(), count, medium_[4].data(), medium_[4].data() + medium_[4].size(), count);
        erat::cross_off_medium<5>(words_.data(), count, medium_[5].data(), medium_[5].data() + medium_[5].size(), count);
        erat::cross_off_medium<6>(words_.data(), count, medium_[6].data(), medium_[6].data() + medium_[6].size(), count);
        erat::cross_off_medium<7>(words_.data(), count, medium_[7].data(), medium_[7].data() + medium_[7].size(), count);

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
    // (state[pr]); otherwise bit positions, all into state[0].
    static void activate_dense(const std::vector<uint64_t>& primes, size_t& next,
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
    static void activate_med64(const std::vector<uint64_t>& primes, size_t& next,
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

    // Medium tier: smallest m coprime with 210 (not just 30) with
    // p*m >= start_val -- every medium prime is > 163, so multiples of 7
    // are always redundant here (see erat_small.hpp::cross_off_medium).
    // Bit position, (qp<<6)|w packing, one list per residue class
    // (medium_[pr]) so cross_off_medium<PR> gets PR as a compile-time
    // template parameter -- same shape as activate_dense's small-tier
    // branch above, just mod-210 stepping instead of mod-30. Same t/w
    // decomposition as the sparse tier's own EratBig-style activation.
    static void activate_medium(const std::vector<uint64_t>& primes, size_t& next,
                                 std::vector<erat::DenseState>* medium,
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
            uint64_t pos = wheel_index(p * m) - k_low;
            medium[pr].push_back({static_cast<uint32_t>(((p / WHEEL_MOD) << 6) | w),
                                   static_cast<uint32_t>(pos)});
            ++next;
        }
    }

    // med64 tier (EXPERIMENT -- see docs/RESEARCH.md): PR a compile-time
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
    template <int PR>
    void process_med64(uint8_t* bytes, uint64_t bytes_needed) {
        for (int j = 0; j < 8; ++j) {
            for (erat::DenseState& st : m64_cur_[PR * 8 + j]) {
                uint64_t i = st.pos;
                uint64_t qp = st.qw >> 6;
                uint32_t jj = st.qw & 7;
                erat::cross_off<PR>(bytes, bytes_needed, qp, i, jj);
                m64_nxt_[PR * 8 + jj].push_back(
                    {static_cast<uint32_t>((qp << 6) | (PR << 3) | jj), static_cast<uint32_t>(i - bytes_needed)});
            }
        }
    }

    // Processes exactly the sparse-tier entries due this segment: mark,
    // advance, reschedule into whichever future bucket the next hit lands
    // in -- see the block-pool comment right below for how a due entry
    // gets there and where it goes next.
    //
    // Pulled out of sieve_and_emit into its own function, and marked
    // noinline to make sure it stays that way even under -O3/-flto: with
    // dense/onfly/sparse all fully inlined into one function, perf showed
    // real cycles going to a spilled-to-stack reload of a loop-invariant
    // member (num_buckets_) -- the *number* of values simultaneously live
    // across all three tiers was forcing spills, not a poor choice of
    // which value to spill. Giving this tier its own function gives it its
    // own register allocation scope instead, so its live ranges stop
    // competing with the other two tiers' for the same register file.
    // Confirmed with perf stat, not just wall-clock: at N=1e12 with a
    // third of base primes forced sparse (-s 500000), cycles dropped ~5-9%
    // and IPC rose from 1.07 to 1.14-1.19 across repeated runs,
    // consistently. The flip side of a real function call is real call
    // overhead, paid once per segment even when this tier has nothing due
    // -- sieve_and_emit below skips the call entirely when sparse_primes
    // is empty for the whole run, which is what keeps the dense-only
    // case's numbers unchanged from before this split.
    //
    // Fixed-size blocks of erat::DenseState pulled from a pool, one queue
    // (linked list of blocks) per ring slot -- primesieve's own EratBig
    // design, replacing an earlier idx-indexed intrusive list (see
    // docs/RESEARCH.md for that history and why it lost). The entry itself
    // carries everything needed to process it (qp/pr/j packed into `qw`,
    // `pos` relative to whichever segment it's due in, same layout as the
    // dense tiers' DenseState), so rescheduling COPIES the entry into the
    // target slot's tail block instead of relinking an index -- both the
    // read (draining a slot's blocks front to back) and the write
    // (appending to a tail) are sequential. A live entry costs exactly 8
    // bytes (sizeof(DenseState)) at any one time.
    //
    // SPARSE_BLOCK_ENTRIES=128 (1 KiB/block) was tuned against both a
    // forced-heavy-sparse proxy and the natural N=1e13 cliff after the two
    // regimes initially disagreed at the primesieve-default 1024 -- see
    // docs/RESEARCH.md for the numbers and why they disagreed. Several
    // further changes (rounding the ring-slot division away, prefetching
    // the hit loop's next target, 4-way software-pipelining it, a
    // sliding-window ring instead of the modular one below) were all tried
    // and reverted -- see docs/RESEARCH.md.
    // Drains this segment's ring slot: for each due entry, mark its one
    // hit (byte marking, mod-210 table lookup for mask/step -- see
    // wheel210_big.hpp), advance to the next hit, and re-file it (usually
    // into a future slot, occasionally the current one again if the step
    // is small) by byte position with a shift/mask instead of a division.
    // `pos` in a live entry is always relative to whichever segment it's
    // due in, so no k_low/k_high parameters are needed here at all, unlike
    // the old scheme.
    __attribute__((noinline))
    void process_big() {
        const uint32_t slot = static_cast<uint32_t>(cur_segment_ & (num_buckets_ - 1));
        uint8_t* const s = reinterpret_cast<uint8_t*>(words_.data());
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
                erat::DenseState* it = blk->entries();
                erat::DenseState* end = next_blk ? blk->block_end() : last_end;
                for (; it != end; ++it) {
                    uint32_t qw = it->qw;
                    uint64_t pos = it->pos;
                    uint64_t a = qw >> 9;
                    uint32_t idx = qw & 511;
                    const big::Entry& te = big::TABLE[idx];
                    s[pos] |= te.mask;
                    pos += a * te.dm + te.corr;
                    uint64_t ahead = pos >> log2sb;
                    erat::DenseState e{static_cast<uint32_t>((a << 9) | te.next), static_cast<uint32_t>(pos & modsb)};
                    push_sparse_entry(static_cast<uint32_t>((cur + ahead) & bmask), e);
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
        if (w == nullptr || (reinterpret_cast<uintptr_t>(w) & (BLK_BYTES - 1)) == 0) {
            Blk* nb = alloc_blk();
            nb->next = nullptr;
            if (w == nullptr) head_[slot] = nb;
            else reinterpret_cast<Blk*>(reinterpret_cast<char*>(w) - BLK_BYTES)->next = nb;
            w = nb->entries();
        }
        *w = entry;
        tail_[slot] = w + 1;
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
    const Presieve& presieve_;

    // Dense tiers' per-prime state (erat_small.hpp), in lockstep with
    // small_primes/medium_primes as they activate -- no bucket, walked
    // every segment (small: every sub-block).
    std::vector<erat::DenseState> small_[8]; // one list per residue class p % 30
    std::vector<erat::DenseState> medium_[8]; // one list per residue class p % 30

    // med64 tier (EXPERIMENT, see docs/RESEARCH.md): double-buffered, one
    // pair of (class, entry phase) lists swapped every segment instead of
    // migrating entries in place -- see process_med64's own comment.
    // med64_reserved_ survives begin_chunk() on purpose: a vector's
    // capacity survives clear(), so the one-time reserve (sieve_and_emit)
    // only needs to happen once per SegmentSieve instance, not per chunk.
    std::array<std::vector<erat::DenseState>, 64> m64_cur_{};
    std::array<std::vector<erat::DenseState>, 64> m64_nxt_{};
    bool med64_reserved_ = false;

    uint32_t log2_sb_ = 0; // log2(segment width in bytes) -- see constructor
    uint64_t num_buckets_ = 1;
    uint64_t cur_segment_ = 0;

    size_t next_small_idx_ = 0;
    size_t next_med64_idx_ = 0;
    size_t next_medium_idx_ = 0;
    size_t next_sparse_idx_ = 0;
};
