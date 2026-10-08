#pragma once
// The base primes up to sqrt(N), kept as a bitmap on the wheel (BasePrimes),
// sieved straight into that bitmap with the main sieve's own pieces: the
// pre-sieve pattern and the small tier's byte-marking kernels.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "erat_small.hpp"
#include "presieve.hpp"
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

// The sparse tier's primes (tuning.hpp's classify): the run [k_begin, k_end) of
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
// against 406 MB as a list of uint64_t. The wheel's own primes
// (WHEEL_PRIMES) aren't in the bitmap; `count` includes them.
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

// Wheel indices per window of sieve_base_primes: 32 KiB of bitmap (~1M
// numbers), an L1d-sized slice like the small tier's sub-block.
constexpr uint64_t BASE_SIEVE_WINDOW_K = 32 * 1024 * 8;

// The base primes up to limit, sieved into the wheel bitmap itself, one
// 32 KiB window at a time: the pre-sieve fill marks the multiples of 7..163
// (and leaves those primes themselves alone, see Presieve::self_k), then the
// small tier's kernel (erat_small.hpp::cross_off_class) crosses off every
// prime from 167 to isqrt(limit) (at most 65,536 for any 64-bit N) from its
// square up, and each word goes into the bitmap inverted (set bit = prime).
// The windows are split into up to `threads` contiguous parts of whole words,
// sieved in parallel.
inline BasePrimes sieve_base_primes(uint64_t limit, const Presieve& presieve, unsigned threads = 1) {
    BasePrimes b;
    b.limit = limit;
    b.k_end = wheel_count_upto(limit);
    b.bits.assign((b.k_end + 63) / 64, 0);
    for (uint64_t p : WHEEL_PRIMES) b.count += p <= limit;

    if (limit >= FIRST_WHEEL_PRIME) {
        // The crossing-off primes: past the pre-sieve's largest, up to
        // isqrt(limit), from a plain odd sieve of [3, isqrt(limit)].
        const uint64_t presieve_max = presieve.self_k.empty() ? 0 : wheel_number(presieve.max_self_k);
        const uint64_t root = isqrt(limit);
        std::vector<uint64_t> sp;
        {
            std::vector<uint8_t> comp(root / 2 + 1, 0); // comp[i]: the odd number 2i+1
            for (uint64_t p = 3; p <= root; p += 2) {
                if (comp[p / 2]) continue;
                if (p > presieve_max && p >= FIRST_WHEEL_PRIME) sp.push_back(p);
                for (uint64_t m = p * p; m <= root; m += 2 * p) comp[m / 2] = 1;
            }
        }

        const uint64_t total_k = b.bits.size() * 64;
        const uint64_t windows = (total_k + BASE_SIEVE_WINDOW_K - 1) / BASE_SIEVE_WINDOW_K;
        const uint64_t parts = std::clamp<uint64_t>(windows / 4, 1, std::max(1u, threads));
        std::vector<uint64_t> found(parts, 0);
        auto run = [&](uint64_t t) {
            const uint64_t k_lo = windows * t / parts * BASE_SIEVE_WINDOW_K;
            const uint64_t k_hi = std::min(total_k, windows * (t + 1) / parts * BASE_SIEVE_WINDOW_K);
            // Each crossing-off prime's first hit at or past the part's start,
            // SegmentSieve::activate_dense's derivation, by residue class.
            std::vector<erat::DenseState> st[8];
            const uint64_t low_n = wheel_number(k_lo);
            for (uint64_t p : sp) {
                const uint64_t start_val = std::max(p * p, low_n);
                const uint64_t pr = static_cast<uint64_t>(WHEEL_POS[p % WHEEL_MOD]);
                uint64_t m = (start_val + p - 1) / p;
                uint64_t r = m % WHEEL_MOD;
                const uint64_t step = STEP_TO_COPRIME[r];
                m += step;
                r += step;
                if (r >= WHEEL_MOD) r -= WHEEL_MOD;
                const uint64_t pos = (p * m) / WHEEL_MOD - k_lo / 8;
                st[pr].push_back({static_cast<uint32_t>(((p / WHEEL_MOD) << 6) | (pr << 3) | WHEEL_POS[r]),
                                  static_cast<uint32_t>(pos)});
            }
            std::vector<uint64_t> win(BASE_SIEVE_WINDOW_K / 64);
            uint8_t* const s = reinterpret_cast<uint8_t*>(win.data());
            uint64_t n = 0;
            for (uint64_t k = k_lo; k < k_hi; k += BASE_SIEVE_WINDOW_K) {
                const uint64_t count = std::min(BASE_SIEVE_WINDOW_K, k_hi - k); // a multiple of 64
                const uint64_t bytes = count / 8;
                presieve.fill(win.data(), k, count);
                erat::cross_off_class<0>(s, bytes, st[0].data(), st[0].data() + st[0].size(), bytes);
                erat::cross_off_class<1>(s, bytes, st[1].data(), st[1].data() + st[1].size(), bytes);
                erat::cross_off_class<2>(s, bytes, st[2].data(), st[2].data() + st[2].size(), bytes);
                erat::cross_off_class<3>(s, bytes, st[3].data(), st[3].data() + st[3].size(), bytes);
                erat::cross_off_class<4>(s, bytes, st[4].data(), st[4].data() + st[4].size(), bytes);
                erat::cross_off_class<5>(s, bytes, st[5].data(), st[5].data() + st[5].size(), bytes);
                erat::cross_off_class<6>(s, bytes, st[6].data(), st[6].data() + st[6].size(), bytes);
                erat::cross_off_class<7>(s, bytes, st[7].data(), st[7].data() + st[7].size(), bytes);
                if (k == 0) win[0] |= 1; // the number 1
                uint64_t* const out = b.bits.data() + k / 64;
                for (uint64_t w = 0; w < count / 64; ++w) out[w] = ~win[w];
                if (k + count == total_k && (b.k_end & 63)) // past limit in the last word
                    out[count / 64 - 1] &= (uint64_t{1} << (b.k_end & 63)) - 1;
                for (uint64_t w = 0; w < count / 64; ++w) n += static_cast<uint64_t>(__builtin_popcountll(out[w]));
            }
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
