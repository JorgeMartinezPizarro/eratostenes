#pragma once
// Segmented sieve on a compile-time wheel (see wheel.hpp), bit-packed into
// uint64_t words, using a bucket sieve to mark multiples.
//
// Each base prime is scheduled into the "bucket" of the future segment
// where its next multiple falls (a fixed-size ring, buckets_). Processing
// a segment means looking at only its own bucket: mark, then reschedule
// each prime into whichever future bucket its next hit belongs to. Buckets
// are indexed by segment number modulo the ring size, which only works
// because no prime's multiples can ever be more than num_buckets_ segments
// apart -- see the constructor for how that bound is computed and
// margined, and schedule() for the runtime check that would catch it
// (loudly) if that bound were ever wrong.
//
// Newly-relevant primes (p*p just crossed into range) are picked up by a
// single monotonically-advancing pointer into wheel_base_primes (sorted by
// p) -- each prime is activated exactly once per thread chunk.
//
// Extraction (turning the finished bit array into actual prime values):
// invert each word, decompose into (q, r) = (k / WHEEL_SIZE, k %
// WHEEL_SIZE) once per word, then walk set bits with ctz + clear-lowest-bit.

#include <cstdint>
#include <vector>
#include <algorithm>
#include <stdexcept>

#include "wheel.hpp"

class SegmentSieve {
public:
    // seg_k_width: wheel-index width of a normal (non-final) segment, same
    // as passed to sieve_and_emit's k_high-k_low in every call but the
    // last of a chunk.
    // base_prime_max: the largest base prime this sieve will ever be given
    // (i.e. isqrt(limit)) -- used to size the bucket ring generously
    // enough that no prime's skip between hits can ever wrap around it.
    SegmentSieve(uint64_t seg_k_width, uint64_t base_prime_max)
        : words_((seg_k_width + 63) / 64, 0),
          seg_k_width_(seg_k_width) {
        uint64_t max_gap = 0;
        for (uint64_t g : WHEEL_GAP) max_gap = std::max(max_gap, g);
        // Upper bound on delta[] (see compute_wheel_deltas in wheel.hpp):
        // floor_term*WHEEL_SIZE + (WHEEL_SIZE-1), floor_term <= (d+WHEEL_MOD-1)/WHEEL_MOD
        // with d = p*max_gap. A few extra WHEEL_SIZE's of slack cost
        // nothing (buckets are cheap) and keep this comfortably safe.
        uint64_t d = base_prime_max * max_gap;
        uint64_t delta_max = (d / WHEEL_MOD + 1) * static_cast<uint64_t>(WHEEL_SIZE) + 2 * static_cast<uint64_t>(WHEEL_SIZE);
        uint64_t segments_ahead_max = delta_max / seg_k_width_ + 2;
        num_buckets_ = 1;
        while (num_buckets_ < (segments_ahead_max + 1) * 4) num_buckets_ <<= 1; // power of 2, 4x margin
        buckets_.resize(num_buckets_);
    }

    // Must be called once before the first sieve_and_emit call for a new,
    // independent run of consecutive segments in increasing k order (a
    // thread's chunk). Resets all bucket state and the "which primes have
    // activated yet" pointer. Never reuse a SegmentSieve across threads or
    // out of order.
    void begin_chunk() {
        cur_segment_ = 0;
        next_prime_idx_ = 0;
        for (auto& b : buckets_) b.clear();
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

        // Activate any base primes that just became relevant (p*p < high_n).
        // wheel_base_primes is sorted by p, so a single pointer that only
        // ever moves forward is enough: each prime is visited here exactly
        // once for the whole chunk, not once per segment.
        if (next_prime_idx_ < wheel_base_primes.size()) {
            uint64_t low_n = wheel_number(k_low);
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
                int j = WHEEL_POS[r];
                uint64_t k = wheel_index(p * m);

                schedule(k, j, static_cast<uint32_t>(next_prime_idx_), k_high);
                ++next_prime_idx_;
            }
        }

        // Process exactly the entries due this segment: mark, advance,
        // reschedule into whichever future bucket the next hit lands in.
        Bucket& bucket = buckets_[cur_segment_ & (num_buckets_ - 1)];
        for (const Entry& e : bucket) {
            const auto& delta = wheel_base_primes[e.prime_idx].delta;
            uint64_t k = e.k;
            int j = e.j;
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
            schedule(k, j, e.prime_idx, k_high);
        }
        bucket.clear();
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
    struct Entry {
        uint64_t k;
        int j;
        uint32_t prime_idx; // index into wheel_base_primes
    };
    using Bucket = std::vector<Entry>;

    // Schedules (k, j, prime_idx) into the bucket for whichever segment k
    // falls into, given that the segment currently being processed ends at
    // k_high. k < k_high means "due right now" (this segment's bucket);
    // asserts rather than silently corrupting results if the ring ever
    // turns out too small for the actual data (see constructor).
    void schedule(uint64_t k, int j, uint32_t prime_idx, uint64_t k_high) {
        uint64_t segments_ahead = (k < k_high) ? 0 : (k - k_high) / seg_k_width_ + 1;
        if (segments_ahead >= num_buckets_) {
            throw std::runtime_error(
                "bucket sieve: salto de un primo mayor que el margen del anillo de cubos "
                "(bug de dimensionamiento en el constructor de SegmentSieve)");
        }
        buckets_[(cur_segment_ + segments_ahead) & (num_buckets_ - 1)].push_back({k, j, prime_idx});
    }

    std::vector<uint64_t> words_;
    uint64_t seg_k_width_;
    uint64_t num_buckets_ = 1;
    std::vector<Bucket> buckets_;
    uint64_t cur_segment_ = 0;
    size_t next_prime_idx_ = 0;
};
