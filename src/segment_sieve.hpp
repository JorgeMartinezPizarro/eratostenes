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
// Persistent per-prime cursor across segments: a naive segmented sieve
// re-derives, for every (prime, segment) pair, "where is this prime's
// first multiple in this segment" via a couple of divisions. That cost is
// fine for a handful of segments, but the number of (prime, segment) pairs
// grows roughly like pi(sqrt(N)) * N (segment count grows linearly with N,
// while the number of active primes grows with sqrt(N)) -- it outgrows the
// actual marking work (which grows closer to N*log(log(N))) as N gets
// large, and ends up dominating the run time at N in the 1e12+ range.
//
// Instead, each prime's wheel-index position and phase (k, j) is computed
// ONCE per prime per thread chunk (see begin_chunk), and carried forward
// from one sieve_and_emit call to the next as a member Cursor: advancing
// it a segment at a time is pure addition, no division anywhere. This is
// only safe because a single SegmentSieve instance is used, by one thread,
// for a whole contiguous run of segments in increasing k order (see
// sieve_chunk in main.cpp) -- never reused across threads or out of order.
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

    // Must be called once, before the first sieve_and_emit call, with the
    // wheel index the upcoming run of segments will start at. Sets up each
    // prime's persistent cursor at its first relevant multiple -- the only
    // place in this class that divides.
    void begin_chunk(uint64_t chunk_k_low,
                      const std::vector<WheelBasePrime>& wheel_base_primes) {
        uint64_t low_n = wheel_number(chunk_k_low);
        cursors_.resize(wheel_base_primes.size());
        for (size_t i = 0; i < wheel_base_primes.size(); ++i) {
            uint64_t p = wheel_base_primes[i].p;

            // Smallest m coprime with WHEEL_MOD such that p*m >= max(p*p, low_n).
            uint64_t start_val = std::max(p * p, low_n);
            uint64_t m = (start_val + p - 1) / p;
            uint64_t r = m % WHEEL_MOD;
            uint64_t step = STEP_TO_COPRIME[r];
            m += step;
            r += step;
            if (r >= WHEEL_MOD) r -= WHEEL_MOD;

            cursors_[i].k = wheel_index(p * m);
            cursors_[i].j = WHEEL_POS[r];
        }
    }

    template <typename Writer>
    void sieve_and_emit(uint64_t k_low, uint64_t k_high,
                         const std::vector<WheelBasePrime>& wheel_base_primes,
                         Writer& out, uint64_t& prime_count) {
        uint64_t count = (k_high > k_low) ? (k_high - k_low) : 0;
        if (count == 0) return;

        size_t words_needed = (count + 63) / 64;
        std::fill(words_.begin(), words_.begin() + words_needed, 0ULL);

        uint64_t high_n = wheel_number(k_high); // exclusive numeric bound, valid for the p*p cutoff

        for (size_t i = 0; i < wheel_base_primes.size(); ++i) {
            uint64_t p = wheel_base_primes[i].p;
            if (p * p >= high_n) break; // wheel_base_primes is sorted by p

            Cursor& c = cursors_[i];
            const auto& delta = wheel_base_primes[i].delta;

            // k_high is the exact wheel-index bound: no need to track "n"
            // inside the loop (see header comment), just advance k with the
            // precomputed jump table. c.k/c.j carry over to the next
            // segment untouched when this loop runs zero times (prime not
            // yet active in this segment) or stops mid-segment.
            while (c.k < k_high) {
                uint64_t idx = c.k - k_low;
                words_[idx >> 6] |= (1ULL << (idx & 63));
                c.k += delta[c.j];
                ++c.j;
                if (c.j == WHEEL_SIZE) c.j = 0; // avoid a real division (WHEEL_SIZE isn't a power of 2)
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
                uint64_t step = bit_pos - prev_bit;
                prev_bit = bit_pos;
                r += step;
                while (r >= static_cast<uint64_t>(WHEEL_SIZE)) { r -= WHEEL_SIZE; ++q; }

                uint64_t value = q * WHEEL_MOD + WHEEL_R[r];
                out.write_uint64(value);
                ++prime_count;
                bits &= bits - 1; // clear the lowest set bit
            }
        }
    }

private:
    struct Cursor {
        uint64_t k;
        int j;
    };

    std::vector<uint64_t> words_;
    std::vector<Cursor> cursors_; // parallel to wheel_base_primes, set by begin_chunk
};
