#pragma once
// Segmented sieve on a mod-2310 wheel (2,3,5,7,11), bit-packed into
// uint64_t words.
//
// Each segment covers a range of "wheel indices" [k_low, k_high): bit i is
// 1 if wheel_number(k_low + i) is COMPOSITE, 0 if it is a prime candidate.
// The numbers 2, 3, 5, 7 and 11 don't take part in this numbering (emitted
// separately by the caller); wheel_base_primes must include every prime
// <= sqrt(high) except those five, each with its jump table already
// computed (see wheel.hpp) -- computed once per prime, not once per
// segment.
//
// A persistent per-prime cursor across segments (carrying each prime's
// wheel-index position forward instead of re-deriving it every segment)
// was tried and measured *worse* at N=1e12 (1102s vs 970s here): at that
// scale the per-prime jump table (see wheel.hpp) is far bigger than the
// CPU's L3 cache (e.g. ~151MB vs 12MB for base primes up to sqrt(1e12)),
// so the dominant cost is memory bandwidth to stream that shared table,
// not the couple of divisions this class does per (prime, segment) pair.
// The cursor added its own per-thread memory traffic without addressing
// that, so it lost. Kept simple/stateless here on purpose; the real lever
// for N in the 1e12+ range is shrinking that per-prime table (a smaller
// wheel, or a structure shared across primes) rather than removing
// divisions -- see README.
//
// Marking multiples without division (or multiplication) in the inner
// loop: the wheel index k=wheel_index(p*m) is advanced directly using each
// prime's precomputed delta table (see compute_wheel_deltas in wheel.hpp)
// up to the segment's k_high bound; there is no need to reconstruct n=p*m
// at every step.
//
// Extraction: each word is decomposed into (q, r) = (k / WHEEL_SIZE,
// k % WHEEL_SIZE) once per word (one division, amortized over up to 64
// primes), and then advanced bit by bit with a bounded wrap-around
// increment -- no further division anywhere in the hot path.
//
// Bits are inverted and walked with ctz + clear-lowest-bit, which is
// faster than testing every bit individually.

#include <cstdint>
#include <vector>
#include <algorithm>

#include "wheel.hpp"

class SegmentSieve {
public:
    explicit SegmentSieve(uint64_t max_wheel_count) {
        size_t words = (max_wheel_count + 63) / 64;
        words_.assign(words, 0);
    }

    template <typename Writer>
    void sieve_and_emit(uint64_t k_low, uint64_t k_high,
                         const std::vector<WheelBasePrime>& wheel_base_primes,
                         Writer& out, uint64_t& prime_count) {
        uint64_t count = (k_high > k_low) ? (k_high - k_low) : 0;
        if (count == 0) return;

        size_t words_needed = (count + 63) / 64;
        std::fill(words_.begin(), words_.begin() + words_needed, 0ULL);

        uint64_t low_n = wheel_number(k_low);
        uint64_t high_n = wheel_number(k_high); // exclusive numeric bound, valid for the p*p cutoff

        for (const auto& wp : wheel_base_primes) {
            uint64_t p = wp.p;
            if (p * p >= high_n) break; // wheel_base_primes is sorted by p

            // Find the smallest m coprime with WHEEL_MOD such that
            // p*m >= max(p*p, low_n). One division to get m's residue, one
            // table lookup to jump straight to the next coprime value --
            // no per-step search loop.
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;
            int j = WHEEL_POS[r];

            uint64_t k = wheel_index(p * m);
            const auto& delta = wp.delta;

            // k_high is the exact wheel-index bound: no need to track "n"
            // inside the loop (see header comment), just advance k with the
            // precomputed jump table.
            while (k < k_high) {
                uint64_t idx = k - k_low;
                words_[idx >> 6] |= (1ULL << (idx & 63));
                k += delta[j];
                ++j;
                if (j == WHEEL_SIZE) j = 0; // avoid a real division (WHEEL_SIZE isn't a power of 2)
            }
        }

        // Extraction: bit=0 => prime candidate.
        for (size_t w = 0; w < words_needed; ++w) {
            uint64_t bits = ~words_[w];
            uint64_t base_idx = w * 64ULL;
            uint64_t remaining = count - base_idx;
            if (remaining < 64) {
                bits &= (remaining == 0) ? 0ULL : ((1ULL << remaining) - 1ULL);
            }
            if (bits == 0) continue;

            // One real division per word to locate its (q, r) decomposition;
            // amortized over up to 64 primes below.
            uint64_t k_word_start = k_low + base_idx;
            uint64_t q = k_word_start / WHEEL_SIZE;
            uint64_t r = k_word_start % WHEEL_SIZE;

            uint64_t prev_bit = 0;
            while (bits) {
                uint64_t bit_pos = static_cast<uint64_t>(__builtin_ctzll(bits));
                uint64_t step2 = bit_pos - prev_bit;
                prev_bit = bit_pos;
                r += step2;
                while (r >= static_cast<uint64_t>(WHEEL_SIZE)) { r -= WHEEL_SIZE; ++q; }

                uint64_t value = q * WHEEL_MOD + WHEEL_R[r];
                out.write_uint64(value);
                ++prime_count;
                bits &= bits - 1; // clear the lowest set bit
            }
        }
    }

private:
    std::vector<uint64_t> words_;
};
