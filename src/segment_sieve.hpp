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
//     stepping via ONFLY_CORRECTION (wheel.hpp: one multiply by qp=p/30,
//     one shared-table lookup, one add). The unrolled loop was measured
//     slower here: with only a few hits per segment it can't amortize its
//     unpredictable entry/exit (see erat_small.hpp::cross_off_medium).
//     Both flat tiers keep 8 bytes/prime of state (erat::DenseState),
//     walked every segment in place -- there is never a segment these
//     primes "skip", so a bucket would buy nothing.
//
//     These two tiers replaced (dev PC, i5-11400F, cycles:u) a per-prime
//     delta[] table tier (40 bytes/prime, capped by an L3/2 budget) plus
//     the ONFLY_CORRECTION loop for everything past that budget, both on
//     the whole L2-sized segment: -26% at N=1e11, -30% at N=1e12.
//   - sparse_primes (p >= segment width, at most ~1 hit/segment): BUCKET.
//     This is where a bucket earns its keep -- most segments have nothing
//     to do for most of these primes, so scheduling each one into the
//     future segment where its next hit actually falls (a fixed-size
//     ring, buckets_) means a segment's processing only ever looks at the
//     (few) sparse primes actually due, not all of them. Steps forward the
//     same qp*GAP_K[j] + ONFLY_CORRECTION[pr][j] way the medium tier does --
//     no division by the runtime value p anywhere in this tier anymore
//     (see attempt 5 below) -- with k and the wheel phase j packed into
//     one word (sparse_kj_) instead of stored separately, so this costs
//     the same 8 bytes/prime as storing plain k did.
//
//     Five changes to this tier's *stepping math* were tried and
//     reverted as net regressions -- two predate splitting
//     process_sparse_bucket (below) into its own noinline function (see
//     that split's own commit): a shared-table scheme like
//     ONFLY_CORRECTION above (~2x slower at N=1e12 with a third of base
//     primes forced sparse) and an AoS relayout of its per-prime state for
//     locality (~2.25x slower), both measured while this whole function
//     (dense + onfly + sparse) was still fully inlined and suffering real
//     register spilling -- any change adding live variables looked
//     catastrophic there regardless of its own merit.
//       - attempt 3 (dev PC session, N=1e13, natural auto -s, 72,036/
//         227,647 base primes sparse): retried the shared-table scheme
//         *after* the noinline split, on the theory that attempt 1's loss
//         was purely the register-spilling confound. It wasn't -- clean
//         same-session A/B via perf stat cycles:u (frequency-independent,
//         not wall-clock): 48.11T cycles vs 42.68T baseline, +12.7%; IPC
//         0.96->0.85; cache-miss rate 9.52%->12.55%. Root cause: this
//         scheme needs qp+pr per prime (OnFlyPrime, 24 bytes padded)
//         instead of the plain uint64_t p (8 bytes) sparse_primes held
//         before, nearly tripling that array's footprint right in the
//         tier accessed in pseudo-random order (via the bucket ring's
//         intrusive list, no locality to begin with) -- costs more in
//         cache pressure than the division (measured elsewhere as ~2.6%
//         of this tier's cycles) saves. Register spilling was real for
//         attempts 1-2, but wasn't the whole story either apparently --
//         or this tier's memory-footprint sensitivity is itself the
//         thing that changed between N=1e12 (attempts 1-2) and N=1e13
//         (attempt 3), given how much bigger the sparse population is at
//         the natural cliff vs a forced-small-N proxy. Reverted.
//       - attempt 4 (superseded by attempt 5 below -- dev PC, N=1e13,
//         natural auto -s, same 72,036/227,647 sparse split as attempt 3):
//         same division-free goal as attempt 3, but stores m instead of adding
//         qp/pr fields, so there's no memory-footprint growth to fight the
//         division's removal with -- wheel_index(p*m) (a multiply plus a
//         compile-time-constant divide) replaces both the old
//         wheel_number(k)/p division *and* the extra per-prime storage
//         attempt 3 needed. Clean same-session A/B via perf stat
//         cycles:u: 41.39T vs 42.68T baseline, -3.0%; wall-clock 1011.38s
//         vs 1094.00s, -7.55%; IPC 0.96->1.00; cache-miss rate flat
//         (9.52%->9.63%, confirming no footprint growth this time).
//       - attempt 5 (dev PC session, same day): a division-by-p removal
//         that fixes attempt 3's actual failure (memory footprint, not
//         the division itself) directly, instead of attempt 4's
//         alternative fix (wheel_index(p*m) instead of the shared-table
//         step). qp=p/WHEEL_MOD and pr=p%WHEEL_MOD are recomputed from p
//         per due-check rather than stored -- both are divisions by the
//         *compile-time* constant WHEEL_MOD, a cheap multiply-shift, not
//         the runtime-p division being removed -- and k+j are packed into
//         one word (sparse_kj_) instead of two, so total per-prime memory
//         stays exactly what attempt 4's plain k took. Measured two ways:
//         a forced-heavy-sparse proxy (N=1e12, -s 500000, 84% sparse, the
//         same trick used elsewhere in this file to test this tier
//         cheaply) showed a clean win -- cycles:u 4.480T->4.112T (-8.2%),
//         wall-clock 96.57s->89.06s (-7.8%), instructions:u down too (not
//         just cycles), cache-miss rate 4.04%->3.48%. At the *natural*
//         N=1e13 cliff (31.6% sparse, this tier only ~17% of total cycles
//         per a perf profile taken this session) the same fix only moved
//         wall-clock 898.63s->892.09s (-0.7%) -- small because the tier
//         itself is still a minority of the work at this N, not because
//         the fix doesn't hold up; instructions:u still dropped
//         (42.819e9->42.293e9), confirming a real if modest effect here,
//         expected to matter more as N grows past 1e13 and this tier's
//         share of total cycles grows with it (see main.cpp's comment on
//         the segment-width L2 cliff). Kept.
//
// Extraction (turning the finished bit array into actual prime values):
// invert each word, decompose into (q, r) = (k / WHEEL_SIZE, k %
// WHEEL_SIZE) once per word, then walk set bits with ctz + clear-lowest-bit.

#include <cstdint>
#include <vector>
#include <algorithm>
#include <stdexcept>

#include "erat_small.hpp"
#include "presieve.hpp"
#include "wheel.hpp"

// Bits needed to hold a wheel phase j (0..wheel_size-1) -- see
// SegmentSieve::sparse_kj_ for what this packs it into. A free function
// (not a class member) because a constexpr member can't be used to
// initialize another static member of the same still-incomplete class.
constexpr int compute_sparse_j_bits(int wheel_size) {
    int bits = 0;
    while ((1 << bits) < wheel_size) ++bits;
    return bits;
}

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
    SegmentSieve(uint64_t seg_k_width, uint64_t base_prime_max, const Presieve& presieve,
                 uint64_t sub_block_bytes)
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
        uint64_t max_gap = 0;
        for (uint64_t g : WHEEL_GAP) max_gap = std::max(max_gap, g);
        // Upper bound on a sparse prime's k-gap between hits: floor_term*
        // WHEEL_SIZE + (WHEEL_SIZE-1), floor_term <= (d+WHEEL_MOD-1)/WHEEL_MOD
        // with d = p*max_gap (see wheel_delta_at in wheel.hpp for where this
        // bound comes from). A few extra WHEEL_SIZE's of slack cost nothing
        // (buckets are cheap) and keep this comfortably safe. Only sparse
        // primes use the bucket ring now (dense/onfly are flat, see header
        // comment), but base_prime_max is the largest base prime overall,
        // so this bound stays valid for whichever primes actually end up
        // sparse.
        uint64_t d = base_prime_max * max_gap;
        uint64_t delta_max = (d / WHEEL_MOD + 1) * static_cast<uint64_t>(WHEEL_SIZE) + 2 * static_cast<uint64_t>(WHEEL_SIZE);
        uint64_t segments_ahead_max = delta_max / seg_k_width_ + 2;
        num_buckets_ = 1;
        while (num_buckets_ < (segments_ahead_max + 1) * 4) num_buckets_ <<= 1; // power of 2, 4x margin
        bucket_head_.assign(num_buckets_, NPOS);
    }

    // Must be called once before the first sieve_and_emit call for a new,
    // independent run of consecutive segments in increasing k order (a
    // thread's chunk). Resets all bucket/flat-tier state and the "which
    // primes have activated yet" pointers. Never reuse a SegmentSieve
    // across threads or out of order.
    void begin_chunk() {
        cur_segment_ = 0;
        next_small_idx_ = 0;
        next_medium_idx_ = 0;
        next_sparse_idx_ = 0;
        for (auto& v : small_) v.clear();
        medium_.clear();
        sparse_kj_.clear();
        sparse_next_.clear();
        std::fill(bucket_head_.begin(), bucket_head_.end(), NPOS);
    }

    template <typename Writer>
    void sieve_and_emit(uint64_t k_low, uint64_t k_high,
                         const std::vector<uint64_t>& small_primes,
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
        // Each of the three lists is sorted by p and gets its own
        // monotonically-advancing pointer, so every prime is visited here
        // exactly once for the whole chunk, not once per segment.
        //
        activate_dense(small_primes, next_small_idx_, small_, true, high_n, low_n, k_low);
        activate_dense(medium_primes, next_medium_idx_, &medium_, false, high_n, low_n, k_low);

        while (next_sparse_idx_ < sparse_primes.size()) {
            uint64_t p = sparse_primes[next_sparse_idx_];
            if (p * p >= high_n) break;

            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;
            uint64_t k = wheel_index(p * m);
            uint64_t j = static_cast<uint64_t>(WHEEL_POS[r]);

            // sparse_kj_/sparse_next_ grow in lockstep with
            // next_sparse_idx_ (see the pool comment near their
            // declaration), so this index is exactly where this prime's
            // permanent slot lives. sparse_kj_ packs k and the wheel phase
            // j into one word (k << SPARSE_J_BITS | j) -- see
            // process_sparse_bucket for the division-free stepping this
            // enables at no extra memory cost.
            sparse_kj_.push_back((k << SPARSE_J_BITS) | j);
            sparse_next_.push_back(NPOS);
            schedule_sparse(static_cast<uint32_t>(next_sparse_idx_), k, k_high);
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

        // Medium tier: few hits per sub-block, so one pass over the whole
        // segment each.
        erat::cross_off_medium(words_.data(), count, medium_.data(), medium_.data() + medium_.size(), count);

        // Sparse tier: see process_sparse_bucket below (pulled out of this
        // function on purpose -- see its own comment). sparse_primes is
        // either empty for the whole run or not -- never changes segment
        // to segment -- so skipping the call entirely when it's empty
        // avoids paying a real (non-inlined) call's overhead every single
        // segment for N where this tier never has anything to do (every N
        // tested up to 1e12 on this machine, see README#benchmarks).
        if (!sparse_primes.empty()) process_sparse_bucket(k_low, k_high, sparse_primes);
        ++cur_segment_;

        // Extraction: bit=0 => prime candidate.
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
                prime_count += static_cast<uint64_t>(__builtin_popcountll(bits));
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
                ++prime_count;
                bits &= bits - 1; // clear the lowest set bit
            }
        }
    }

private:
    static constexpr uint32_t NPOS = static_cast<uint32_t>(-1);

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

            // Smallest m coprime with WHEEL_MOD with p*m >= max(p*p, low_n).
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;

            // Byte of p*m is (p*m)/30 (k = byte*8 + bit, see erat_small.hpp).
            uint64_t pos = by_class ? (p * m) / WHEEL_MOD - k_low / 8 : wheel_index(p * m) - k_low;
            uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
            uint64_t j = static_cast<uint64_t>(WHEEL_POS[r]);
            state[by_class ? pr : 0].push_back({static_cast<uint32_t>(((p / WHEEL_MOD) << 6) | (pr << 3) | j),
                             static_cast<uint32_t>(pos)});
            ++next;
        }
    }

    // Exact minimum bits for the active WHEEL_SIZE (3 for mod 30's 8, 9
    // for mod 2310's 480), not a fixed guess, so the k budget this leaves
    // (64 minus this) only ever shrinks as much as the active wheel
    // actually needs.
    static constexpr int SPARSE_J_BITS = compute_sparse_j_bits(WHEEL_SIZE);
    static constexpr uint64_t SPARSE_J_MASK = (uint64_t{1} << SPARSE_J_BITS) - 1;

    // Processes exactly the sparse-tier entries due this segment: mark,
    // advance, reschedule into whichever future bucket the next hit lands
    // in. Each activated sparse prime owns exactly one permanent slot in
    // sparse_kj_/sparse_next_ for the rest of the chunk (see the pool
    // comment near their declaration) -- "the bucket" is just that slot's
    // index appearing in this ring position's intrusive list, relinked
    // into a new position by schedule_sparse, never allocated or freed
    // again after activation.
    //
    // Pulled out of sieve_and_emit into its own function, and marked
    // noinline to make sure it stays that way even under -O3/-flto: with
    // dense/onfly/sparse all fully inlined into one function, perf showed
    // real cycles going to a spilled-to-stack reload of a loop-invariant
    // member (num_buckets_) inside what's now schedule_sparse -- the
    // *number* of values simultaneously live across all three tiers was
    // forcing spills, not a poor choice of which value to spill. Passing
    // values in as explicit parameters instead of member reads didn't
    // change anything measured (see git history) because that doesn't
    // reduce how many values are live at once, only where they come from.
    // Giving this tier its own function gives it its own register
    // allocation scope instead, so its live ranges stop competing with
    // the other two tiers' for the same register file. Confirmed with
    // perf stat, not just wall-clock (which turned out noisy for this
    // tier -- see README#benchmarks): at N=1e12 with a third of base
    // primes forced sparse (-s 500000), cycles dropped ~5-9% and IPC rose
    // from 1.07 to 1.14-1.19 across repeated runs, consistently. The
    // flip side of a real function call is real call overhead, paid once
    // per segment even when this tier has nothing due -- sieve_and_emit
    // below skips the call entirely when sparse_primes is empty for the
    // whole run (every N up to 1e12 tested on this machine), which is
    // what keeps the dense-only case's numbers unchanged from before this
    // split.
    __attribute__((noinline))
    void process_sparse_bucket(uint64_t k_low, uint64_t k_high, const std::vector<uint64_t>& sparse_primes) {
        uint32_t slot = static_cast<uint32_t>(cur_segment_ & (num_buckets_ - 1));
        uint32_t idx = bucket_head_[slot];
        bucket_head_[slot] = NPOS;
        while (idx != NPOS) {
            uint32_t next_idx = sparse_next_[idx]; // save: schedule_sparse below overwrites it
            // Attempt 5 (see the tier's header comment for 1-4): same
            // division-free stepping as the medium tier (qp*GAP_K[j] +
            // ONFLY_CORRECTION[pr][j]), but qp/pr are recomputed from p
            // here instead of stored -- p/WHEEL_MOD and p%WHEEL_MOD are
            // divisions by the *compile-time* constant WHEEL_MOD (a cheap
            // multiply-shift the compiler generates), not the runtime-p
            // division attempt 4 already removed, so there's no real cost
            // to redoing them per due-check instead of caching them.
            // sparse_kj_ packs k and the phase j into one word -- same 8
            // bytes/prime as attempt 4's plain k, avoiding attempt 3's
            // actual failure mode (a 40-byte/prime total after adding a
            // stored qp/pr struct *and* separate k/j/next arrays).
            uint64_t p = sparse_primes[idx];
            uint64_t packed = sparse_kj_[idx];
            uint64_t k = packed >> SPARSE_J_BITS;
            uint32_t j = static_cast<uint32_t>(packed & SPARSE_J_MASK);
            uint64_t qp = p / WHEEL_MOD;
            uint32_t pr = static_cast<uint32_t>(WHEEL_POS[p % WHEEL_MOD]);
            while (k < k_high) {
                uint64_t widx = k - k_low;
                words_[widx >> 6] |= (1ULL << (widx & 63));
                k += qp * GAP_K[j] + ONFLY_CORRECTION[pr][j];
                if constexpr (WHEEL_SIZE_IS_POW2) {
                    j = (j + 1) & (WHEEL_SIZE - 1);
                } else {
                    ++j;
                    if (j == WHEEL_SIZE) j = 0;
                }
            }
            sparse_kj_[idx] = (k << SPARSE_J_BITS) | j;
            schedule_sparse(idx, k, k_high);
            idx = next_idx;
        }
    }

    // Schedules (or reschedules) sparse prime `idx` into the bucket ring
    // slot for whichever segment k falls into, given that the segment
    // currently being processed ends at k_high. k < k_high means "due
    // right now" (this segment's bucket); asserts rather than silently
    // corrupting results if the ring ever turns out too small for the
    // actual data (see constructor).
    //
    // No allocation here, ever, after activation: idx's slot in sparse_kj_/
    // sparse_next_ already exists (see the pool comment near their
    // declaration) -- this just relinks it onto the target ring slot's
    // list (sparse_kj_[idx] is the caller's job to update first -- k here
    // is only used to place the slot, never stored, since sparse_kj_ is now
    // the persisted state -- see process_sparse_bucket). That's the actual
    // memory-pool win over a std::vector<Entry> per bucket: no per-bucket
    // heap allocation to begin with, and no reallocation on reschedule
    // either, since a prime never needs more than the one slot it was
    // given at activation.
    void schedule_sparse(uint32_t idx, uint64_t k, uint64_t k_high) {
        uint64_t segments_ahead = (k < k_high) ? 0 : (k - k_high) / seg_k_width_ + 1;
        if (segments_ahead >= num_buckets_) {
            throw std::runtime_error(
                "bucket sieve: salto de un primo disperso mayor que el margen del anillo de "
                "cubos (bug de dimensionamiento en el constructor de SegmentSieve)");
        }
        uint32_t slot = static_cast<uint32_t>((cur_segment_ + segments_ahead) & (num_buckets_ - 1));
        sparse_next_[idx] = bucket_head_[slot];
        bucket_head_[slot] = idx;
    }

    std::vector<uint64_t> words_;
    uint64_t seg_k_width_;
    uint64_t sub_block_bytes_;
    const Presieve& presieve_;

    // Dense tiers' per-prime state (erat_small.hpp), in lockstep with
    // small_primes/medium_primes as they activate -- no bucket, walked
    // every segment (small: every sub-block).
    std::vector<erat::DenseState> small_[8]; // one list per residue class p % 30
    std::vector<erat::DenseState> medium_;

    // Sparse tier's bucket ring, as an intrusive linked list over a flat
    // pool instead of one std::vector<Entry> per ring slot: sparse_kj_/
    // sparse_next_ grow in lockstep with next_sparse_idx_ as primes
    // activate (mirroring small_/medium_ above), so sparse_kj_[i] is
    // always prime i's current (k, wheel phase j) packed into one word
    // (see process_sparse_bucket's attempt-5 comment) and sparse_next_[i]
    // chains it into whichever ring slot's list it's currently scheduled
    // on.
    // bucket_head_[slot] is that list's head index, or NPOS if empty. A
    // prime is relinked (schedule_sparse) every time it's processed, but
    // its slot in sparse_kj_/sparse_next_ is allocated exactly once, at
    // activation -- no heap allocation on the hot path at all.
    //
    // An AoS layout (one {k, p, next} struct per prime, instead of these
    // three parallel arrays) was tried here on the theory that it would
    // turn up to 3 likely cache misses per hit into 1 -- measured *worse*
    // (~2.25x slower at N=1e12 with a third of base primes forced sparse,
    // on top of the plain-division baseline, which was already ~2x
    // primesieve's own class of cost there). Second regression in a row
    // from a locality-motivated change to this tier (see the division
    // removal attempt above it in git history) -- the actual bottleneck
    // here still isn't understood; see main.cpp's comment on this tier.
    uint64_t num_buckets_ = 1;
    std::vector<uint32_t> bucket_head_;
    // k and the wheel phase j, packed into one word -- see
    // process_sparse_bucket's attempt-5 comment for why, at no memory
    // cost over storing plain k (same 8 bytes/prime either way).
    std::vector<uint64_t> sparse_kj_;
    std::vector<uint32_t> sparse_next_;
    uint64_t cur_segment_ = 0;

    size_t next_small_idx_ = 0;
    size_t next_medium_idx_ = 0;
    size_t next_sparse_idx_ = 0;
};
