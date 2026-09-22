#pragma once
// Segmented sieve on a compile-time wheel (see wheel.hpp), bit-packed into
// uint64_t words.
//
// Two scheduling strategies, chosen per prime tier by expected hits per
// segment (see SMALL_PRIME_LIMIT/TABLE_BYTES_BUDGET in main.cpp for the
// actual cutoffs) -- this mirrors primesieve's own split (EratSmall/
// EratMedium vs EratBig): a prime p's average gap between hits, in
// wheel-index terms, is ~p (each wheel-coprime multiplier step advances
// the value by ~p*WHEEL_MOD/WHEEL_SIZE, which converts back to a k-gap of
// ~p).
//
//   - wheel_base_primes and dense_onfly_primes (p < segment width, so
//     >=1 hit/segment on average): FLAT, no bucket. Each activated prime's
//     (k, j) state lives in a plain parallel array (dense_k_/dense_j_,
//     onfly_k_/onfly_j_) and is walked *every* segment, updating that same
//     slot in place -- there is never a segment these primes "skip", so a
//     bucket's whole reason to exist (letting a segment's processing touch
//     only the primes actually due) buys nothing here, and previously cost
//     a schedule() call (bounds check + vector push_back) every segment
//     for no benefit. wheel_base_primes tracks phase j into a per-prime
//     delta[] table (wheel.hpp) -- cheap reuse, and few enough of these
//     primes that the table stays tiny (see TABLE_BYTES_BUDGET). Once a
//     prime needs recomputing the same phase's advance on the fly instead,
//     it reads it from ONFLY_CORRECTION (wheel.hpp) -- a small table
//     *shared* across every such prime (one multiply by the prime's own
//     qp=p/WHEEL_MOD, one lookup, one add; no division by p, no per-prime
//     memory cost either).
//   - sparse_primes (p >= segment width, at most ~1 hit/segment): BUCKET.
//     This is where a bucket earns its keep -- most segments have nothing
//     to do for most of these primes, so scheduling each one into the
//     future segment where its next hit actually falls (a fixed-size
//     ring, buckets_) means a segment's processing only ever looks at the
//     (few) sparse primes actually due, not all of them. Stores the
//     multiplier m itself across segments (sparse_m_) and derives k from
//     it via wheel_index (p*m, then a divide by the compile-time constant
//     WHEEL_MOD) -- no division by the runtime value p anymore (see
//     attempt 4 below); same 8 bytes/prime as storing k would take.
//
//     Four changes to this tier's *stepping math* were tried and
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
//       - attempt 4 (this one, kept -- dev PC, N=1e13, natural auto -s,
//         same 72,036/227,647 sparse split as attempt 3): same
//         division-free goal as attempt 3, but stores m instead of adding
//         qp/pr fields, so there's no memory-footprint growth to fight the
//         division's removal with -- wheel_index(p*m) (a multiply plus a
//         compile-time-constant divide) replaces both the old
//         wheel_number(k)/p division *and* the extra per-prime storage
//         attempt 3 needed. Clean same-session A/B via perf stat
//         cycles:u: 41.39T vs 42.68T baseline, -3.0%; wall-clock 1011.38s
//         vs 1094.00s, -7.55%; IPC 0.96->1.00; cache-miss rate flat
//         (9.52%->9.63%, confirming no footprint growth this time). Kept.
//
// Extraction (turning the finished bit array into actual prime values):
// invert each word, decompose into (q, r) = (k / WHEEL_SIZE, k %
// WHEEL_SIZE) once per word, then walk set bits with ctz + clear-lowest-bit.

#include <cstdint>
#include <vector>
#include <algorithm>
#include <stdexcept>

#include "presieve.hpp"
#include "wheel.hpp"

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
    // be excluded from wheel_base_primes by the caller.
    SegmentSieve(uint64_t seg_k_width, uint64_t base_prime_max, const Presieve& presieve)
        : words_((seg_k_width + 63) / 64, 0),
          seg_k_width_(seg_k_width),
          presieve_(presieve) {
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
        next_prime_idx_ = 0;
        next_onfly_idx_ = 0;
        next_sparse_idx_ = 0;
        dense_k_.clear();
        dense_j_.clear();
        onfly_k_.clear();
        onfly_j_.clear();
        sparse_m_.clear();
        sparse_next_.clear();
        std::fill(bucket_head_.begin(), bucket_head_.end(), NPOS);
    }

    template <typename Writer>
    void sieve_and_emit(uint64_t k_low, uint64_t k_high,
                         const std::vector<WheelBasePrime>& wheel_base_primes,
                         const std::vector<OnFlyPrime>& dense_onfly_primes,
                         const std::vector<uint64_t>& sparse_primes,
                         Writer& out, uint64_t& prime_count) {
        uint64_t count = (k_high > k_low) ? (k_high - k_low) : 0;
        if (count == 0) return;

        size_t words_needed = (count + 63) / 64;
        presieve_.fill(words_.data(), k_low, count);

        uint64_t high_n = wheel_number(k_high); // exclusive numeric bound, valid for the p*p cutoff
        uint64_t low_n = wheel_number(k_low);

        // Activate any base primes that just became relevant (p*p < high_n).
        // Each of the three lists is sorted by p and gets its own
        // monotonically-advancing pointer, so every prime is visited here
        // exactly once for the whole chunk, not once per segment.
        //
        // Dense/onfly activation appends to dense_k_/dense_j_ (or
        // onfly_k_/onfly_j_) in lockstep with next_prime_idx_/
        // next_onfly_idx_, so dense_k_[i] is always primeN's state, no
        // separate index needed -- see the flat loops below.
        while (next_prime_idx_ < wheel_base_primes.size()) {
            uint64_t p = wheel_base_primes[next_prime_idx_].p;
            if (p * p >= high_n) break;

            // Find the smallest m coprime with WHEEL_MOD such that
            // p*m >= max(p*p, low_n): the prime's first relevant multiple.
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;

            dense_k_.push_back(wheel_index(p * m));
            dense_j_.push_back(WHEEL_POS[r]);
            ++next_prime_idx_;
        }

        while (next_onfly_idx_ < dense_onfly_primes.size()) {
            uint64_t p = dense_onfly_primes[next_onfly_idx_].p;
            if (p * p >= high_n) break;

            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;

            onfly_k_.push_back(wheel_index(p * m));
            onfly_j_.push_back(WHEEL_POS[r]);
            ++next_onfly_idx_;
        }

        while (next_sparse_idx_ < sparse_primes.size()) {
            uint64_t p = sparse_primes[next_sparse_idx_];
            if (p * p >= high_n) break;

            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            uint64_t k = wheel_index(p * m);

            // sparse_m_/sparse_next_ grow in lockstep with next_sparse_idx_
            // (see the pool comment near their declaration), so this index
            // is exactly where this prime's permanent slot lives. sparse_m_
            // stores the multiplier m itself (not k) -- see
            // process_sparse_bucket for why.
            sparse_m_.push_back(m);
            sparse_next_.push_back(NPOS);
            schedule_sparse(static_cast<uint32_t>(next_sparse_idx_), k, k_high);
            ++next_sparse_idx_;
        }

        // Flat tiers: every activated prime is due *every* segment (that's
        // the whole point of not bucketing them -- see header comment), so
        // just walk the full activated range and update each slot in place.
        // No reschedule bookkeeping, no bucket indirection.
        size_t dense_activated = dense_k_.size();
        for (size_t i = 0; i < dense_activated; ++i) {
            // Software prefetch a few entries ahead: wheel_base_primes[i]
            // is walked strictly sequentially (good for the hardware
            // prefetcher on its own), but each iteration's while-loop below
            // runs a variable, data-dependent number of times before
            // moving to i+1 -- that variability can starve the hardware
            // prefetcher's lookahead. Explicit here since it's a genuinely
            // different access pattern from the sparse tier's bucket-ring
            // prefetch attempt (pointer-chasing, reverted -- see
            // process_sparse_bucket's comment): this one is sequential and
            // predictable, the case software prefetch is actually meant
            // for.
            //
            // Kept (dev PC, N=1e13, natural auto -s, on top of the
            // division-free sparse tier above): cache-miss rate dropped
            // 9.63%->5.67%, and wall-clock 1011.38s->901.61s (-10.85%) --
            // but cycles:u went slightly *up* (+3.2%), the opposite of
            // what decided every other change on this tier. Reconciled by
            // average frequency (cycles:u/task-clock): 3.478GHz->3.982GHz.
            // Fewer memory stalls let the core sustain a higher clock, so
            // more cycles executed in less wall-time isn't a contradiction
            // here -- but it did produce a genuine false alarm before
            // landing on this number: a same-session "confirmation" rerun
            // launched immediately after the first (no idle gap) came back
            // at 1273.77s/48.74T cycles, matching this project's own
            // documented PL1/Tau back-to-back-long-runs trap (see git
            // history) almost exactly, with instructions:u identical
            // across every run (~42.819e9) confirming it was the same code
            // under different power/thermal states, not a different
            // execution path. Settled only after a real cooldown (wsl
            // --shutdown, fresh boot, machine otherwise idle) reproduced
            // the fast number twice. Reproduced a third time (898.63s,
            // 5.72% miss rate) after an unrelated editing slip briefly
            // deleted this very prefetch line while only touching the
            // comment above it -- caught because the "no-prefetch" number
            // that slip produced (892.06s) didn't fit either cluster
            // (901s with, 1011s without), which is what flagged it as
            // wrong rather than a third data point.
            if (i + 4 < dense_activated) __builtin_prefetch(&wheel_base_primes[i + 4], 0, 1);
            const auto& delta = wheel_base_primes[i].delta;
            uint64_t k = dense_k_[i];
            int j = dense_j_[i];
            while (k < k_high) {
                uint64_t idx = k - k_low;
                words_[idx >> 6] |= (1ULL << (idx & 63));
                k += delta[j];
                if constexpr (WHEEL_SIZE_IS_POW2) {
                    j = (j + 1) & (WHEEL_SIZE - 1); // branchless wraparound
                } else {
                    ++j;
                    if (j == WHEEL_SIZE) j = 0;
                }
            }
            dense_k_[i] = k;
            dense_j_[i] = j;
        }

        size_t onfly_activated = onfly_k_.size();
        for (size_t i = 0; i < onfly_activated; ++i) {
            uint64_t qp = dense_onfly_primes[i].qp;
            uint32_t pr = dense_onfly_primes[i].pr;
            uint64_t k = onfly_k_[i];
            int j = onfly_j_[i];
            while (k < k_high) {
                uint64_t idx = k - k_low;
                words_[idx >> 6] |= (1ULL << (idx & 63));
                k += qp * GAP_K[j] + ONFLY_CORRECTION[pr][j];
                if constexpr (WHEEL_SIZE_IS_POW2) {
                    j = (j + 1) & (WHEEL_SIZE - 1);
                } else {
                    ++j;
                    if (j == WHEEL_SIZE) j = 0;
                }
            }
            onfly_k_[i] = k;
            onfly_j_[i] = j;
        }

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

    // Processes exactly the sparse-tier entries due this segment: mark,
    // advance, reschedule into whichever future bucket the next hit lands
    // in. Each activated sparse prime owns exactly one permanent slot in
    // sparse_m_/sparse_next_ for the rest of the chunk (see the pool
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
            // Attempt 4 (see the tier's header comment for 1-3): stores the
            // multiplier m itself across segments instead of k, so there's
            // no division by p to recover it anymore -- k is derived from m
            // via wheel_index (p*m, then a divide by the compile-time
            // constant WHEEL_MOD, which the compiler turns into a
            // multiply-shift) instead of the other way around. Same 8
            // bytes/prime as the k it replaces -- no memory-footprint
            // growth, unlike attempt 3's per-prime qp/pr fields.
            uint64_t p = sparse_primes[idx];
            uint64_t m = sparse_m_[idx];
            uint64_t k = wheel_index(p * m);
            while (k < k_high) {
                uint64_t widx = k - k_low;
                words_[widx >> 6] |= (1ULL << (widx & 63));
                // m is already coprime with WHEEL_MOD (it's the
                // multiplier of the hit just marked), so
                // STEP_TO_COPRIME[m % WHEEL_MOD] alone is 0 -- that
                // table answers "distance to the *nearest* coprime
                // residue", which is where you already are. Step past
                // it first so it finds the *next* one instead of
                // stalling on this one forever.
                ++m;
                uint64_t step = STEP_TO_COPRIME[m % WHEEL_MOD];
                m += step;
                k = wheel_index(p * m);
            }
            sparse_m_[idx] = m;
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
    // No allocation here, ever, after activation: idx's slot in sparse_m_/
    // sparse_next_ already exists (see the pool comment near their
    // declaration) -- this just relinks it onto the target ring slot's
    // list (sparse_m_[idx] is the caller's job to update first -- k here
    // is only used to place the slot, never stored, since sparse_m_ is now
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
    const Presieve& presieve_;

    // Flat tiers' per-prime state, indexed in lockstep with
    // wheel_base_primes/dense_onfly_primes as they activate (see
    // sieve_and_emit's activation loops) -- no bucket, walked every
    // segment.
    std::vector<uint64_t> dense_k_;
    std::vector<int> dense_j_;
    std::vector<uint64_t> onfly_k_;
    std::vector<int> onfly_j_;

    // Sparse tier's bucket ring, as an intrusive linked list over a flat
    // pool instead of one std::vector<Entry> per ring slot: sparse_m_/
    // sparse_next_ grow in lockstep with next_sparse_idx_ as primes
    // activate (mirroring dense_k_/onfly_k_ above), so sparse_m_[i] is
    // always prime i's current multiplier m (see process_sparse_bucket's
    // attempt-4 comment for why m, not k) and sparse_next_[i] chains it
    // into whichever ring slot's list it's currently scheduled on.
    // bucket_head_[slot] is that list's head index, or NPOS if empty. A
    // prime is relinked (schedule_sparse) every time it's processed, but
    // its slot in sparse_m_/sparse_next_ is allocated exactly once, at
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
    // The multiplier m, not k -- see process_sparse_bucket's attempt-4
    // comment for why storing this instead of k removes the division by p
    // that used to be needed to recover it, at no memory cost (same 8
    // bytes/prime either way).
    std::vector<uint64_t> sparse_m_;
    std::vector<uint32_t> sparse_next_;
    uint64_t cur_segment_ = 0;

    size_t next_prime_idx_ = 0;
    size_t next_onfly_idx_ = 0;
    size_t next_sparse_idx_ = 0;
};
