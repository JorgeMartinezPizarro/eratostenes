#pragma once
// Simple (non-segmented) sieve to get the base primes up to sqrt(N).
// This range is always small (e.g. sqrt(1e11) ~ 316228), so an in-memory
// "odds only" bit sieve is more than enough.

#include <cstdint>
#include <vector>
#include <cmath>

// Exact integer square root (avoids double-rounding errors from sqrt() for
// large numbers).
inline uint64_t isqrt(uint64_t n) {
    if (n == 0) return 0;
    uint64_t s = static_cast<uint64_t>(std::sqrt(static_cast<long double>(n)));
    while (s > 0 && s * s > n) --s;
    while ((s + 1) * (s + 1) <= n) ++s;
    return s;
}

// Returns every prime <= limit.
inline std::vector<uint64_t> sieve_base_primes(uint64_t limit) {
    std::vector<uint64_t> primes;
    if (limit < 2) return primes;
    primes.push_back(2);
    if (limit < 3) return primes;

    // is_composite[i] represents the odd number (2*i + 3)
    uint64_t count = (limit - 1) / 2; // how many odds >=3 there are up to limit
    std::vector<bool> is_composite(count, false);

    for (uint64_t i = 0; i < count; ++i) {
        if (is_composite[i]) continue;
        uint64_t p = 2 * i + 3;
        if (p * p > limit) continue;
        // first odd multiple of p to mark: p*p (already odd since p is)
        for (uint64_t n = p * p; n <= limit; n += 2 * p) {
            is_composite[(n - 3) / 2] = true;
        }
    }

    for (uint64_t i = 0; i < count; ++i) {
        if (!is_composite[i]) primes.push_back(2 * i + 3);
    }
    return primes;
}
