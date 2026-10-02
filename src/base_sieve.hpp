#pragma once
// The base primes up to sqrt(N), with a small segmented sieve of their own,
// kept as a bitmap on the wheel (BasePrimes).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "wheel.hpp"

// Exact integer square root (avoids double-rounding errors from sqrt() for
// large numbers). Compared by division: (s+1)*(s+1) wraps to 0 at s+1 = 2^32,
// i.e. for any n from (2^32-1)^2 up.
inline uint64_t isqrt(uint64_t n) {
    if (n == 0) return 0;
    uint64_t s = static_cast<uint64_t>(std::sqrt(static_cast<long double>(n)));
    while (s > 0 && s > n / s) --s;
    while (s + 1 <= n / (s + 1)) ++s;
    return s;
}

constexpr uint64_t BASE_SIEVE_WINDOW = 32 * 1024;

// The odd numbers with index [lo, hi) (index i = number 2i+3; lo a multiple
// of BASE_SIEVE_WINDOW) sieved one window at a time, one byte per number, by
// the odd primes sp (all of them up to sqrt of the largest), each carrying its
// next multiple from window to window; emit(p) gets every prime found, in
// increasing order.
template <typename Emit>
inline void sieve_odd_range(uint64_t lo, uint64_t hi, const std::vector<uint64_t>& sp, Emit&& emit) {
    std::vector<uint64_t> next(sp.size()); // index of each prime's next odd multiple to cross off
    for (size_t j = 0; j < sp.size(); ++j) {
        const uint64_t p = sp[j];
        uint64_t k = (p * p - 3) / 2;
        if (k < lo) k += (lo - k + p - 1) / p * p;
        next[j] = k;
    }
    std::vector<uint8_t> comp(BASE_SIEVE_WINDOW);
    for (; lo < hi; lo += BASE_SIEVE_WINDOW) {
        const uint64_t len = std::min(BASE_SIEVE_WINDOW, hi - lo);
        std::fill_n(comp.data(), len, uint8_t{0});
        for (size_t j = 0; j < sp.size(); ++j) {
            uint64_t k = next[j];
            if (k >= lo + len) continue; // p*p (or its next multiple) lies past this window
            const uint64_t p = sp[j];
            for (k -= lo; k < len; k += p) comp[k] = 1;
            next[j] = lo + k;
        }
        // Zero bytes are primes, eight at a time: ~w keeps bit 0 of each zero
        // byte, walked with ctz.
        uint64_t k = 0;
        for (; k + 8 <= len; k += 8) {
            uint64_t w;
            std::memcpy(&w, comp.data() + k, 8);
            uint64_t z = ~w & 0x0101010101010101ULL;
            while (z) {
                emit(2 * (lo + k + static_cast<uint64_t>(__builtin_ctzll(z)) / 8) + 3);
                z &= z - 1;
            }
        }
        for (; k < len; ++k)
            if (!comp[k]) emit(2 * (lo + k) + 3);
    }
}

// The sparse tier's primes (main.cpp's classify): the run [k_begin, k_end) of
// a BasePrimes bitmap, which SegmentSieve's activation walks in increasing
// order.
struct SparsePrimes {
    const uint64_t* words = nullptr;
    uint64_t k_begin = 0;
    uint64_t k_end = 0;
    uint64_t count = 0; // primes in the run
    bool empty() const { return count == 0; }
    uint64_t size() const { return count; }
};

// Every prime <= limit (= isqrt(N)), as a bitmap on the wheel: bit k is set
// when wheel_number(k) is a prime, for every k with wheel_number(k) <= limit.
// WHEEL_SIZE bits per WHEEL_MOD numbers: 33 MB for limit 1e9 (N = 1e18),
// where a vector<uint64_t> of the same primes took 406 MB, and every thread's
// activation read it all. The wheel's own primes (WHEEL_PRIMES) aren't in the
// bitmap; `count` includes them.
struct BasePrimes {
    static constexpr uint64_t RANK_WORDS = 64; // words per rank block

    uint64_t limit = 0;
    uint64_t k_end = 0;         // wheel_count_upto(limit)
    uint64_t count = 0;         // every prime <= limit, the wheel's own included
    std::vector<uint64_t> bits; // (k_end + 63) / 64 words
    std::vector<uint64_t> rank; // bitmap primes before each block of RANK_WORDS words

    // Bitmap primes (the wheel's own excluded) with wheel index < k.
    uint64_t rank_k(uint64_t k) const {
        k = std::min(k, k_end);
        const uint64_t w = k >> 6, b = w / RANK_WORDS;
        uint64_t r = rank[b];
        for (uint64_t i = b * RANK_WORDS; i < w; ++i) r += static_cast<uint64_t>(__builtin_popcountll(bits[i]));
        if (k & 63) r += static_cast<uint64_t>(__builtin_popcountll(bits[w] & ((uint64_t{1} << (k & 63)) - 1)));
        return r;
    }

    // Every prime <= x, the wheel's own included.
    uint64_t count_upto(uint64_t x) const {
        uint64_t c = 0;
        for (uint64_t p : WHEEL_PRIMES) c += (p <= x && p <= limit);
        if (x < FIRST_WHEEL_PRIME) return c;
        return c + rank_k(wheel_count_upto(std::min(x, limit)));
    }

    // f(p) for every bitmap prime p in [lo, hi), in increasing order.
    template <typename F>
    void for_each(uint64_t lo, uint64_t hi, F&& f) const {
        if (hi == 0) return;
        const uint64_t k0 = lo ? wheel_count_upto(lo - 1) : 0;
        const uint64_t k1 = wheel_count_upto(std::min(hi - 1, limit));
        for (uint64_t k = k0; k < k1;) {
            const uint64_t wi = k >> 6;
            uint64_t w = bits[wi] & (~uint64_t{0} << (k & 63));
            const uint64_t word_end = std::min((wi + 1) << 6, k1);
            while (w) {
                const uint64_t kk = (wi << 6) + static_cast<uint64_t>(__builtin_ctzll(w));
                if (kk >= word_end) break;
                f(wheel_number(kk));
                w &= w - 1;
            }
            k = word_end;
        }
    }

    // The primes >= lo, for the sparse tier.
    SparsePrimes from(uint64_t lo) const {
        SparsePrimes s;
        s.words = bits.data();
        s.k_begin = std::min(lo ? wheel_count_upto(lo - 1) : 0, k_end);
        s.k_end = k_end;
        s.count = rank_k(k_end) - rank_k(s.k_begin);
        return s;
    }
};

// The base primes up to limit: sieve_odd_range over 3..limit, split into up
// to `threads` contiguous parts sieved in parallel (one part for small
// limits: fewer than 16 windows per part), each setting its primes' bits
// (an atomic OR: neighbouring parts can share a word). limit is isqrt(N): 1e9
// at N = 1e18, where the one-shot vector<bool> sieve of 2026-09 took 7.8 s on
// one thread before any worker started (docs/RESEARCH.md).
inline BasePrimes sieve_base_primes(uint64_t limit, unsigned threads = 1) {
    BasePrimes b;
    b.limit = limit;
    b.k_end = wheel_count_upto(limit);
    b.bits.assign((b.k_end + 63) / 64, 0);
    for (uint64_t p : WHEEL_PRIMES) b.count += p <= limit;

    if (limit >= 3) {
        // Odd sieving primes up to sqrt(limit) (at most 65535 for any 64-bit
        // N); small_comp[i] is the odd number 2i+1.
        const uint64_t root = isqrt(limit);
        std::vector<uint8_t> small_comp(root / 2 + 1, 0);
        std::vector<uint64_t> sp;
        for (uint64_t p = 3; p <= root; p += 2) {
            if (small_comp[p / 2]) continue;
            sp.push_back(p);
            for (uint64_t m = p * p; m <= root; m += 2 * p) small_comp[m / 2] = 1;
        }

        const uint64_t odd = (limit - 1) / 2; // odd numbers 3..limit
        const uint64_t windows = (odd + BASE_SIEVE_WINDOW - 1) / BASE_SIEVE_WINDOW;
        const uint64_t parts = std::clamp<uint64_t>(windows / 16, 1, std::max(1u, threads));
        std::vector<uint64_t> found(parts, 0);
        auto run = [&](uint64_t t) {
            const uint64_t lo = windows * t / parts * BASE_SIEVE_WINDOW;
            const uint64_t hi = std::min(odd, windows * (t + 1) / parts * BASE_SIEVE_WINDOW);
            uint64_t n = 0;
            sieve_odd_range(lo, hi, sp, [&](uint64_t p) {
                if (p < FIRST_WHEEL_PRIME) return; // the wheel's own, counted above
                const uint64_t k = wheel_index(p);
                std::atomic_ref<uint64_t>(b.bits[k >> 6]).fetch_or(uint64_t{1} << (k & 63), std::memory_order_relaxed);
                ++n;
            });
            found[t] = n;
        };
        if (parts == 1) {
            run(0);
        } else {
            std::vector<std::thread> pool;
            for (uint64_t t = 0; t < parts; ++t) pool.emplace_back(run, t);
            for (auto& th : pool) th.join();
        }
        for (uint64_t n : found) b.count += n;
    }

    b.rank.assign(b.bits.size() / BasePrimes::RANK_WORDS + 1, 0);
    uint64_t r = 0;
    for (size_t i = 0; i < b.bits.size(); ++i) {
        if (i % BasePrimes::RANK_WORDS == 0) b.rank[i / BasePrimes::RANK_WORDS] = r;
        r += static_cast<uint64_t>(__builtin_popcountll(b.bits[i]));
    }
    if (b.bits.size() % BasePrimes::RANK_WORDS == 0) b.rank[b.bits.size() / BasePrimes::RANK_WORDS] = r;
    return b;
}
