#pragma once
// Pre-sieve: a precomputed bit pattern for the smallest base primes, used
// to fill a new segment's bit array via a shifted-word copy instead of
// running their marking loop every segment.
//
// The bucket sieve (SegmentSieve) already skips a prime for any segment
// where it has no multiple -- but the smallest base primes (7, 11, 13...)
// have a multiple in *every* segment (their period is far smaller than the
// segment width), so bucket routing never skips them: their marking loop
// (read delta, advance, set bit) runs in full every single segment. That's
// real per-bit work, not scheduling overhead, so bucket routing can't
// remove it.
//
// What can: for a fixed small set of primes, the pattern of which
// wheel-indices they mark composite is periodic (period = WHEEL_SIZE *
// product(primes), since they're pairwise coprime -- proof: wheel_number(k)
// mod p only depends on k mod (p*WHEEL_SIZE), because advancing k by
// p*WHEEL_SIZE advances q = k/WHEEL_SIZE by exactly p, adding a multiple of
// p*WHEEL_MOD, which vanishes mod p). Precompute that pattern once, and
// filling a segment becomes a bulk copy from the precomputed buffer instead
// of per-prime, per-hit marking.
//
// Same diminishing-returns trade-off as the wheel itself (see wheel.hpp):
// each extra prime removes ~1/p more marking work but multiplies the
// buffer by p. Kept well within L2/L3 by design -- see PRESIEVE_PRIMES.

#include <cstdint>
#include <vector>

#include "wheel.hpp"

// Primes to fold into the pre-sieve pattern (must be coprime with
// WHEEL_MOD; any that happen to already be wheel primes for the active
// WHEEL_PRIMES config are skipped automatically in build_presieve).
// product(7,11,13,17) = 17017 -> pattern period = WHEEL_SIZE*17017 bits
// (~17KB for mod 30); comfortably cache-resident and shared read-only
// across all threads.
inline const std::vector<uint64_t> PRESIEVE_PRIMES = {7, 11, 13, 17};

struct Presieve {
    std::vector<uint64_t> primes;  // subset of PRESIEVE_PRIMES actually used
    uint64_t period_k = 0;         // WHEEL_SIZE * product(primes), in wheel-index units
    std::vector<uint64_t> words;   // period_k + max_seg_k_width + slack bits; bit=1 => composite
    // wheel_index(p) for each presieve prime p -- the one absolute position
    // where the periodic pattern is wrong (see build_presieve): it reads as
    // composite (p is, trivially, a multiple of itself) but is actually
    // prime. Every OTHER position sharing that bit mod period_k is a real
    // composite (p times a genuinely larger cofactor) and must stay marked,
    // so the buffer itself is left uncorrected; fill() patches only the
    // exact absolute position, not the periodic bit.
    std::vector<uint64_t> self_k;

    // Fills the first `count` bits of dst (word-granular, dst must have
    // room for ceil(count/64) words) with the pre-sieve pattern for the
    // segment starting at wheel-index k_low. Replaces zero-initializing
    // the segment's bit array: bucket-scheduled primes above the pre-sieve
    // depth then OR their own marks on top, same as before.
    void fill(uint64_t* dst, uint64_t k_low, uint64_t count) const {
        uint64_t words_needed = (count + 63) / 64;
        uint64_t bit_start = k_low % period_k;
        uint64_t src_word = bit_start >> 6;
        uint64_t shift = bit_start & 63;
        const uint64_t* src = words.data();
        if (shift == 0) {
            for (uint64_t i = 0; i < words_needed; ++i) dst[i] = src[src_word + i];
        } else {
            for (uint64_t i = 0; i < words_needed; ++i) {
                dst[i] = (src[src_word + i] >> shift) | (src[src_word + i + 1] << (64 - shift));
            }
        }
        // self_k is tiny (one entry per presieve prime) and only ever
        // actually falls inside k_low==0's segment, but checking
        // unconditionally is cheap and doesn't need that assumption.
        for (uint64_t sk : self_k) {
            if (sk >= k_low && sk < k_low + count) {
                uint64_t idx = sk - k_low;
                dst[idx >> 6] &= ~(1ULL << (idx & 63));
            }
        }
    }
};

// Builds the pattern for `primes` (those coprime with WHEEL_MOD and not
// already wheel primes), covering enough bits that any segment up to
// max_seg_k_width wide, starting anywhere in [0, period_k), can be filled
// by fill() with a single unwrapped shifted-word copy.
inline Presieve build_presieve(const std::vector<uint64_t>& primes, uint64_t max_seg_k_width) {
    Presieve ps;
    for (uint64_t p : primes) if (p >= FIRST_WHEEL_PRIME) ps.primes.push_back(p);

    uint64_t period_k = static_cast<uint64_t>(WHEEL_SIZE);
    for (uint64_t p : ps.primes) period_k *= p;
    ps.period_k = period_k;

    // +max_seg_k_width: any bit_start in [0, period_k) plus a segment-sized
    // window still lands inside the buffer, no wraparound needed. +128:
    // the shifted-word read touches src[i+1], one word past the last one
    // fill() logically needs.
    uint64_t total_bits = period_k + max_seg_k_width + 128;
    ps.words.assign((total_bits + 63) / 64, 0);

    // Mark every wheel-representable multiple of p, starting at m=1 (value
    // p itself), not p*p: SegmentSieve starts at p*p because on a full
    // sieve, smaller multiples are already covered by *other* base primes
    // below p -- a guarantee this buffer doesn't have, since it only ever
    // marks p in {7, 11, 13, ...}, nothing smaller. Marking from m=1 also
    // marks p itself (m=1 -> value p) at k0=wheel_index(p) -- that single
    // bit is wrong only at that one absolute position (see self_k above:
    // every later position sharing the same bit mod period_k is a genuine
    // composite and must stay marked, so it's *not* cleared here).
    for (uint64_t p : ps.primes) {
        auto delta = compute_wheel_deltas(p);
        uint64_t m = 1;
        uint64_t r = m % WHEEL_MOD;
        uint64_t step = STEP_TO_COPRIME[r];
        m += step;
        r += step;
        if (r >= WHEEL_MOD) r -= WHEEL_MOD;
        int j = WHEEL_POS[r];
        uint64_t k = wheel_index(p * m);

        while (k < total_bits) {
            ps.words[k >> 6] |= (1ULL << (k & 63));
            k += delta[j];
            if constexpr (WHEEL_SIZE_IS_POW2) {
                j = (j + 1) & (WHEEL_SIZE - 1);
            } else {
                ++j;
                if (j == WHEEL_SIZE) j = 0;
            }
        }
        ps.self_k.push_back(wheel_index(p));
    }
    return ps;
}
