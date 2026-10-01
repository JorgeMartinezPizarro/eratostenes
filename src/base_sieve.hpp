#pragma once
// The base primes up to sqrt(N), with a small segmented sieve of their own.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Exact integer square root (avoids double-rounding errors from sqrt() for
// large numbers).
inline uint64_t isqrt(uint64_t n) {
    if (n == 0) return 0;
    uint64_t s = static_cast<uint64_t>(std::sqrt(static_cast<long double>(n)));
    while (s > 0 && s * s > n) --s;
    while ((s + 1) * (s + 1) <= n) ++s;
    return s;
}

// Upper bound on pi(x), only used to reserve() the result below: Dusart
// (1999), pi(x) <= x/ln x * (1 + 1/ln x + 2.51/ln^2 x) for x >= 355991, and
// Rosser-Schoenfeld's pi(x) < 1.25506 x/ln x below that. Within ~0.05% at
// 1e9, so the 406 MB of base primes at N = 1e18 are written once, never
// reallocated.
inline uint64_t prime_count_upper_bound(uint64_t x) {
    if (x < 17) return 7; // pi(16) = 6
    const double lx = std::log(static_cast<double>(x));
    const double xd = static_cast<double>(x);
    const double b = x >= 355991 ? xd / lx * (1 + 1 / lx + 2.51 / (lx * lx)) : 1.25506 * xd / lx;
    return static_cast<uint64_t>(b) + 1;
}

// Returns every prime <= limit, in increasing order. The odd numbers 3..limit
// (index i = number 2i+3) are sieved one L1-sized window at a time, one byte
// per number, by the odd primes up to isqrt(limit) -- at most 65535 for any
// 64-bit N, found with a plain sieve -- each carrying its next multiple from
// window to window. limit is isqrt(N): 1e9 at N = 1e18, where the one-shot
// vector<bool> sieve this replaces took 7.8s on one thread (dev PC) before
// any worker started; it dominated the top-of-range tails (BENCHMARK.md).
inline std::vector<uint64_t> sieve_base_primes(uint64_t limit) {
    std::vector<uint64_t> primes;
    if (limit < 2) return primes;
    primes.reserve(prime_count_upper_bound(limit));
    primes.push_back(2);
    if (limit < 3) return primes;

    // Odd sieving primes up to sqrt(limit); small_comp[i] is the odd number 2i+1.
    const uint64_t root = isqrt(limit);
    std::vector<uint8_t> small_comp(root / 2 + 1, 0);
    std::vector<uint64_t> sp, next; // next: index of the prime's next odd multiple to cross off
    for (uint64_t p = 3; p <= root; p += 2) {
        if (small_comp[p / 2]) continue;
        sp.push_back(p);
        next.push_back((p * p - 3) / 2);
        for (uint64_t m = p * p; m <= root; m += 2 * p) small_comp[m / 2] = 1;
    }

    constexpr uint64_t WINDOW = 32 * 1024;
    std::vector<uint8_t> comp(WINDOW);
    const uint64_t count = (limit - 1) / 2; // odd numbers 3..limit
    for (uint64_t lo = 0; lo < count; lo += WINDOW) {
        const uint64_t len = std::min(WINDOW, count - lo);
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
                primes.push_back(2 * (lo + k + static_cast<uint64_t>(__builtin_ctzll(z)) / 8) + 3);
                z &= z - 1;
            }
        }
        for (; k < len; ++k)
            if (!comp[k]) primes.push_back(2 * (lo + k) + 3);
    }
    return primes;
}
