#pragma once
// The mod-30 wheel: the sieve only represents numbers coprime to 2, 3 and 5,
// the 8 residues 1, 7, 11, 13, 17, 19, 23, 29 of every 30, so one byte of
// the bit array is exactly 30 consecutive integers. A wheel index k numbers
// those candidates (k = 0 is 1, k = 1 is 7, ...); wheel_number() and
// wheel_index() convert. The layout is fixed: the dense tiers' masks and
// offsets (erat_small.hpp) and the multiplier tables (wheel210_big.hpp) are
// derived for it. Bigger wheels (mod 210, 2310) were measured before the
// tiered design and lost; see docs/RESEARCH.md, wheel.hpp section. The
// multiples of 7 and 11 are skipped by the multiplier wheels instead.

#include <cstdint>
#include <array>

constexpr std::array<uint64_t, 3> WHEEL_PRIMES = {2, 3, 5};

constexpr uint64_t wheel_gcd(uint64_t a, uint64_t b) {
    while (b != 0) {
        uint64_t t = b;
        b = a % b;
        a = t;
    }
    return a;
}

constexpr uint64_t compute_wheel_mod() {
    uint64_t m = 1;
    for (uint64_t p : WHEEL_PRIMES) m *= p;
    return m;
}
constexpr uint64_t WHEEL_MOD = compute_wheel_mod();

constexpr int compute_wheel_size() {
    // WHEEL_MOD is squarefree (product of distinct primes), so
    // phi(WHEEL_MOD) = product(p - 1) over its prime factors.
    uint64_t phi = 1;
    for (uint64_t p : WHEEL_PRIMES) phi *= (p - 1);
    return static_cast<int>(phi);
}
constexpr int WHEEL_SIZE = compute_wheel_size();

static_assert(WHEEL_MOD == 30 && WHEEL_SIZE == 8, "the byte layout is one byte = 30 numbers (mod 30)");
constexpr int WHEEL_SIZE_LOG2 = 3; // k / 8 and k % 8 as a shift and a mask

constexpr bool is_prime_trial(uint64_t n) {
    if (n < 2) return false;
    for (uint64_t d = 2; d * d <= n; ++d) {
        if (n % d == 0) return false;
    }
    return true;
}

// The smallest prime not covered by the wheel -- the first "base prime"
// that actually needs marking (below it, primes are emitted directly).
constexpr uint64_t compute_first_wheel_prime() {
    uint64_t c = WHEEL_PRIMES.back() + 1;
    while (!is_prime_trial(c)) ++c;
    return c;
}
constexpr uint64_t FIRST_WHEEL_PRIME = compute_first_wheel_prime();

// The WHEEL_SIZE residues in [1, WHEEL_MOD) coprime with WHEEL_MOD, sorted.
constexpr std::array<uint64_t, WHEEL_SIZE> make_wheel_r() {
    std::array<uint64_t, WHEEL_SIZE> r{};
    int idx = 0;
    for (uint64_t x = 1; x < WHEEL_MOD; ++x) {
        if (wheel_gcd(x, WHEEL_MOD) == 1) r[idx++] = x;
    }
    return r;
}
constexpr std::array<uint64_t, WHEEL_SIZE> WHEEL_R = make_wheel_r();

// Gap from WHEEL_R[j] to the next candidate (WHEEL_R[(j+1) % WHEEL_SIZE],
// adding WHEEL_MOD when wrapping past the end of the period).
constexpr std::array<uint64_t, WHEEL_SIZE> make_wheel_gap() {
    std::array<uint64_t, WHEEL_SIZE> gap{};
    for (int i = 0; i < WHEEL_SIZE; ++i) {
        uint64_t next = (i + 1 < WHEEL_SIZE) ? WHEEL_R[i + 1] : (WHEEL_R[0] + WHEEL_MOD);
        gap[i] = next - WHEEL_R[i];
    }
    return gap;
}
constexpr std::array<uint64_t, WHEEL_SIZE> WHEEL_GAP = make_wheel_gap();

// WHEEL_POS[r] = position of r within WHEEL_R (0..WHEEL_SIZE-1), or -1 if r
// is not coprime with WHEEL_MOD. Table generated at compile time.
constexpr std::array<int, WHEEL_MOD> make_wheel_pos() {
    std::array<int, WHEEL_MOD> pos{};
    for (auto& v : pos) v = -1;
    for (int j = 0; j < WHEEL_SIZE; ++j) pos[WHEEL_R[j]] = j;
    return pos;
}
constexpr std::array<int, WHEEL_MOD> WHEEL_POS = make_wheel_pos();

// STEP_TO_COPRIME[r] = smallest s >= 0 such that (r+s) % WHEEL_MOD is
// coprime with WHEEL_MOD. Lets callers jump straight to the next wheel
// candidate with a single lookup instead of a division-per-step search
// loop (used once per base prime per segment, not in any hot inner loop).
constexpr std::array<uint32_t, WHEEL_MOD> make_step_to_coprime() {
    std::array<uint32_t, WHEEL_MOD> step{};
    for (uint64_t r = 0; r < WHEEL_MOD; ++r) {
        uint32_t s = 0;
        while (WHEEL_POS[(r + s) % WHEEL_MOD] < 0) ++s;
        step[r] = s;
    }
    return step;
}
constexpr std::array<uint32_t, WHEEL_MOD> STEP_TO_COPRIME = make_step_to_coprime();

// The k-th number coprime with WHEEL_MOD (k=0 -> 1, k=1 -> the next one, ...).
inline uint64_t wheel_number(uint64_t k) {
    uint64_t q = k / WHEEL_SIZE;
    uint64_t j = k % WHEEL_SIZE;
    return q * WHEEL_MOD + WHEEL_R[j];
}

// Wheel index of n (n MUST be coprime with WHEEL_MOD).
inline uint64_t wheel_index(uint64_t n) {
    uint64_t q = n / WHEEL_MOD;
    uint64_t r = n % WHEEL_MOD;
    return q * WHEEL_SIZE + static_cast<uint64_t>(WHEEL_POS[r]);
}

// Counts how many numbers coprime with WHEEL_MOD are in [1, limit]
// (including 1). Equivalent to the (exclusive) wheel index of the first
// number above limit, i.e. an exclusive upper bound for k when sieving up
// to limit.
inline uint64_t wheel_count_upto(uint64_t limit) {
    uint64_t q = limit / WHEEL_MOD;
    uint64_t r = limit % WHEEL_MOD;
    uint64_t partial = 0;
    for (int j = 0; j < WHEEL_SIZE; ++j) {
        if (WHEEL_R[j] <= r) ++partial;
    }
    return q * WHEEL_SIZE + partial;
}

// The wheel-index advance when a multiplier of p moves from phase jj to
// phase jj+1 (n grows by p*WHEEL_GAP[jj]); p_mod is p % WHEEL_MOD. Only
// used to build presieve tables (compute_wheel_deltas) -- the marking
// tiers step with erat_small.hpp's constant offsets (small, med64) or
// wheel210_big.hpp's mod-210 tables (medium, sparse).
inline uint64_t wheel_delta_at(uint64_t p, uint64_t p_mod, int jj) {
    uint64_t rp = (p_mod * WHEEL_R[jj]) % WHEEL_MOD;
    uint64_t d = p * WHEEL_GAP[jj];
    uint64_t floor_term = (rp + d) / WHEEL_MOD;
    int pos_before = WHEEL_POS[rp];
    int pos_after = WHEEL_POS[(rp + d) % WHEEL_MOD];
    // floor_term*WHEEL_SIZE always dominates (pos_after-pos_before)
    // (range -(WHEEL_SIZE-1)..(WHEEL_SIZE-1)), so the result is always >= 0.
    int64_t signed_delta = static_cast<int64_t>(floor_term) * WHEEL_SIZE
                          + (pos_after - pos_before);
    return static_cast<uint64_t>(signed_delta);
}

// Per-phase wheel-index advances for p (presieve table construction only).
inline std::array<uint32_t, WHEEL_SIZE> compute_wheel_deltas(uint64_t p) {
    std::array<uint32_t, WHEEL_SIZE> delta{};
    uint64_t pmod = p % WHEEL_MOD;
    for (int jj = 0; jj < WHEEL_SIZE; ++jj) {
        delta[jj] = static_cast<uint32_t>(wheel_delta_at(p, pmod, jj));
    }
    return delta;
}
